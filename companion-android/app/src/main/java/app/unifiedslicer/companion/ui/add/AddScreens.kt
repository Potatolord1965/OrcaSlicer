package app.unifiedslicer.companion.ui.add

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.MarkforgedAccount
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.source.bambu.BambuAuth
import app.unifiedslicer.companion.core.source.creality.CrealityApi
import app.unifiedslicer.companion.core.source.markforged.EigerSource
import app.unifiedslicer.companion.ui.Panel
import app.unifiedslicer.companion.ui.SectionLabel
import app.unifiedslicer.companion.ui.theme.LocalStudio
import app.unifiedslicer.companion.ui.theme.color
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import java.util.UUID

/** Routes of the add flows; [AddChoice.route] is appended to "add/". */
enum class AddChoice(val route: String, val title: String, val subtitle: String, val brand: Brand) {
    BAMBU("bambu", "Bambu Lab", "Bambu Cloud account", Brand.BAMBU),
    ELEGOO("elegoo", "Elegoo", "Elegoo Cloud account (Centauri Carbon 2)", Brand.ELEGOO),
    CREALITY("creality", "Creality", "Creality Cloud account", Brand.CREALITY),
    MARKFORGED("markforged", "Markforged", "Eiger organization API keys", Brand.MARKFORGED),
}

private fun friendly(e: Throwable): String = when (e) {
    is AuthException -> e.message ?: "Sign-in rejected"
    is java.net.UnknownHostException -> "No internet connection"
    else -> e.message ?: e.javaClass.simpleName
}

@Composable
fun AddScreen(onBack: () -> Unit, onPick: (String) -> Unit) {
    FormScaffold("Add", onBack) {
        SectionLabel("Cloud accounts")
        AddChoice.entries.forEach { c -> ChoiceRow(c.title, c.subtitle, c.brand) { onPick(c.route) } }
        SectionLabel("On your network")
        LanKind.entries.forEach { k -> ChoiceRow(k.label, "Local network", k.brand) { onPick("lan/${k.name}") } }
        Spacer(Modifier.padding(8.dp))
    }
}

@Composable
private fun ChoiceRow(title: String, subtitle: String, brand: Brand, onClick: () -> Unit) {
    Panel(Modifier.fillMaxWidth(), onClick = onClick) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(10.dp).clip(CircleShape).background(brand.color()))
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.bodyLarge)
                Text(subtitle, style = MaterialTheme.typography.bodySmall, color = LocalStudio.current.muted)
            }
            Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, null, tint = LocalStudio.current.muted)
        }
    }
}

