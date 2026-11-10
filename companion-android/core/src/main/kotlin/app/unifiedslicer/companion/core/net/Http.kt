package app.unifiedslicer.companion.core.net

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonElement
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okio.BufferedSink
import java.io.File
import java.io.IOException
import java.util.concurrent.TimeUnit

/** The account's sign-in is no longer accepted: the user must sign in again. */
class AuthException(message: String) : IOException(message)

/** An HTTP error with the server's status code and a short body excerpt. */
class HttpException(val code: Int, message: String, val body: String = "") : IOException(message)

const val USER_AGENT = "UnifiedSlicerCompanion/1.0 (Android)"

val JSON_MEDIA = "application/json; charset=utf-8".toMediaType()

fun defaultHttpClient(): OkHttpClient = OkHttpClient.Builder()
    .connectTimeout(10, TimeUnit.SECONDS)
    .readTimeout(20, TimeUnit.SECONDS)
    .callTimeout(30, TimeUnit.SECONDS)
    .build()

/** For uploads: no overall deadline, generous write and read time-outs. */
fun OkHttpClient.forUploads(): OkHttpClient = newBuilder()
    .callTimeout(0, TimeUnit.SECONDS)
    .writeTimeout(60, TimeUnit.SECONDS)
    .readTimeout(180, TimeUnit.SECONDS)
    .build()

/** A request body streaming [length] bytes of [file] from [offset], reporting bytes written so far. */
fun fileBody(
    file: File,
    contentType: String = "application/octet-stream",
    offset: Long = 0,
    length: Long = file.length() - offset,
    onProgress: (sent: Long) -> Unit = {},
): RequestBody = object : RequestBody() {
    override fun contentType() = contentType.toMediaType()
    override fun contentLength() = length
    override fun writeTo(sink: BufferedSink) {
        file.inputStream().use { input ->
            var toSkip = offset
            while (toSkip > 0) toSkip -= input.skip(toSkip).also { if (it <= 0) throw IOException("Cannot read ${file.name}") }
            val buf = ByteArray(64 * 1024)
            var left = length
            while (left > 0) {
                val n = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                if (n < 0) throw IOException("${file.name} is shorter than expected")
                sink.write(buf, 0, n)
                left -= n
                onProgress(length - left)
            }
        }
    }
}

/** Runs [request] off the main thread and returns the body text; non-2xx becomes [HttpException]. */
suspend fun OkHttpClient.fetchText(request: Request): String = withContext(Dispatchers.IO) {
    val withUa = if (request.header("User-Agent") == null) request.newBuilder().header("User-Agent", USER_AGENT).build() else request
    newCall(withUa).execute().use { resp ->
        val text = resp.body?.string().orEmpty()
        if (resp.code == 401 || resp.code == 403) throw AuthException("The printer or service rejected the credentials (HTTP ${resp.code})")
        if (!resp.isSuccessful) throw HttpException(resp.code, "HTTP ${resp.code} from ${request.url.host}", text.take(2000))
        text
    }
}

fun md5(file: File): ByteArray {
    val digest = java.security.MessageDigest.getInstance("MD5")
    file.inputStream().use { input ->
        val buf = ByteArray(64 * 1024)
        while (true) {
            val n = input.read(buf)
            if (n < 0) break
            digest.update(buf, 0, n)
        }
    }
    return digest.digest()
}

fun ByteArray.toHex(upper: Boolean = false): String = joinToString("") { (if (upper) "%02X" else "%02x").format(it) }

fun jsonBody(json: JsonElement) = json.toString().toRequestBody(JSON_MEDIA)
fun jsonBody(json: String) = json.toRequestBody(JSON_MEDIA)

/**
 * Runs [request] off the main thread and returns the parsed JSON body.
 * 401/403 become [AuthException]; other non-2xx statuses become [HttpException].
 */
suspend fun OkHttpClient.fetchJson(request: Request, authErrors: Boolean = true): JsonElement = withContext(Dispatchers.IO) {
    val withUa = if (request.header("User-Agent") == null) request.newBuilder().header("User-Agent", USER_AGENT).build() else request
    newCall(withUa).execute().use { resp ->
        val text = resp.body?.string().orEmpty()
        if (authErrors && (resp.code == 401 || resp.code == 403))
            throw AuthException("Sign-in rejected (HTTP ${resp.code})")
        if (!resp.isSuccessful)
            throw HttpException(resp.code, "HTTP ${resp.code} from ${request.url.host}", text.take(2000))
        parseJson(text.ifEmpty { "{}" }) ?: throw IOException("Unexpected reply from ${request.url.host}")
    }
}

/** Accepts "192.168.1.5", "192.168.1.5:7125" or "http://host/path" and returns a base URL without a trailing slash. */
fun baseUrl(host: String, defaultScheme: String = "http"): String {
    val h = host.trim().trimEnd('/')
    return if (h.startsWith("http://", true) || h.startsWith("https://", true)) h else "$defaultScheme://$h"
}

/** Host part of [host] as entered by the user (no scheme, port or path). */
fun bareHost(host: String): String =
    host.trim().substringAfter("://").substringBefore('/').let { h ->
        if (h.startsWith("[")) h.substringBefore(']').removePrefix("[") else h.substringBefore(':')
    }
