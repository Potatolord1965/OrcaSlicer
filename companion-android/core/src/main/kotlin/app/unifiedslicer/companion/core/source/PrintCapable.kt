package app.unifiedslicer.companion.core.source

import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.CrealityAccount
import app.unifiedslicer.companion.core.model.ElegooAccount
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.MarkforgedAccount
import java.io.Closeable
import java.io.File

/** A sliced file on the phone, ready to send. [name] is the name the printer shows. */
data class PrintJob(val file: File, val name: String)

/** Progress of a send: [percent] 0..100 when known, [status] when the step changes. */
fun interface SendProgress {
    fun update(percent: Int?, status: String?)
}

/** A source whose printers can be sent a print. Phones cannot slice, so files must be printer-ready. */
interface PrintCapable {
    /** Uploads [job] to the printer [deviceId] and starts it. Throws with a readable message on failure. */
    suspend fun print(deviceId: String, job: PrintJob, progress: SendProgress)
}

/** Whether printers of [account] can be sent the file [fileName] from this app. */
fun printableBy(account: Account, fileName: String): Boolean = when (account) {
    is ElegooAccount -> hasExtension(fileName, ".gcode", ".3mf")
    is CrealityAccount -> hasExtension(fileName, ".gcode")
    is LanPrinter -> when (account.kind) {
        LanKind.ELEGOO_CC2 -> hasExtension(fileName, ".gcode")
        LanKind.MOONRAKER, LanKind.OCTOPRINT -> hasExtension(fileName, ".gcode", ".gco", ".g")
        LanKind.PRUSALINK -> hasExtension(fileName, ".gcode", ".bgcode")
        // LAN-mode sends need Bambu's developer mode and FTPS; not supported.
        LanKind.BAMBU -> false
    }
    // Bambu only accepts cloud prints from its own apps; Eiger slices STL itself.
    is BambuAccount, is MarkforgedAccount -> false
}

/** Why [account]'s printers take no prints from this app, for brands where that is by design. */
fun printBlockedReason(account: Account): String? = when {
    account is BambuAccount || (account is LanPrinter && account.kind == LanKind.BAMBU) ->
        "Bambu printers only accept prints from Bambu's own apps. Use Print in MakerWorld to hand the job to Bambu Handy."
    account is MarkforgedAccount -> "Markforged printers only print jobs sliced in Eiger."
    else -> null
}

internal fun hasExtension(fileName: String, vararg extensions: String) =
    extensions.any { fileName.endsWith(it, ignoreCase = true) }

/**
 * Real-time messaging to printers behind Elegoo's cloud (Agora RTM). The SDK is Android-only, so
 * the app supplies the implementation.
 */
fun interface RtmConnector {
    /** Logs in and returns a session; [onMessage] gets every incoming message with its publisher id. */
    suspend fun connect(appId: String, userId: String, token: String, onMessage: (publisher: String, body: String) -> Unit): RtmSession
}

interface RtmSession : Closeable {
    /** Sends [message] to one user (a printer's messaging id). */
    suspend fun publish(peer: String, message: String)
}
