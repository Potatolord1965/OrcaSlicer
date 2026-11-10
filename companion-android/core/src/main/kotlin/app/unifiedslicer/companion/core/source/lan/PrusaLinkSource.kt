package app.unifiedslicer.companion.core.source.lan

import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.Temperature
import app.unifiedslicer.companion.core.net.long
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.serialization.json.JsonElement

/** Prusa printers through PrusaLink's v1 API. */
class PrusaLinkSource(account: LanPrinter, ctx: SourceContext) : HttpPrinterSource(account, ctx) {
    override val via = "LAN · PrusaLink"

    override suspend fun read(): PrinterStatus {
        val status = get("/api/v1/status")
        val file = if (status.obj("job") != null) runCatching { get("/api/v1/job").obj("file") }.getOrNull() else null
        return parse(status, file)
    }

    fun parse(s: JsonElement, file: JsonElement?): PrinterStatus {
        val p = s.obj("printer")
        val state = when (p.str("state")?.uppercase()) {
            "IDLE", "READY" -> PrinterState.IDLE
            "BUSY" -> PrinterState.PREPARING
            "PRINTING" -> PrinterState.PRINTING
            "PAUSED" -> PrinterState.PAUSED
            "FINISHED" -> PrinterState.FINISHED
            "STOPPED", "ERROR" -> PrinterState.FAILED
            "ATTENTION" -> PrinterState.PAUSED
            else -> PrinterState.UNKNOWN
        }
        val job = s.obj("job")
        val printing = state.active || state == PrinterState.FINISHED
        return status(state).copy(
            stage = "Needs attention".takeIf { p.str("state").equals("ATTENTION", true) },
            progress = job.num("progress")?.toInt()?.takeIf { printing },
            remainingSeconds = job.long("time_remaining")?.takeIf { state.active && it > 0 },
            file = (file.str("display_name") ?: file.str("name"))?.takeIf { printing },
            nozzle = p.num("temp_nozzle")?.let { Temperature(it, p.num("target_nozzle")?.takeIf { t -> t > 0 }) },
            bed = p.num("temp_bed")?.let { Temperature(it, p.num("target_bed")?.takeIf { t -> t > 0 }) },
        )
    }
}
