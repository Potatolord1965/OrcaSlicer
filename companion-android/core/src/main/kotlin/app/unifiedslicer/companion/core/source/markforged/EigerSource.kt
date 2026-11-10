package app.unifiedslicer.companion.core.source.markforged

import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.MarkforgedAccount
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.arr
import app.unifiedslicer.companion.core.net.bool
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.long
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import okhttp3.Credentials
import okhttp3.Request

/** Markforged Eiger public API v3 (Basic auth with the organization's API keys). Eiger reports no temperatures. */
class EigerSource(override val account: MarkforgedAccount, private val ctx: SourceContext) : PrinterSource {
    private val auth = Credentials.basic(account.accessKey, account.secretKey)

    private suspend fun get(path: String): JsonElement =
        ctx.http.fetchJson(Request.Builder().url(API + path).header("Authorization", auth).build())

    override suspend fun refresh(): List<PrinterStatus> {
        val list = mutableListOf<JsonElement>()
        for (page in 1..20) {
            val res = get("/devices?page%5Bnumber%5D=$page&page%5Bsize%5D=100")
            list += (res as? JsonArray) ?: res.arr("items").orEmpty()
            if (res.bool("has_more_items") != true) break
        }
        return coroutineScope {
            list.mapNotNull { d ->
                val id = d.str("id") ?: return@mapNotNull null
                // The list carries no active job; the device record does.
                async { toStatus(id, runCatching { get("/devices/$id") }.getOrDefault(d)) }
            }.awaitAll()
        }
    }

    fun toStatus(id: String, d: JsonElement): PrinterStatus {
        val raw = d.str("state").orEmpty()
        val job = d.obj("active_job")
        val state = state(raw, job.str("printing_state"))
        val printing = state.active || state == PrinterState.FINISHED
        return PrinterStatus(
            accountId = account.id,
            deviceId = id,
            name = d.str("name") ?: id,
            model = d.str("device_type").orEmpty(),
            brand = Brand.MARKFORGED,
            via = "Eiger",
            state = state,
            stage = raw.takeIf { it.isNotEmpty() && state == PrinterState.FAILED },
            progress = job.int("progress")?.takeIf { printing },
            remainingSeconds = job.long("estimated_seconds_remaining")?.takeIf { state.active && it > 0 },
            layer = job.int("current_layer")?.takeIf { printing },
            totalLayers = job.int("layer_count")?.takeIf { printing && it > 0 },
            file = job.obj("build").str("title")?.takeIf { printing },
        )
    }

    companion object {
        private const val API = "https://www.eiger.io/api/v3"

        fun state(state: String, printingState: String?): PrinterState = when (state.lowercase()) {
            "printing" -> if (printingState?.lowercase() in setOf("heating", "scanning", "calibrating")) PrinterState.PREPARING else PrinterState.PRINTING
            "print paused" -> PrinterState.PAUSED
            "print finished" -> PrinterState.FINISHED
            "material jam", "fiber jam", "out of material", "user action required" -> PrinterState.FAILED
            "offline", "disconnected" -> PrinterState.OFFLINE
            "" -> PrinterState.UNKNOWN
            else -> PrinterState.IDLE
        }
    }
}
