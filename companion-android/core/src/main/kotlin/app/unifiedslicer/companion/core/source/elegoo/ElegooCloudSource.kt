package app.unifiedslicer.companion.core.source.elegoo

import app.unifiedslicer.companion.core.model.ElegooAccount
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.LenientJson
import app.unifiedslicer.companion.core.net.arr
import app.unifiedslicer.companion.core.net.bool
import app.unifiedslicer.companion.core.net.el
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.jsonBody
import app.unifiedslicer.companion.core.net.long
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.net.fetchText
import app.unifiedslicer.companion.core.net.fileBody
import app.unifiedslicer.companion.core.net.forUploads
import app.unifiedslicer.companion.core.net.md5
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.toHex
import app.unifiedslicer.companion.core.source.PrintCapable
import app.unifiedslicer.companion.core.source.PrintJob
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SendProgress
import app.unifiedslicer.companion.core.source.hasExtension
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.Request
import java.io.IOException

/**
 * Elegoo Cloud (matrix.elegoo.com), as used by ElegooSlicer and Unified Slicer: bound printers
 * from device/list, last reported status from device/report-data/list.
 */
class ElegooCloudSource(account: ElegooAccount, private val ctx: SourceContext) : PrinterSource, PrintCapable {
    override var account: ElegooAccount = account
        private set

    override suspend fun refresh(): List<PrinterStatus> {
        val list = call("GET", "/api/v1/device-management-server/device/list")
        val devices = (list.el("data") as? kotlinx.serialization.json.JsonArray).orEmpty()
        return coroutineScope {
            devices.mapNotNull { d ->
                val serial = d.str("serialNo") ?: return@mapNotNull null
                val model = d.str("pcode").orEmpty()
                val name = d.str("deviceName") ?: model.ifEmpty { serial }
                val online = d.bool("online") ?: d.int("onlineStatus")?.let { it == 1 }
                async {
                    if (online == false) return@async PrinterStatus(account.id, serial, name, model, Brand.ELEGOO, VIA, PrinterState.OFFLINE)
                    val (report, updated) = report(serial)
                    ElegooStatus.toStatus(account.id, serial, name, model, VIA, report, updated.takeIf { it > 0 } ?: System.currentTimeMillis())
                }
            }.awaitAll()
        }
    }

    /** The cloud's last report as {group: {field: value}}, plus its newest update time. */
    private suspend fun report(serial: String): Pair<JsonObject, Long> {
        val reply = call("GET", "/api/v1/device-management-server/device/report-data/list?deviceCode=" +
                java.net.URLEncoder.encode(serial, "UTF-8"))
        return parseReport(reply.el("data"))
    }

