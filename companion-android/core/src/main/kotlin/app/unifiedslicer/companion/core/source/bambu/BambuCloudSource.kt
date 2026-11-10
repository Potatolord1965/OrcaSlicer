package app.unifiedslicer.companion.core.source.bambu

import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.MqttLink
import app.unifiedslicer.companion.core.net.arr
import app.unifiedslicer.companion.core.net.bool
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.mergeJson
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.source.awaitCondition
import kotlinx.serialization.json.JsonObject
import okhttp3.Request
import java.util.concurrent.ConcurrentHashMap

/**
 * Bambu Cloud: the account's bound printers from the REST API, live status from Bambu's MQTT
 * broker (user "u_<uid>", password = access token, topic device/<serial>/report).
 */
class BambuCloudSource(account: BambuAccount, private val ctx: SourceContext) : PrinterSource {
    override var account: BambuAccount = account
        private set

    private data class Device(val id: String, val name: String, val model: String, val online: Boolean, val printStatus: String?)

    private var devices: List<Device> = emptyList()
    private var devicesAt = 0L
    private var link: MqttLink? = null
    private var linkSerials: Set<String> = emptySet()
    private val reports = ConcurrentHashMap<String, Pair<JsonObject, Long>>()

    override suspend fun refresh(): List<PrinterStatus> {
        if (System.currentTimeMillis() - devicesAt > DEVICE_LIST_TTL_MS) {
            devices = withRenewal { fetchDevices() }
            devicesAt = System.currentTimeMillis()
        }
        withRenewal { connectLink() }
        return devices.map { d ->
            val report = reports[d.id]
            when {
                !d.online -> PrinterStatus(account.id, d.id, d.name, d.model, Brand.BAMBU, VIA, PrinterState.OFFLINE)
                report != null -> BambuReport.toStatus(account.id, d.id, d.name, d.model, VIA, report.first, report.second)
                else -> PrinterStatus(
                    account.id, d.id, d.name, d.model, Brand.BAMBU, VIA,
                    if (d.printStatus == "ACTIVE") PrinterState.PRINTING else PrinterState.IDLE,
                )
            }
        }
    }

    private suspend fun connectLink() {
        val serials = devices.map { it.id }.toSet()
        if (link == null || serials != linkSerials) {
            link?.close()
            linkSerials = serials
            link = if (serials.isEmpty()) null else MqttLink(
                uri = "ssl://${BambuAuth.mqttHost(account.region)}:8883",
                username = "u_${account.uid}",
                password = account.accessToken,
                topics = serials.map { "device/$it/report" },
                onConnected = { l -> serials.forEach { l.publish("device/$it/request", PUSH_ALL) } },
                onMessage = ::onMessage,
            )
        }
        link?.let { l ->
            if (!l.isConnected) {
                l.ensureConnected()
                val online = devices.filter { it.online }.map { it.id }
                awaitCondition(6_000) { online.all { reports.containsKey(it) } }
            }
        }
    }

    private fun onMessage(topic: String, payload: String) {
        val serial = topic.removePrefix("device/").substringBefore('/')
        val print = parseJson(payload).obj("print") ?: return
        val merged = reports[serial]?.first?.let { mergeJson(it, print) } ?: print
        reports[serial] = merged to System.currentTimeMillis()
    }

    private suspend fun fetchDevices(): List<Device> {
        val reply = ctx.http.fetchJson(
            Request.Builder().url(BambuAuth.apiBase(account.region) + "/v1/iot-service/api/user/bind")
                .header("Authorization", "Bearer ${account.accessToken}").build()
        )
        return reply.arr("devices").orEmpty().mapNotNull { d ->
            val id = d.str("dev_id") ?: return@mapNotNull null
            Device(
                id = id,
                name = d.str("name") ?: id,
                model = d.str("dev_product_name") ?: d.str("dev_model_name").orEmpty(),
                online = d.bool("online") ?: true,
                printStatus = d.str("print_status"),
            )
        }
    }

    /** Runs [block]; on a rejected token renews it once and retries. */
    private suspend fun <T> withRenewal(block: suspend () -> T): T = try {
        block()
    } catch (e: AuthException) {
        account = BambuAuth(ctx.http).refresh(account)
        ctx.onAccountUpdated(account)
        link?.close(); link = null
        block()
    }

    override fun close() {
        link?.close()
        link = null
    }

    private companion object {
        const val VIA = "Bambu Cloud"
        const val DEVICE_LIST_TTL_MS = 60_000L
        const val PUSH_ALL = """{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}"""
    }
}
