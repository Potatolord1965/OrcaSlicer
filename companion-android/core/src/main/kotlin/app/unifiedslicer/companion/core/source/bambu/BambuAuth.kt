package app.unifiedslicer.companion.core.source.bambu

import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.HttpException
import app.unifiedslicer.companion.core.net.USER_AGENT
import app.unifiedslicer.companion.core.net.fetchJson
import app.unifiedslicer.companion.core.net.jsonBody
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.net.str
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException
import java.util.UUID

/** Bambu Lab account sign-in (the same REST API Bambu Studio and Bambu Handy use). */
class BambuAuth(private val http: OkHttpClient) {
    sealed interface Result {
        data class SignedIn(val account: BambuAccount) : Result
        /** Bambu e-mailed a one-time code; finish with [loginWithCode]. */
        data object NeedsEmailCode : Result
        /** The account has two-factor authentication; finish with [loginWithTfa]. */
        data class NeedsTfa(val tfaKey: String) : Result
    }

    suspend fun login(email: String, password: String, region: String): Result {
        val reply = post(region, "/v1/user-service/user/login", buildJsonObject {
            put("account", email); put("password", password); put("apiError", "")
        })
        reply.str("accessToken")?.let { return Result.SignedIn(finish(email, it, reply.str("refreshToken"), region)) }
        return when (reply.str("loginType")) {
            "verifyCode" -> { sendEmailCode(email, region); Result.NeedsEmailCode }
            "tfa" -> Result.NeedsTfa(reply.str("tfaKey") ?: throw IOException("Bambu asked for 2FA without a key"))
            else -> throw AuthException("Bambu did not accept the e-mail or password")
        }
    }

    suspend fun sendEmailCode(email: String, region: String) {
        post(region, "/v1/user-service/user/sendemail/code", buildJsonObject { put("email", email); put("type", "codeLogin") })
    }

    suspend fun loginWithCode(email: String, code: String, region: String): BambuAccount {
        val reply = post(region, "/v1/user-service/user/login", buildJsonObject { put("account", email); put("code", code.trim()) })
        val token = reply.str("accessToken") ?: throw AuthException("Bambu did not accept the code")
        return finish(email, token, reply.str("refreshToken"), region)
    }

    suspend fun loginWithTfa(email: String, tfaKey: String, code: String, region: String): BambuAccount = withContext(Dispatchers.IO) {
        val site = if (region == "china") "https://bambulab.cn" else "https://bambulab.com"
        val req = Request.Builder().url("$site/api/sign-in/tfa")
            .header("User-Agent", USER_AGENT)
            .post(jsonBody(buildJsonObject { put("tfaKey", tfaKey); put("tfaCode", code.trim()) }))
            .build()
        val token = http.newCall(req).execute().use { resp ->
            val cookie = resp.headers("Set-Cookie").firstNotNullOfOrNull { c ->
                c.split(';').first().trim().takeIf { it.startsWith("token=") }?.removePrefix("token=")
            }
            cookie ?: parseJson(resp.body?.string().orEmpty()).str("accessToken")
        } ?: throw AuthException("Bambu did not accept the two-factor code")
        finish(email, token, null, region)
    }

    suspend fun refresh(account: BambuAccount): BambuAccount {
        if (account.refreshToken.isEmpty()) throw AuthException("Bambu sign-in expired")
        val reply = try {
            post(account.region, "/v1/user-service/user/refreshtoken", buildJsonObject { put("refreshToken", account.refreshToken) })
        } catch (e: HttpException) {
            throw AuthException("Bambu sign-in expired")
        }
        val token = reply.str("accessToken") ?: throw AuthException("Bambu sign-in expired")
        return account.copy(accessToken = token, refreshToken = reply.str("refreshToken") ?: account.refreshToken)
    }

    private suspend fun finish(email: String, token: String, refreshToken: String?, region: String): BambuAccount {
        val pref = http.fetchJson(
            Request.Builder().url(apiBase(region) + "/v1/design-user-service/my/preference")
                .header("Authorization", "Bearer $token").build()
        )
        val uid = pref.str("uid") ?: throw IOException("Bambu returned no user id")
        return BambuAccount(
            id = "bambu-" + UUID.randomUUID(),
            label = email,
            region = region,
            accessToken = token,
            refreshToken = refreshToken.orEmpty(),
            uid = uid,
        )
    }

    private suspend fun post(region: String, path: String, body: kotlinx.serialization.json.JsonObject) = try {
        http.fetchJson(Request.Builder().url(apiBase(region) + path).post(jsonBody(body)).build(), authErrors = false)
    } catch (e: HttpException) {
        // Bambu answers a wrong password with 400 and a JSON message.
        val msg = parseJson(e.body).str("error")
        if (e.code in 400..499) throw AuthException(msg ?: "Bambu rejected the sign-in") else throw e
    }

    companion object {
        fun apiBase(region: String) = if (region == "china") "https://api.bambulab.cn" else "https://api.bambulab.com"
        fun mqttHost(region: String) = if (region == "china") "cn.mqtt.bambulab.com" else "us.mqtt.bambulab.com"
    }
}