@Composable
fun BambuLoginScreen(http: OkHttpClient, replaceId: String?, onBack: () -> Unit, onDone: (Account) -> Unit) {
    var email by rememberSaveable { mutableStateOf("") }
    var password by rememberSaveable { mutableStateOf("") }
    var region by rememberSaveable { mutableStateOf("global") }
    var code by rememberSaveable { mutableStateOf("") }
    var step by rememberSaveable { mutableStateOf("password") } // password | email-code | tfa
    var tfaKey by rememberSaveable { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    val auth = remember { BambuAuth(http) }
    fun done(a: BambuAccount) = onDone(if (replaceId != null) a.copy(id = replaceId) else a)

    FormScaffold("Bambu Lab", onBack) {
        Hint("Sign in with your Bambu Lab account. Printers bound to it in Bambu Handy or Bambu Studio appear here.")
        Choice(listOf("global" to "Global", "china" to "China"), region) { region = it }
        Field(email, { email = it }, "E-mail", keyboard = KeyboardType.Email)
        if (step == "password") {
            Field(password, { password = it }, "Password", secret = true)
        } else {
            Hint(if (step == "tfa") "Enter the code from your authenticator app." else "Bambu e-mailed you a verification code.")
            Field(code, { code = it }, "Code", keyboard = KeyboardType.Number)
        }
        ErrorText(error)
        PrimaryButton(if (step == "password") "Sign in" else "Verify", busy, accent = Brand.BAMBU.color()) {
            busy = true; error = null
            scope.launch {
                try {
                    when (step) {
                        "password" -> when (val r = auth.login(email.trim(), password, region)) {
                            is BambuAuth.Result.SignedIn -> done(r.account)
                            BambuAuth.Result.NeedsEmailCode -> step = "email-code"
                            is BambuAuth.Result.NeedsTfa -> { tfaKey = r.tfaKey; step = "tfa" }
                        }
                        "email-code" -> done(auth.loginWithCode(email.trim(), code, region))
                        else -> done(auth.loginWithTfa(email.trim(), tfaKey, code, region))
                    }
                } catch (e: Exception) {
                    error = friendly(e)
                } finally {
                    busy = false
                }
            }
        }
    }
}

@Composable
fun CrealityLoginScreen(http: OkHttpClient, replaceId: String?, onBack: () -> Unit, onDone: (Account) -> Unit) {
    var type by rememberSaveable { mutableStateOf(2) }
    var account by rememberSaveable { mutableStateOf("") }
    var password by rememberSaveable { mutableStateOf("") }
    var region by rememberSaveable { mutableStateOf("global") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    FormScaffold("Creality", onBack) {
        Hint("Sign in with your Creality Cloud account. Printers bound in Creality Print or the Creality Cloud app appear here.")
        Choice(listOf("global" to "Global", "china" to "China"), region) { region = it }
        Choice(listOf(2 to "E-mail", 1 to "Phone"), type) { type = it }
        Field(account, { account = it }, if (type == 2) "E-mail" else "Phone with country code", keyboard = if (type == 2) KeyboardType.Email else KeyboardType.Phone)
        Field(password, { password = it }, "Password", secret = true)
        ErrorText(error)
        PrimaryButton("Sign in", busy, accent = Brand.CREALITY.color()) {
            busy = true; error = null
            scope.launch {
                try {
                    val a = CrealityApi(http, region, CrealityApi.newDeviceUid()).login(type, account.trim(), password)
                    onDone(if (replaceId != null) a.copy(id = replaceId) else a)
                } catch (e: Exception) {
                    error = friendly(e)
                } finally {
                    busy = false
                }
            }
        }
    }
}

@Composable
fun MarkforgedScreen(http: OkHttpClient, replaceId: String?, onBack: () -> Unit, onDone: (Account) -> Unit) {
    var access by rememberSaveable { mutableStateOf("") }
    var secret by rememberSaveable { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    FormScaffold("Markforged", onBack) {
        Hint("Markforged printers report through Eiger. An organization admin creates API keys in Eiger under Settings > API Access.")
        Field(access, { access = it.trim() }, "Access key")
        Field(secret, { secret = it.trim() }, "Secret key", secret = true)
        ErrorText(error)
        PrimaryButton("Connect", busy, enabled = access.isNotEmpty() && secret.isNotEmpty(), accent = Brand.MARKFORGED.color()) {
            busy = true; error = null
            scope.launch {
                try {
                    val a = MarkforgedAccount(replaceId ?: ("markforged-" + UUID.randomUUID()), "Eiger", access, secret)
                    EigerSource(a, SourceContext(http)).refresh() // checks the keys
                    onDone(a)
                } catch (e: Exception) {
                    error = friendly(e)
                } finally {
                    busy = false
                }
            }
        }
    }
}

@Composable
fun LanPrinterScreen(kind: LanKind, existing: LanPrinter?, onBack: () -> Unit, onDone: (Account) -> Unit) {
    var name by rememberSaveable { mutableStateOf(existing?.label ?: "") }
    var host by rememberSaveable { mutableStateOf(existing?.host ?: "") }
    var secret by rememberSaveable { mutableStateOf(existing?.secret ?: "") }
    var serial by rememberSaveable { mutableStateOf(existing?.serial ?: "") }
    FormScaffold(kind.label, onBack) {
        Hint(
            when (kind) {
                LanKind.BAMBU -> "Turn on LAN mode (or LAN only liveview) on the printer. The IP address, access code and serial number are in the printer's network settings."
                LanKind.ELEGOO_CC2 -> "The IP address and access code are in the printer's Settings > Network. The default code is 123456."
                LanKind.MOONRAKER -> "Moonraker's address, e.g. 192.168.1.20 or 192.168.1.20:7125. An API key is only needed if Moonraker requires one."
                LanKind.OCTOPRINT -> "OctoPrint's address, e.g. octopi.local. Create an API key in OctoPrint under Settings > Application Keys."
                LanKind.PRUSALINK -> "The printer's IP address and the PrusaLink API key from the printer's network settings."
            }
        )
        Hint("Only reachable while your phone is on the same network (or a VPN to it).")
        Field(name, { name = it }, "Name")
        Field(host, { host = it.trim() }, "IP address or host", keyboard = KeyboardType.Uri)
        kind.secretLabel?.let { Field(secret, { secret = it.trim() }, it, secret = true) }
        if (kind.needsSerial) Field(serial, { serial = it.trim().uppercase() }, "Serial number")
        PrimaryButton("Save", busy = false, enabled = host.isNotBlank() && (!kind.needsSerial || serial.isNotBlank()), accent = kind.brand.color()) {
            onDone(
                LanPrinter(
                    id = existing?.id ?: ("lan-" + UUID.randomUUID()),
                    label = name.trim().ifEmpty { kind.label },
                    kind = kind,
                    host = host,
                    secret = secret,
                    serial = serial,
                )
            )
        }
    }
}
