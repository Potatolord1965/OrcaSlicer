package app.unifiedslicer.companion.ui.add

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.net.el
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.net.str
import app.unifiedslicer.companion.core.source.elegoo.ElegooCloudSource
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonObject
import java.util.UUID

/** One stable id per installation: Elegoo binds the tokens its sign-in page issues to it. */
private fun elegooDeviceId(context: Context): String {
    val prefs = context.getSharedPreferences("elegoo", Context.MODE_PRIVATE)
    return prefs.getString("device_id", null) ?: UUID.randomUUID().toString().also { prefs.edit().putString("device_id", it).apply() }
}

/**
 * The page posts JSON requests the way ElegooSlicer's pages do (window.chrome.webview.postMessage
 * under WebView2, the wx handler elsewhere) and reads replies through window.HandleStudio.
 */
private const val BRIDGE_SHIM = """
(function () {
  if (window.__unifiedBridge) return;
  window.__unifiedBridge = true;
  var post = function (m) { UnifiedBridge.postMessage(typeof m === 'string' ? m : JSON.stringify(m)); };
  window.chrome = window.chrome || {};
  window.chrome.webview = window.chrome.webview || { postMessage: post, addEventListener: function () {}, removeEventListener: function () {} };
  window.webkit = window.webkit || {};
  window.webkit.messageHandlers = window.webkit.messageHandlers || {};
  window.webkit.messageHandlers.wx = { postMessage: post };
  window.wx = window.wx || { postMessage: post };
})();
"""

@OptIn(ExperimentalMaterial3Api::class)
@SuppressLint("SetJavaScriptEnabled")
@Composable
fun ElegooLoginScreen(replaceId: String?, dark: Boolean, onBack: () -> Unit, onDone: (Account) -> Unit) {
    var error by remember { mutableStateOf<String?>(null) }
    var webView by remember { mutableStateOf<WebView?>(null) }
    DisposableEffect(Unit) { onDispose { webView?.destroy() } }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Elegoo") },
                navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") } },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background),
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { ctx ->
                    val deviceId = elegooDeviceId(ctx)
                    val main = Handler(Looper.getMainLooper())
                    WebView(ctx).apply {
                        webView = this
                        settings.javaScriptEnabled = true
                        settings.domStorageEnabled = true
                        settings.userAgentString = ElegooCloudSource.LOGIN_USER_AGENT
                        fun reply(id: String, method: String, ok: Boolean, message: String = "") {
                            val r = buildJsonObject {
                                put("id", id); put("method", method); put("type", "response")
                                put("code", if (ok) 0 else -1); put("message", if (ok) "success" else message)
                                putJsonObject("data") {}
                            }
                            evaluateJavascript("window.HandleStudio && window.HandleStudio($r)", null)
                        }
                        fun handle(raw: String) {
                            // Only Elegoo's own pages may sign the app in.
                            if (!ElegooCloudSource.isElegooPage(url)) return
                            val msg = parseJson(raw) ?: return
                            if ((msg.str("type") ?: "request") != "request") return
                            val method = msg.str("method").orEmpty()
                            val id = msg.str("id").orEmpty()
                            when (method) {
                                "report.userInfo" -> {
                                    val account = ElegooCloudSource.accountFromLogin(msg.el("params"), replaceId ?: ("elegoo-" + UUID.randomUUID()))
                                    reply(id, method, account != null, "No account was returned")
                                    if (account != null) onDone(account) else error = "The Elegoo sign-in page did not return an account."
                                }
                                "report.ready" -> {
                                    reply(id, method, true)
                                    val event = buildJsonObject {
                                        put("method", "client.setDeviceId"); put("type", "event")
                                        putJsonObject("data") { put("deviceId", deviceId) }
                                    }
                                    evaluateJavascript("window.HandleStudio && window.HandleStudio($event)", null)
                                }
                                "report.websiteOpen" -> {
                                    reply(id, method, true)
                                    msg.el("params").str("url")?.takeIf { it.startsWith("https://") }?.let {
                                        ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(it)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                                    }
                                }
                                "reload" -> { reply(id, method, true); loadUrl(ElegooCloudSource.loginUrl(deviceId, dark)) }
                                else -> reply(id, method, true)
                            }
                        }
                        addJavascriptInterface(object {
                            @JavascriptInterface
                            fun postMessage(message: String) { main.post { handle(message) } }
                        }, "UnifiedBridge")
                        val documentStart = WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)
                        if (documentStart) WebViewCompat.addDocumentStartJavaScript(this, BRIDGE_SHIM, setOf("https://*.nexprint.com", "https://nexprint.com", "https://*.elegoo.com"))
                        webViewClient = object : WebViewClient() {
                            override fun onPageStarted(view: WebView, url: String?, favicon: android.graphics.Bitmap?) {
                                if (!documentStart) view.evaluateJavascript(BRIDGE_SHIM, null)
                            }
                            override fun onPageFinished(view: WebView, url: String?) {
                                view.evaluateJavascript(BRIDGE_SHIM, null)
                            }
                            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                                // Links to other sites (terms, help) open in the browser.
                                val u = request.url.toString()
                                if (ElegooCloudSource.isElegooPage(u)) return false
                                runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, request.url).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
                                return true
                            }
                        }
                        loadUrl(ElegooCloudSource.loginUrl(deviceId, dark))
                    }
                },
            )
            error?.let {
                Text(it, Modifier.align(Alignment.BottomCenter).padding(16.dp), color = MaterialTheme.colorScheme.error)
            }
        }
    }
}
