package app.unifiedslicer.companion.core.source.lan

import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.Temperature
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.baseUrl
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.serialization.json.JsonElement
import okhttp3.Request
import java.io.IOException

/** Base for LAN printers polled over HTTP with an optional X-Api-Key. Unreachable means offline. */
abstract class HttpPrinterSource(final override val account: LanPrinter, protected val ctx: SourceContext) : PrinterSource {
    protected val base = baseUrl(account.host)
    protected abstract val via: String

    protected suspend fun get(path: String): JsonElement {
        val b = Request.Builder().url(base + path)
        if (account.secret.isNotBlank()) b.header("X-Api-Key", account.secret.trim())
        return ctx.http.fetchJson(b.build())
    }

    /** A request to [path] carrying the API key, for subclasses that upload. */
    protected fun request(path: String): Request.Builder {
        val b = Request.Builder().url(base + path)
        if (account.secret.isNotBlank()) b.header("X-Api-Key", account.secret.trim())
        return b
    }

    protected fun status(state: PrinterState) =
        PrinterStatus(account.id, account.host, account.label, account.kind.label, account.brand, via, state)

    protected abstract suspend fun read(): PrinterStatus

    override suspend fun refresh(): List<PrinterStatus> = listOf(
        try {
            read()
        } catch (e: AuthException) {
            throw e
        } catch (e: HttpException) {
            throw e
        } catch (e: IOException) {
            status(PrinterState.OFFLINE)
        }
    )

    protected fun JsonElement?.temp(group: String, cur: String = "temperature", target: String = "target"): Temperature? =
        obj(group)?.let { g -> g.num(cur)?.let { Temperature(it, g.num(target)?.takeIf { t -> t > 0 }) } }
}
