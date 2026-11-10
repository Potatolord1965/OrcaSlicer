package app.unifiedslicer.companion.ui

import android.annotation.SuppressLint
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Base64
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.URLUtil
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import app.unifiedslicer.companion.core.source.PrintJob
import app.unifiedslicer.companion.data.PrintFiles
import app.unifiedslicer.companion.ui.theme.color
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * Pages hand over in-memory downloads as blob: URLs, which only the page itself can read. This
 * reads the blob in the page and passes it to the app in pieces.
 */
private fun blobScript(url: String, name: String) = """
(async function () {
  try {
    const blob = await (await fetch(${quote(url)})).blob();
    const step = 512 * 1024;
    UnifiedDownloads.begin(${quote(name)}, blob.size);
    for (let at = 0; at < blob.size; at += step) {
      const text = await new Promise((ok, bad) => {
        const r = new FileReader();
        r.onload = () => ok(r.result); r.onerror = bad;
        r.readAsDataURL(blob.slice(at, at + step));
      });
      UnifiedDownloads.chunk(text.substring(text.indexOf(',') + 1));
    }
    UnifiedDownloads.end();
  } catch (e) { UnifiedDownloads.fail(String(e)); }
})();
"""

private fun quote(s: String) = "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", " ") + "\""

/** An in-app browser for one model site. A downloaded printer-ready file is handed to [onFile]. */
@OptIn(ExperimentalMaterial3Api::class)
@SuppressLint("SetJavaScriptEnabled")
@Composable
fun BrowserScreen(site: ModelSite, http: OkHttpClient, onBack: () -> Unit, onFile: (PrintJob) -> Unit) {
    var webView by remember { mutableStateOf<WebView?>(null) }
    var loading by remember { mutableIntStateOf(0) }
    var download by remember { mutableStateOf<String?>(null) }
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val accent = site.brand.color()

    BackHandler { if (webView?.canGoBack() == true) webView?.goBack() else onBack() }
    DisposableEffect(Unit) { onDispose { webView?.destroy() } }

    fun finished(file: File) {
        download = null
        if (PrintFiles.isPrinterReady(file.name)) onFile(PrintJob(file, file.name))
        else {
            file.delete()
            scope.launch { snackbar.showSnackbar("${file.name} is a model, not a sliced file. Slice it in Unified Slicer on your computer, or pick a print profile here.") }
        }
    }

    fun failed(message: String) {
        download = null
        scope.launch { snackbar.showSnackbar("Download failed: $message") }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(site.title) },
                navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.Filled.Close, "Close") } },
                actions = {
                    IconButton(onClick = { webView?.let { if (it.canGoBack()) it.goBack() } }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
                    IconButton(onClick = { webView?.reload() }) { Icon(Icons.Filled.Refresh, "Reload") }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background),
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { ctx ->
                    val main = Handler(Looper.getMainLooper())
                    WebView(ctx).apply {
                        webView = this
                        settings.javaScriptEnabled = true
                        settings.domStorageEnabled = true
                        settings.setSupportMultipleWindows(false)
                        // Sites treat an embedded web view ("; wv") as second-class; present as the browser it is.
                        settings.userAgentString = settings.userAgentString.replace("; wv", "")
                        CookieManager.getInstance().setAcceptThirdPartyCookies(this, true)

                        // blob: downloads arrive through this bridge, written straight to the cache.
                        addJavascriptInterface(object {
                            private var out: FileOutputStream? = null
                            private var file: File? = null

                            @JavascriptInterface
                            fun begin(name: String, size: Double) {
                                out?.close()
                                file = PrintFiles.newFile(ctx, name).also { out = FileOutputStream(it) }
                                main.post { download = name }
                            }

                            @JavascriptInterface
                            fun chunk(base64: String) { out?.write(Base64.decode(base64, Base64.DEFAULT)) }

                            @JavascriptInterface
                            fun end() {
                                out?.close(); out = null
                                file?.let { f -> main.post { finished(f) } }
                            }

                            @JavascriptInterface
                            fun fail(message: String) {
                                out?.close(); out = null
                                file?.delete()
                                main.post { failed(message) }
                            }
                        }, "UnifiedDownloads")

                        setDownloadListener { url, userAgent, contentDisposition, mimeType, _ ->
                            val name = PrintFiles.safeName(URLUtil.guessFileName(url, contentDisposition, mimeType))
                            if (url.startsWith("blob:")) {
                                evaluateJavascript(blobScript(url, name), null)
                                return@setDownloadListener
                            }
                            if (!url.startsWith("https://")) {
                                failed("unsupported link")
                                return@setDownloadListener
                            }
                            download = name
                            val cookies = CookieManager.getInstance().getCookie(url)
                            val referer = this.url
                            scope.launch {
                                try {
                                    val file = withContext(Dispatchers.IO) {
                                        val req = Request.Builder().url(url).header("User-Agent", userAgent).apply {
                                            if (!cookies.isNullOrEmpty()) header("Cookie", cookies)
                                            if (!referer.isNullOrEmpty()) header("Referer", referer)
                                        }.build()
                                        val client = http.newBuilder().callTimeout(0, TimeUnit.SECONDS).readTimeout(120, TimeUnit.SECONDS).build()
                                        client.newCall(req).execute().use { resp ->
                                            if (!resp.isSuccessful) throw IOException("HTTP ${resp.code}")
                                            val target = PrintFiles.newFile(ctx, name)
                                            (resp.body ?: throw IOException("empty reply")).byteStream().use { input ->
                                                target.outputStream().use { input.copyTo(it) }
                                            }
                                            target
                                        }
                                    }
                                    finished(file)
                                } catch (e: Exception) {
                                    failed(e.message ?: "network error")
                                }
                            }
                        }

                        webChromeClient = object : WebChromeClient() {
                            override fun onProgressChanged(view: WebView, newProgress: Int) { loading = newProgress }
                        }
                        webViewClient = object : WebViewClient() {
                            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                                val uri = request.url
                                if (uri.scheme == "http" || uri.scheme == "https") return false
                                // App links (e.g. "Open in Bambu Handy") go to the app that owns them.
                                runCatching {
                                    val intent = if (uri.scheme == "intent") Intent.parseUri(uri.toString(), Intent.URI_INTENT_SCHEME)
                                    else Intent(Intent.ACTION_VIEW, uri)
                                    intent.addCategory(Intent.CATEGORY_BROWSABLE)
                                    intent.component = null
                                    intent.selector = null
                                    ctx.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                                }.onFailure {
                                    scope.launch { snackbar.showSnackbar("No app on this phone opens that link.") }
                                }
                                return true
                            }
                        }
                        loadUrl(site.url)
                    }
                },
            )
            if (loading in 1..99) LinearProgressIndicator(
                progress = { loading / 100f }, modifier = Modifier.fillMaxWidth().align(Alignment.TopCenter), color = accent,
            )
            download?.let {
                Text(
                    "Downloading $it…",
                    Modifier.align(Alignment.BottomCenter).padding(16.dp),
                    style = MaterialTheme.typography.bodyMedium, color = accent,
                )
            }
        }
    }
}
