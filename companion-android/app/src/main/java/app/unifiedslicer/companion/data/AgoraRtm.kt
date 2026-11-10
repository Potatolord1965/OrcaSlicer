package app.unifiedslicer.companion.data

import app.unifiedslicer.companion.core.source.RtmConnector
import app.unifiedslicer.companion.core.source.RtmSession
import io.agora.rtm.ErrorInfo
import io.agora.rtm.MessageEvent
import io.agora.rtm.PublishOptions
import io.agora.rtm.ResultCallback
import io.agora.rtm.RtmClient
import io.agora.rtm.RtmConfig
import io.agora.rtm.RtmConstants
import io.agora.rtm.RtmEventListener
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.io.IOException
import java.util.EnumSet
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * Elegoo's cloud messaging through Agora's RTM 2 SDK, configured as the desktop app does it
 * (string user ids, user-channel "PlainText" messages). The SDK allows one client per process.
 */
object AgoraRtm : RtmConnector {
    private fun <T> callback(cont: kotlinx.coroutines.CancellableContinuation<Unit>, what: String) = object : ResultCallback<T> {
        override fun onSuccess(result: T?) { if (cont.isActive) cont.resume(Unit) }
        override fun onFailure(error: ErrorInfo?) {
            if (cont.isActive) cont.resumeWithException(IOException("Elegoo messaging: $what failed (${error?.errorReason ?: error?.errorCode})"))
        }
    }

    override suspend fun connect(appId: String, userId: String, token: String, onMessage: (publisher: String, body: String) -> Unit): RtmSession =
        withContext(Dispatchers.IO) {
            val listener = object : RtmEventListener {
                override fun onMessageEvent(event: MessageEvent) {
                    val body = when (val data = event.message?.data) {
                        is String -> data
                        is ByteArray -> String(data, Charsets.UTF_8)
                        else -> return
                    }
                    runCatching { onMessage(event.publisherId.orEmpty(), body) }
                }
            }
            val config = RtmConfig.Builder(appId, userId)
                .areaCode(EnumSet.of(RtmConstants.RtmAreaCode.GLOB))
                .presenceTimeout(300)
                .heartbeatInterval(30)
                .useStringUserId(true)
                .eventListener(listener)
                .build()
            runCatching { RtmClient.release() } // a session left over from an earlier send
            val client = try {
                RtmClient.create(config)
            } catch (e: Exception) {
                throw IOException("Elegoo messaging could not start: ${e.message}", e)
            }
            try {
                withTimeout(20_000) {
                    suspendCancellableCoroutine<Unit> { cont -> client.login(token, callback<Void>(cont, "sign-in")) }
                }
            } catch (e: Exception) {
                runCatching { RtmClient.release() }
                throw if (e is IOException) e else IOException("Elegoo messaging did not connect", e)
            }
            object : RtmSession {
                private val options = PublishOptions(RtmConstants.RtmChannelType.USER, "PlainText", false)

                override suspend fun publish(peer: String, message: String) = withTimeout(15_000) {
                    suspendCancellableCoroutine<Unit> { cont -> client.publish(peer, message, options, callback<Void>(cont, "sending")) }
                }

                override fun close() {
                    runCatching { client.logout(null) }
                    runCatching { RtmClient.release() }
                }
            }
        }
}