    /**
     * Cloud print as ElegooSlicer does it: upload to Elegoo's storage, tell the printer over
     * messaging to download it (1057, progress 6006), then start it (1020).
     */
    override suspend fun print(deviceId: String, job: PrintJob, progress: SendProgress) {
        val rtm = ctx.rtm ?: throw IOException("Elegoo cloud printing is not available on this device")
        val serial = deviceId
        val name = if (hasExtension(job.name, ".gcode", ".3mf")) job.name else job.name + ".gcode"
        progress.update(0, "Checking the printer…")
        if (report(serial).first.obj("machine_status").int("status") == 2)
            throw IOException("The printer is busy with another print")

        val md5 = md5(job.file)
        val hex = md5.toHex()
        val ext = "." + name.substringAfterLast('.').lowercase()
        val serverName = account.userId.takeLast(6) + "_" + serial.takeLast(6) + "_" + hex + ext
        val before = fileCreateTime(serial, name)

        progress.update(0, "Uploading to the Elegoo cloud…")
        val md5Base64 = java.util.Base64.getEncoder().encodeToString(md5)
        val slot = call("GET", "/api/v1/device-management-server/oss/biz-entrypoint?filename=" + enc(serverName) +
                "&bucketAlias=iot-private&module=gcode&fileMd5=" + enc(md5Base64)).obj("data")
        val entry = slot.str("entrypoint") ?: throw IOException("Elegoo did not provide an upload slot")
        val url = slot.str("accessUrl") ?: throw IOException("Elegoo did not provide an upload slot")
        val size = job.file.length().coerceAtLeast(1)
        // Cloudflare in front of the bucket rejects requests without a client identity (fetchText adds ours).
        ctx.http.forUploads().fetchText(
            Request.Builder().url(entry).header("Content-MD5", md5Base64)
                .put(fileBody(job.file) { progress.update((it * 50 / size).toInt(), null) }).build()
        )

        progress.update(50, "Connecting to the printer…")
        val token = call("GET", "/api/v1/device-management-server/device/list/agora-token?source=slicer").obj("data").obj("agoraToken")
        val rtmUser = token.str("rtmUserId") ?: throw IOException("Elegoo did not return a messaging token for this account")
        val rtmToken = token.str("rtmToken") ?: throw IOException("Elegoo did not return a messaging token for this account")
        val channel = ElegooPrinterChannel.open(rtm, token.str("userId").orEmpty(), rtmUser, rtmToken, serial)
        try {
            val task = buildJsonObject { put("taskID", serial) }
            runCatching { channel.request(ElegooPrinterChannel.M_CANCEL_DOWNLOAD, task, 5) } // nothing to cancel is fine
            delay(1000)
            channel.download = null
            progress.update(50, "The printer is downloading the file…")
            channel.request(ElegooPrinterChannel.M_DOWNLOAD_FILE, buildJsonObject {
                put("filename", name); put("url", url); put("md5", hex); put("taskID", serial)
            }, 15)
            try {
                awaitDownload(channel, serial, name, before, progress)
            } catch (e: CancellationException) {
                withContext(NonCancellable) { runCatching { channel.request(ElegooPrinterChannel.M_CANCEL_DOWNLOAD, task, 5, needReply = false) } }
                throw e
            }

            progress.update(100, "Starting the print…")
            fun start(force: Boolean) = buildJsonObject {
                put("storage_media", "local")
                put("filename", name)
                putJsonObject("config") {
                    put("delay_video", false); put("printer_check", true); put("print_layout", "A")
                    put("bedlevel_force", force); putJsonArray("slot_map") {}
                }
            }
            try {
                channel.request(ElegooPrinterChannel.M_START_PRINT, start(false), 10)
            } catch (e: ElegooPrinterError) {
                if (e.code != 1026) throw e // no bed mesh saved yet: level first
                channel.request(ElegooPrinterChannel.M_START_PRINT, start(true), 10)
            }
            progress.update(100, "Print started")
        } finally {
            channel.close()
        }
    }

    private suspend fun awaitDownload(channel: ElegooPrinterChannel, serial: String, name: String, before: String?, progress: SendProgress) {
        var lastPct = -1
        var lastChange = System.currentTimeMillis()
        while (true) {
            delay(500)
            channel.download?.let { d ->
                val pct = d.int("progress") ?: 0
                if (pct != lastPct) {
                    lastPct = pct
                    lastChange = System.currentTimeMillis()
                    progress.update(50 + pct / 2, null)
                }
                when (d.int("status")) {
                    1 -> return
                    2 -> throw IOException("The printer cancelled the download")
                    3 -> throw IOException("The printer failed to download the file")
                }
            }
            if (System.currentTimeMillis() - lastChange > 60_000) {
                // No progress for a minute: the file may have landed anyway.
                val now = fileCreateTime(serial, name)
                if (now != null && now != before) return
                throw IOException("The printer reported no download progress for 60 seconds")
            }
        }
    }

    /** createTime of a file on the printer as the cloud knows it, or null. */
    private suspend fun fileCreateTime(serial: String, name: String): String? = runCatching {
        call("GET", "/api/v1/device-management-server/local-file/filename?serialNo=" + enc(serial) + "&filename=" + enc(name))
            .obj("data")?.get("createTime")?.toString()
    }.getOrNull()

    private fun enc(s: String) = java.net.URLEncoder.encode(s, "UTF-8").replace("+", "%20")

    private suspend fun call(method: String, path: String, body: JsonElement? = null): JsonElement {
        ensureFreshToken()
        return try {
            request(method, path, body, account.accessToken)
        } catch (e: AuthException) {
            renew()
            request(method, path, body, account.accessToken)
        }
    }

    private suspend fun request(method: String, path: String, body: JsonElement?, token: String?): JsonElement {
        val b = Request.Builder().url(apiBase(account.region) + path).header("Accept", "application/json")
        if (token != null) b.header("Authorization", "Bearer $token")
        b.method(method, body?.let { jsonBody(it) })
        val reply = ctx.http.fetchJson(b.build())
        // The API answers HTTP 200 with its own code.
        when (val code = reply.int("code") ?: -1) {
            0 -> return reply
            401, 403 -> throw AuthException("Elegoo sign-in expired")
            else -> throw IOException("Elegoo API: code $code ${reply.str("msg") ?: reply.str("message").orEmpty()}".trim())
        }
    }

