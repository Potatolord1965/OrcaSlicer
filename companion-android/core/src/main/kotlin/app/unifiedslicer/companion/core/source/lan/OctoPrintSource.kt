package app.unifiedslicer.companion.core.source.lan

import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.bool
import app.unifiedslicer.companion.core.net.long
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

class OctoPrintSource(account: LanPrinter, ctx: SourceContext) : HttpPrinterSource(account, ctx), PrintCapable {
    override val via = "LAN · OctoPrint"

    override suspend fun read(): PrinterStatus {
        val printer = try {
            get("/api/printer")
        } catch (e: HttpException) {
            // 409: OctoPrint is running but not connected to the printer.
            if (e.code == 409) return status(PrinterState.OFFLINE).copy(alert = "OctoPrint is not connected to the printer")
            throw e
        }
        return parse(printer, get("/api/job"))
    }

    override suspend fun print(deviceId: String, job: PrintJob, progress: SendProgress) {
        progress.update(0, "Uploading to OctoPrint…")
        val size = job.file.length().coerceAtLeast(1)
        val body = MultipartBody.Builder().setType(MultipartBody.FORM)
            .addFormDataPart("select", "true")
            .addFormDataPart("print", "true")
            .addFormDataPart("file", job.name, fileBody(job.file) { progress.update((it * 100 / size).toInt(), null) })
            .build()
        ctx.http.forUploads().fetchText(request("/api/files/local").post(body).build())
        progress.update(100, "Print started")
    }

    fun parse(printer: JsonElement, job: JsonElement): PrinterStatus {
        val flags = printer.obj("state").obj("flags")
        val state = when {
            flags.bool("error") == true || flags.bool("closedOrError") == true -> PrinterState.FAILED
            flags.bool("paused") == true || flags.bool("pausing") == true -> PrinterState.PAUSED
            flags.bool("printing") == true || flags.bool("cancelling") == true -> PrinterState.PRINTING
            flags.bool("ready") == true || flags.bool("operational") == true -> PrinterState.IDLE
            else -> PrinterState.UNKNOWN
        }
        val progress = job.obj("progress")
        val temps = printer.obj("temperature")
        return status(state).copy(
            progress = progress.num("completion")?.toInt()?.takeIf { state.active },
            remainingSeconds = progress.long("printTimeLeft")?.takeIf { state.active && it > 0 },
            file = job.obj("job").obj("file").str("name")?.takeIf { state.active },
            nozzle = temps.temp("tool0", "actual", "target"),
            bed = temps.temp("bed", "actual", "target"),
            alert = printer.obj("state").str("error"),
        )
    }
}
