package app.unifiedslicer.companion.core.source.bambu

import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.MqttLink
import app.unifiedslicer.companion.core.net.bareHost
import app.unifiedslicer.companion.core.net.mergeJson
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.awaitCondition
import kotlinx.serialization.json.JsonObject

/**
 * A Bambu printer in LAN mode: MQTT over TLS on port 8883, user "bblp", password = the access
 * code from the printer's screen. The printer's certificate is self-signed, so it is not verified.
 */
class BambuLanSource(override val account: LanPrinter) : PrinterSource {
    private val serial = account.serial.trim()
    @Volatile private var report: JsonObject? = null
    @Volatile private var reportAt = 0L

    private val link = MqttLink(
        uri = "ssl://${bareHost(account.host)}:8883",
        username = "bblp",
        password = account.secret,
        topics = listOf("device/$serial/report"),
        insecureTls = true,
        onConnected = { it.publish("device/$serial/request", PUSH_ALL) },
    ) { _, payload ->
        parseJson(payload).obj("print")?.let { p ->
            report = report?.let { mergeJson(it, p) } ?: p
            reportAt = System.currentTimeMillis()
        }
    }

    override suspend fun refresh(): List<PrinterStatus> {
        try {
            if (!link.isConnected) {
                link.ensureConnected()
                awaitCondition(6_000) { report != null }
            }
        } catch (e: app.unifiedslicer.companion.core.net.AuthException) {
            throw e
        } catch (e: java.io.IOException) {
            return listOf(PrinterStatus(account.id, serial, account.label, "", Brand.BAMBU, VIA, PrinterState.OFFLINE))
        }
        val r = report ?: return listOf(PrinterStatus(account.id, serial, account.label, "", Brand.BAMBU, VIA, PrinterState.UNKNOWN))
        return listOf(BambuReport.toStatus(account.id, serial, account.label, "", VIA, r, reportAt))
    }

    override fun close() = link.close()

    private companion object {
        const val VIA = "LAN"
        const val PUSH_ALL = """{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}"""
    }
}