    private suspend fun ensureFreshToken() {
        var exp = account.accessExpireMs
        if (exp in 1..9_999_999_999L) exp *= 1000 // seconds, not ms
        if (exp > 1_000_000_000_000L && exp - 60_000 < System.currentTimeMillis() && account.refreshToken.isNotEmpty()) renew()
    }

    private suspend fun renew() {
        if (account.refreshToken.isEmpty()) throw AuthException("Elegoo sign-in expired")
        val body = buildJsonObject { put("refreshToken", account.refreshToken); put("clientId", "Slicer") }
        val d = try {
            request("POST", "/api/v1/account-center-server/account-auth/token/refresh", body, account.accessToken).el("data")
        } catch (e: HttpException) {
            throw AuthException("Elegoo sign-in expired")
        } catch (e: IOException) {
            if (e is AuthException || e.message?.startsWith("Elegoo API") == true) throw AuthException("Elegoo sign-in expired")
            throw e
        }
        val token = d.str("accessToken") ?: throw AuthException("Elegoo sign-in expired")
        account = account.copy(
            accessToken = token,
            refreshToken = d.str("refreshToken") ?: account.refreshToken,
            accessExpireMs = d.long("expiresTime") ?: 0,
        )
        ctx.onAccountUpdated(account)
    }

    companion object {
        private const val VIA = "Elegoo Cloud"
        const val LOGIN_URL = "https://www.nexprint.com/account/slicer-login"

        /** Elegoo's sign-in page only issues device-API tokens to a client that presents itself as ElegooSlicer. */
        const val LOGIN_USER_AGENT = "ElegooSlicer/1.5.3.5 (dark) Mozilla/5.0 (Windows NT 10.0; Win64; x64)"

        fun apiBase(region: String) = if (region == "CN") "https://matrix.elegoo.com.cn" else "https://matrix.elegoo.com"

        fun loginUrl(deviceId: String, dark: Boolean): String =
            LOGIN_URL.toHttpUrl().newBuilder()
                .addQueryParameter("language", "en")
                .addQueryParameter("region", "US")
                .addQueryParameter("theme", if (dark) "dark" else "light")
                .addQueryParameter("deviceId", deviceId)
                .build().toString()

        /** Only Elegoo's own pages may hand the app an account. */
        fun isElegooPage(url: String?): Boolean {
            val u = runCatching { url!!.toHttpUrl() }.getOrNull() ?: return false
            if (u.scheme != "https") return false
            val host = u.host.lowercase()
            return listOf("nexprint.com", "nexprint.cn", "elegoo.com", "elegoo.com.cn").any { host == it || host.endsWith(".$it") }
        }

        /** Builds an account from the sign-in page's report.userInfo params, or null if it carries none. */
        fun accountFromLogin(params: JsonElement?, id: String): ElegooAccount? {
            fun pick(vararg keys: String) = keys.firstNotNullOfOrNull { params.str(it) }
            val token = pick("accessToken", "token") ?: return null
            val user = pick("userId", "accountId") ?: return null
            return ElegooAccount(
                id = id,
                label = pick("nickname", "email") ?: "Elegoo account",
                accessToken = token,
                refreshToken = pick("refreshToken").orEmpty(),
                accessExpireMs = params.long("accessTokenExpireTime") ?: 0,
                userId = user,
            )
        }

        /** report-data/list's data: {group: [{reportLinkKey, reportValue, updateTime}]}. */
        fun parseReport(data: JsonElement?): Pair<JsonObject, Long> {
            var newest = 0L
            val groups = (data as? JsonObject).orEmpty().mapNotNull { (group, items) ->
                val list = items as? kotlinx.serialization.json.JsonArray ?: return@mapNotNull null
                val fields = list.mapNotNull { item ->
                    val key = item.str("reportLinkKey") ?: return@mapNotNull null
                    newest = maxOf(newest, item.long("updateTime") ?: 0L)
                    val raw = item.el("reportValue")
                    // Values come as JSON-encoded strings ("2", "\"name.gcode\"", "{...}").
                    val value = (raw as? JsonPrimitive)?.takeIf { it.isString }?.let {
                        runCatching { LenientJson.parseToJsonElement(it.content) }.getOrNull() ?: it
                    } ?: raw ?: JsonPrimitive(null as String?)
                    key to value
                }.toMap()
                group to JsonObject(fields)
            }.toMap()
            return JsonObject(groups) to newest
        }
    }
}
