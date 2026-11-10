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
import app.unifiedslicer.companion.core.source.PrinterSource
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
class ElegooCloudSource(account: ElegooAccount, private val ctx: SourceContext) : PrinterSource {
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
