package app.unifiedslicer.companion.core.source.elegoo

import app.unifiedslicer.companion.core.net.el
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.source.RtmConnector
import app.unifiedslicer.companion.core.source.RtmSession
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.Closeable
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/**
 * Request/response with one printer over Elegoo's cloud messaging, as in the desktop app's
 * PrinterChannel: {"id", "method", "params"} to the user "<agora user id><serial>", replies
 * matched by id, download progress pushed as method 6006.
 */
class ElegooPrinterChannel private constructor(private val printerUser: String) : Closeable {
    private var session: RtmSession? = null
    private val nextId = AtomicInteger((System.currentTimeMillis() / 1000 % 100000).toInt() * 10)
    private val pending = ConcurrentHashMap<Int, CompletableDeferred<JsonElement>>()

    /** Latest 6006 result: {progress, status} with status 1 done, 2 cancelled, 3 failed. */
    @Volatile var download: JsonObject? = null

    private fun onMessage(publisher: String, body: String) {
        if (publisher.isNotEmpty() && publisher != printerUser) return
        val msg = parseJson(body) as? JsonObject ?: return
        if (msg.int("method") == M_DOWNLOAD_PROGRESS) download = msg.obj("result")
        else msg.int("id")?.let { pending[it]?.complete(msg) }
    }

    suspend fun request(method: Int, params: JsonObject = JsonObject(emptyMap()), timeoutSeconds: Int = 10, needReply: Boolean = true): JsonElement? {
        val id = nextId.incrementAndGet()
        val waiter = CompletableDeferred<JsonElement>()
        if (needReply) pending[id] = waiter
        try {
            val msg = buildJsonObject { put("id", id); put("method", method); put("params", params) }
            (session ?: throw IOException("Not connected to Elegoo messaging")).publish(printerUser, msg.toString())
            if (!needReply) return null
            val reply = withTimeoutOrNull(timeoutSeconds * 1000L) { waiter.await() }
                ?: throw IOException("The printer did not answer within $timeoutSeconds s. Is it online?")
            val code = (reply.el("result") as? JsonObject).int("error_code") ?: 0
            if (code != 0) throw ElegooPrinterError(code, method)
            return reply
        } finally {
            pending.remove(id)
        }
    }

    override fun close() {
        session?.close()
        session = null
    }

    companion object {
        const val AGORA_APP_ID = "d035320941e34cd5bc4ec106eff05580" // public, from Elegoo's open-source elegoo-link
        const val M_START_PRINT = 1020
        const val M_DOWNLOAD_FILE = 1057
        const val M_CANCEL_DOWNLOAD = 1058
        const val M_DOWNLOAD_PROGRESS = 6006

        suspend fun open(rtm: RtmConnector, agoraUserId: String, rtmUserId: String, rtmToken: String, serial: String): ElegooPrinterChannel {
            val ch = ElegooPrinterChannel(agoraUserId + serial)
            ch.session = rtm.connect(AGORA_APP_ID, rtmUserId, rtmToken, ch::onMessage)
            return ch
        }
    }
}

class ElegooPrinterError(val code: Int, method: Int) : IOException(
    when (code) {
        1026 -> "The printer has no bed mesh yet (error 1026)"
        else -> "The printer refused the request (method $method, error $code)"
    }
)
