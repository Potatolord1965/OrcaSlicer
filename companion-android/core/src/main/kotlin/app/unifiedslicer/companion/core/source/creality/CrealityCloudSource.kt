package app.unifiedslicer.companion.core.source.creality

import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.CrealityAccount
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.Temperature
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.arr
import app.unifiedslicer.companion.core.net.asNumber
import app.unifiedslicer.companion.core.net.el
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.jsonBody
import app.unifiedslicer.companion.core.net.long
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException
import java.util.UUID

/** Creality Cloud REST API, ported from Unified Slicer's CrealityCloud client (from Creality Print 6). */
class CrealityApi(private val http: OkHttpClient, private val region: String, private val deviceUid: String) {
    private val base = if (region == "china") "https://api.crealitycloud.cn" else "https://api.crealitycloud.com"

    suspend fun post(path: String, body: JsonElement, session: CrealityAccount? = null): JsonElement =
        envelope(http.fetchJson(request(path, session).post(jsonBody(body)).build()))

    suspend fun get(path: String, session: CrealityAccount?): JsonElement =
        envelope(http.fetchJson(request(path, session).get().build()))

    private fun request(path: String, session: CrealityAccount?) = Request.Builder().url(base + path).apply {
        header("__CXY_APP_ID_", "creality_model")
        header("__CXY_PLATFORM_", "11")
        header("__CXY_OS_LANG_", "0")
        header("__CXY_OS_VER_", "win")
        header("__CXY_DUID_", deviceUid)
        header("__CXY_REQUESTID_", UUID.randomUUID().toString())
        if (session != null) {
            header("__CXY_UID_", session.userId)
            header("__CXY_TOKEN_", session.token)
        }
    }

    /** {code, msg, result}: code 0 is success, 4 an expired token. */
    private fun envelope(reply: JsonElement): JsonElement {
        // A bare telemetry map has no envelope.
        if (reply.el("code") == null) return reply
        return when (val code = reply.int("code")) {
            0 -> reply.el("result") ?: JsonObject(emptyMap())
            4 -> throw AuthException("Creality sign-in expired")
            else -> throw IOException(reply.str("msg") ?: "Creality Cloud error $code")
        }
    }

    /** [accountType]: 2 = e-mail, 1 = phone number. */
    suspend fun login(accountType: Int, account: String, password: String): CrealityAccount {
        val body = buildJsonObject { put("type", accountType); put("account", account); put("password", password) }
        val result = try {
            post("/api/cxy/account/v2/loginV2", body)
        } catch (e: IOException) {
            if (e is AuthException) throw e
            post("/api/account/loginV2", body) // older path, still answered
        }
        val token = result.str("token")
        val userId = result.str("userId")
        if (token == null || userId == null || userId == "0") throw AuthException("Creality Cloud returned no session for this account")
        return CrealityAccount(
            id = "creality-" + UUID.randomUUID(),
            label = result.obj("user_info").str("nickName") ?: account,
            region = region,
            token = token,
            userId = userId,
            deviceUid = deviceUid,
        )
    }

    companion object {
        fun newDeviceUid() = UUID.randomUUID().toString().replace("-", "")
    }
}

class CrealityCloudSource(override val account: CrealityAccount, ctx: SourceContext) : PrinterSource {
    private val api = CrealityApi(ctx.http, account.region, account.deviceUid)

    override suspend fun refresh(): List<PrinterStatus> {
        val devices = mutableListOf<JsonElement>()
        for (page in 1..10) {
            val result = api.post("/api/rest/print/cluster/devices/getDevices", buildJsonObject { put("page", page); put("pageSize", 100) }, account)
            val chunk = result.arr("list").orEmpty().filter { (it.int("type") ?: 9) in setOf(1, 9) }
            devices += chunk
            if ((result.arr("list")?.size ?: 0) < 100) break
        }
        return coroutineScope {
            devices.mapNotNull { d ->
                val dn = d.str("deviceName") ?: return@mapNotNull null
                async { status(dn, d) }
            }.awaitAll()
        }
    }

    private suspend fun status(dn: String, d: JsonElement): PrinterStatus {
        val rawModel = d.str("model").orEmpty()
        val model = if (rawModel.startsWith("CR-K1", ignoreCase = true)) rawModel.drop(3) else rawModel
        val name = d.str("aliasName") ?: model.ifEmpty { dn }
        val deviceState = d.int("deviceState") ?: -1
        val online = deviceState != -1
        if (!online) return PrinterStatus(account.id, dn, name, model, Brand.CREALITY, VIA, PrinterState.OFFLINE)
        val tbId = d.str("tbId")
        val t: JsonObject = if (tbId != null) runCatching {
            parseTelemetry(api.get("/api/rest/iotrouter/$tbId/values/timeseries?keys=&useStrictDataTypes=true", account))
        }.getOrElse { if (it is AuthException) throw it else JsonObject(emptyMap()) } else JsonObject(emptyMap())
        val info = d.obj("printInfo")
        val state = state(t.int("state") ?: deviceState)
        val printing = state.active || state == PrinterState.FINISHED
        fun temp(cur: String, target: String) = t.num(cur)?.let { Temperature(it, t.num(target)?.takeIf { x -> x > 0 }) }
        return PrinterStatus(
            accountId = account.id,
            deviceId = dn,
            name = name,
            model = model,
            brand = Brand.CREALITY,
            via = VIA,
            state = state,
            progress = (t.int("printProgress") ?: info.int("printProgress"))?.takeIf { printing },
            remainingSeconds = (t.long("printLeftTime") ?: info.long("printLeftTime"))?.takeIf { state.active && it > 0 },
            layer = t.int("layer")?.takeIf { printing },
            totalLayers = t.int("TotalLayer")?.takeIf { printing && it > 0 },
            file = (t.str("printFileName") ?: info.str("modelName"))?.takeIf { printing },
            nozzle = temp("nozzleTemp", "targetNozzleTemp"),
            bed = temp("bedTemp0", "targetBedTemp0"),
            chamber = t.num("boxTemp")?.let { Temperature(it) },
        )
    }

    companion object {
        private const val VIA = "Creality Cloud"

        /** 0 idle, 1 printing, 2 finished, 3 failed, 4 stopped, 5 paused. */
        fun state(s: Int): PrinterState = when (s) {
            1 -> PrinterState.PRINTING
            2 -> PrinterState.FINISHED
            3, 4 -> PrinterState.FAILED
            5 -> PrinterState.PAUSED
            else -> PrinterState.IDLE
        }

        /** Timeseries reply: {key: [{ts, value}]} (possibly in the result envelope) → {key: value}. */
        fun parseTelemetry(reply: JsonElement): JsonObject {
            val map = reply as? JsonObject ?: return JsonObject(emptyMap())
            return JsonObject(map.mapNotNull { (k, v) ->
                when {
                    v is JsonArray -> v.firstOrNull()?.el("value")?.let { k to it }
                    v !is JsonObject -> k to v
                    else -> null
                }
            }.toMap())
        }
    }
}
