package app.unifiedslicer.companion.core.source

import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.CrealityAccount
import app.unifiedslicer.companion.core.model.ElegooAccount
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.MarkforgedAccount
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.source.bambu.BambuCloudSource
import app.unifiedslicer.companion.core.source.bambu.BambuLanSource
import app.unifiedslicer.companion.core.source.creality.CrealityCloudSource
import app.unifiedslicer.companion.core.source.elegoo.ElegooCloudSource
import app.unifiedslicer.companion.core.source.elegoo.ElegooLanSource
import app.unifiedslicer.companion.core.source.lan.MoonrakerSource
import app.unifiedslicer.companion.core.source.lan.OctoPrintSource
import app.unifiedslicer.companion.core.source.lan.PrusaLinkSource
import app.unifiedslicer.companion.core.source.markforged.EigerSource
import kotlinx.coroutines.delay
import okhttp3.OkHttpClient
import java.io.Closeable

/**
 * Reads the printers behind one [Account]. Sources may keep connections open between refreshes
 * (MQTT); [close] releases them.
 */
interface PrinterSource : Closeable {
    val account: Account

    /** Current status of every printer of the account. Throws on network or sign-in errors. */
    suspend fun refresh(): List<PrinterStatus>

    override fun close() {}
}

/**
 * What sources share: the HTTP client, and a callback for sources that renew their tokens, so
 * the app can save the new account.
 */
class SourceContext(
    val http: OkHttpClient,
    val onAccountUpdated: (Account) -> Unit = {},
    /** Needed for printing through Elegoo's cloud; null where the messaging SDK is unavailable. */
    val rtm: RtmConnector? = null,
)

fun createSource(account: Account, ctx: SourceContext): PrinterSource = when (account) {
    is BambuAccount -> BambuCloudSource(account, ctx)
    is ElegooAccount -> ElegooCloudSource(account, ctx)
    is CrealityAccount -> CrealityCloudSource(account, ctx)
    is MarkforgedAccount -> EigerSource(account, ctx)
    is LanPrinter -> when (account.kind) {
        LanKind.BAMBU -> BambuLanSource(account)
        LanKind.ELEGOO_CC2 -> ElegooLanSource(account, ctx)
        LanKind.MOONRAKER -> MoonrakerSource(account, ctx)
        LanKind.OCTOPRINT -> OctoPrintSource(account, ctx)
        LanKind.PRUSALINK -> PrusaLinkSource(account, ctx)
    }
}

/** Waits until [ready] or [timeoutMs] passes. */
internal suspend fun awaitCondition(timeoutMs: Long, ready: () -> Boolean) {
    val end = System.currentTimeMillis() + timeoutMs
    while (!ready() && System.currentTimeMillis() < end) delay(150)
}
