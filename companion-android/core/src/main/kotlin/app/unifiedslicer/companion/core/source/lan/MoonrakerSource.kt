package app.unifiedslicer.companion.core.source.lan

import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.net.fetchText
import app.unifiedslicer.companion.core.net.fileBody
import app.unifiedslicer.companion.core.net.forUploads
import app.unifiedslicer.companion.core.source.PrintCapable
import app.unifiedslicer.companion.core.source.PrintJob
import app.unifiedslicer.companion.core.source.SendProgress
import app.unifiedslicer.companion.core.source.SourceContext
import okhttp3.MultipartBody
import kotlinx.serialization.json.JsonElement

/** Klipper through Moonraker's HTTP API. */
class MoonrakerSource(account: LanPrinter, ctx: SourceContext) : HttpPrinterSource(account, ctx), PrintCapable {
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

    override suspend fun print(deviceId: String, job: PrintJob, progress: SendProgress) {
        progress.update(0, "Uploading to the printer…")
        val size = job.file.length().coerceAtLeast(1)
        val body = MultipartBody.Builder().setType(MultipartBody.FORM)
            .addFormDataPart("root", "gcodes")
            .addFormDataPart("print", "true")
            .addFormDataPart("file", job.name, fileBody(job.file) { progress.update((it * 100 / size).toInt(), null) })
            .build()
        ctx.http.forUploads().fetchText(request("/server/files/upload").post(body).build())
        progress.update(100, "Print started")
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
