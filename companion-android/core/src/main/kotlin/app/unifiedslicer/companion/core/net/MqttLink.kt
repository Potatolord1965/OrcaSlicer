package app.unifiedslicer.companion.core.net

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended
import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttException
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import java.io.Closeable
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.UUID
import java.util.concurrent.Executors
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * A kept-open MQTT connection (Eclipse Paho). Subscribes [topics] on every (re)connect and then
 * calls [onConnected], e.g. to ask the printer for a full status.
 *
 * [insecureTls] is for printers on the local network that present a self-signed certificate
 * (Bambu LAN mode); cloud brokers are always verified.
 */
class MqttLink(
    private val uri: String,
    private val username: String,
    private val password: String,
    private val topics: List<String>,
    private val insecureTls: Boolean = false,
    clientId: String = "usc-" + UUID.randomUUID().toString().take(12),
    private val onConnected: (MqttLink) -> Unit = {},
    private val onMessage: (topic: String, payload: String) -> Unit,
) : Closeable {
    private val client = MqttClient(uri, clientId, MemoryPersistence())
    // Paho must not be called back into from its own callback thread.
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "mqtt-$clientId").apply { isDaemon = true } }
    @Volatile private var closed = false

    val isConnected: Boolean get() = client.isConnected

    init {
        client.setCallback(object : MqttCallbackExtended {
            override fun connectComplete(reconnect: Boolean, serverURI: String?) {
                worker.execute {
                    runCatching {
                        if (topics.isNotEmpty()) client.subscribe(topics.toTypedArray(), IntArray(topics.size) { 0 })
                        onConnected(this@MqttLink)
                    }
                }
            }

            override fun connectionLost(cause: Throwable?) {}
            override fun deliveryComplete(token: IMqttDeliveryToken?) {}
            override fun messageArrived(topic: String, message: MqttMessage) {
                runCatching { onMessage(topic, String(message.payload, Charsets.UTF_8)) }
            }
        })
    }

    /** Connects if not connected. Throws on failure; a bad user name or password becomes [AuthException]. */
    suspend fun ensureConnected() = withContext(Dispatchers.IO) {
        if (closed || client.isConnected) return@withContext
        val opts = MqttConnectOptions().apply {
            userName = username
            password = this@MqttLink.password.toCharArray()
            isCleanSession = true
            isAutomaticReconnect = true
            connectionTimeout = 10
            keepAliveInterval = 30
            if (uri.startsWith("ssl://")) {
                socketFactory = if (insecureTls) trustAllSocketFactory() else SSLSocketFactory.getDefault()
                isHttpsHostnameVerificationEnabled = !insecureTls
            }
        }
        try {
            client.connect(opts)
        } catch (e: MqttException) {
            if (e.reasonCode == MqttException.REASON_CODE_FAILED_AUTHENTICATION.toInt() ||
                e.reasonCode == MqttException.REASON_CODE_NOT_AUTHORIZED.toInt()
            ) throw AuthException("The printer or broker rejected the credentials")
            throw java.io.IOException("Could not connect to ${uri.substringAfter("://")}: ${e.message ?: e.reasonCode}", e)
        }
    }

    fun publish(topic: String, payload: String) {
        if (closed) return
        worker.execute { runCatching { if (client.isConnected) client.publish(topic, payload.toByteArray(), 0, false) } }
    }

    override fun close() {
        closed = true
        worker.execute {
            runCatching { if (client.isConnected) client.disconnect(2000) }
            runCatching { client.close(true) }
        }
        worker.shutdown()
    }

    private companion object {
        fun trustAllSocketFactory(): SSLSocketFactory {
            val trustAll = object : X509TrustManager {
                override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
                override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
                override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            }
            return SSLContext.getInstance("TLS").apply { init(null, arrayOf<TrustManager>(trustAll), SecureRandom()) }.socketFactory
        }
    }
}
