package app.unifiedslicer.companion.core.source.elegoo

import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.MqttLink
import app.unifiedslicer.companion.core.net.bareHost
import app.unifiedslicer.companion.core.net.el
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.mergeJson
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.source.awaitCondition
import kotlinx.serialization.json.JsonObject
import okhttp3.Request
import java.io.IOException
import kotlin.random.Random

/**
 * Elegoo Centauri Carbon 2 on the local network, as in elegoo-link and the desktop plug-in
 * elegoo_cc2_lan.py: serial from http://<ip>/system/info, then MQTT on port 1883 (user "elegoo",
 * password = access code), register, request the full status (1002) and merge 6000 pushes.
 */
class ElegooLanSource(override val account: LanPrinter, private val ctx: SourceContext) : PrinterSource {
    private val host = bareHost(account.host)
    private val code = account.secret.ifEmpty { DEFAULT_ACCESS_CODE }
    private val clientId = "1_PC_" + Random.nextInt(1000, 10000)
    private val requestId = clientId + "_req"
    private var serial = account.serial
    private var link: MqttLink? = null
    @Volatile private var status: JsonObject? = null
    @Volatile private var statusAt = 0L

    override suspend fun refresh(): List<PrinterStatus> {
        try {
            if (serial.isEmpty()) serial = fetchSerial()
            val l = link ?: createLink().also { link = it }
            if (!l.isConnected) {
                l.ensureConnected()
                awaitCondition(8_000) { status != null }
            }
        } catch (e: AuthException) {
            throw e
        } catch (e: IOException) {
            return listOf(offline())
        }
        val s = status ?: return listOf(offline().copy(state = PrinterState.UNKNOWN))
        return listOf(ElegooStatus.toStatus(account.id, deviceId(), account.label, "Centauri Carbon 2", VIA, s, statusAt))
    }

    private fun deviceId() = serial.ifEmpty { host }
    private fun offline() = PrinterStatus(account.id, deviceId(), account.label, "Centauri Carbon 2", Brand.ELEGOO, VIA, PrinterState.OFFLINE)

    private suspend fun fetchSerial(): String {
        val url = "http://$host/system/info?X-Token=" + java.net.URLEncoder.encode(code, "UTF-8")
        val reply = try {
            ctx.http.fetchJson(Request.Builder().url(url).header("X-Token", code).build())
        } catch (e: HttpException) {
            throw IOException(e.message, e)
        }
        val err = reply.int("error_code") ?: 0
        if (err != 0) throw AuthException("The printer rejected the access code (error $err)")
        return reply.obj("system_info").str("sn") ?: throw IOException("The printer did not report a serial number")
    }

    private fun createLink(): MqttLink {
        val base = "elegoo/$serial/"
        return MqttLink(
            uri = "tcp://$host:1883",
            username = "elegoo",
            password = code,
            clientId = clientId,
            topics = listOf(base + clientId + "/api_response", base + "api_status", base + requestId + "/register_response"),
            onConnected = { l -> l.publish(base + "api_register", """{"client_id":"$clientId","request_id":"$requestId"}""") },
        ) { topic, payload -> onMessage(topic, payload) }
    }

    private fun onMessage(topic: String, payload: String) {
        val msg = parseJson(payload) ?: return
        if (topic.endsWith("/register_response")) {
            if (msg.str("error") == "ok")
                link?.publish("elegoo/$serial/$clientId/api_request", """{"id":1,"method":$M_STATUS,"params":{}}""")
            return
        }
        val result = msg.el("result") as? JsonObject ?: return
        val method = msg.int("method")
        when {
            method == M_STATUS_PUSH -> status = status?.let { mergeJson(it, result) } ?: result
            topic.endsWith("/api_response") && (method == M_STATUS || msg.int("id") == 1) -> status = result
            else -> return
        }
        statusAt = System.currentTimeMillis()
    }

    override fun close() {
        link?.close()
        link = null
    }

    private companion object {
        const val VIA = "LAN"
        const val DEFAULT_ACCESS_CODE = "123456"
        const val M_STATUS = 1002
        const val M_STATUS_PUSH = 6000
    }
}
