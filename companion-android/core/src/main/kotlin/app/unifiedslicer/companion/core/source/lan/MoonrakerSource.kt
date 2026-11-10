package app.unifiedslicer.companion.core.source.lan

import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.serialization.json.JsonElement

/** Klipper through Moonraker's HTTP API. */
class MoonrakerSource(account: LanPrinter, ctx: SourceContext) : HttpPrinterSource(account, ctx) {
    override val via = "LAN · Moonraker"

    override suspend fun read(): PrinterStatus {
        val reply = try {
            get("/printer/objects/query?print_stats&virtual_sdcard&display_status&extruder&heater_bed")
        } catch (e: HttpException) {
            // Moonraker is up but Klipper is not ready (shutdown, firmware restart, MCU error).
            if (e.code == 503 || e.code == 400) return status(PrinterState.FAILED).copy(alert = "Klipper not ready")
            throw e
        }
        return parse(reply.obj("result").obj("status"))
    }

    fun parse(s: JsonElement?): PrinterStatus {
        val ps = s.obj("print_stats")
        val state = when (ps.str("state")) {
            "standby" -> PrinterState.IDLE
            "printing" -> PrinterState.PRINTING
            "paused" -> PrinterState.PAUSED
            "complete" -> PrinterState.FINISHED
            "cancelled", "error" -> PrinterState.FAILED
            else -> PrinterState.UNKNOWN
        }
        val printing = state.active || state == PrinterState.FINISHED
        val progress = s.obj("virtual_sdcard").num("progress") ?: s.obj("display_status").num("progress")
        val duration = ps.num("print_duration")
        val remaining = if (progress != null && duration != null && progress > 0.01)
            (duration / progress - duration).toLong() else null
        return status(state).copy(
            progress = progress?.let { (it * 100).toInt() }?.takeIf { printing },
            remainingSeconds = remaining?.takeIf { state.active && it > 0 },
            layer = ps.obj("info").int("current_layer")?.takeIf { printing },
            totalLayers = ps.obj("info").int("total_layer")?.takeIf { printing && it > 0 },
            file = ps.str("filename")?.takeIf { printing },
            nozzle = s.temp("extruder"),
            bed = s.temp("heater_bed"),
            alert = ps.str("message")?.takeIf { state == PrinterState.FAILED },
        )
    }
}
