package app.unifiedslicer.companion.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.data.SendState
import app.unifiedslicer.companion.ui.theme.LocalStudio
import app.unifiedslicer.companion.ui.theme.color

/** Model sites that offer printer-ready files, opened inside the app. */
enum class ModelSite(val title: String, val subtitle: String, val url: String, val brand: Brand) {
    MAKERWORLD("MakerWorld", "Bambu Lab · Print hands the job to Bambu Handy", "https://makerworld.com/", Brand.BAMBU),
    NEXPRINT("Nexprint", "Elegoo · download a sliced file to print it", "https://www.nexprint.com/", Brand.ELEGOO),
    CREALITY("Creality Cloud", "Creality · download a sliced file to print it", "https://www.crealitycloud.com/", Brand.CREALITY),
}

@Composable
fun ModelsScreen(send: SendState?, onOpenSite: (ModelSite) -> Unit, onPickFile: () -> Unit, onShowSend: () -> Unit) {
    val muted = LocalStudio.current.muted
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
        Text("Find something to print", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(top = 8.dp))
        Text(
            "Browse a model site and download a sliced file; the app then offers to send it to a printer.",
            style = MaterialTheme.typography.bodySmall, color = muted,
        )
        if (send != null) {
            Spacer(Modifier.height(12.dp))
            Panel(Modifier.fillMaxWidth(), onClick = onShowSend) {
                Column(Modifier.padding(14.dp)) {
                    Text("${send.file} → ${send.printer}", style = MaterialTheme.typography.bodyMedium, maxLines = 1)
                    Spacer(Modifier.height(8.dp))
                    AccentProgress(send.percent, if (send.error != null) LocalStudio.current.bad else LocalStudio.current.good)
                    Spacer(Modifier.height(6.dp))
                    Text(send.error ?: send.status, style = MaterialTheme.typography.bodySmall, color = muted)
                }
            }
        }
        SectionLabel("Model sites")
        ModelSite.entries.forEach { site ->
            Panel(Modifier.fillMaxWidth().padding(bottom = 8.dp), onClick = { onOpenSite(site) }) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(10.dp).clip(CircleShape).background(site.brand.color()))
                    Spacer(Modifier.width(14.dp))
                    Column(Modifier.weight(1f)) {
                        Text(site.title, style = MaterialTheme.typography.bodyLarge)
                        Text(site.subtitle, style = MaterialTheme.typography.bodySmall, color = muted)
                    }
                    Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, null, tint = muted)
                }
            }
        }
        SectionLabel("From this phone")
        Panel(Modifier.fillMaxWidth(), onClick = onPickFile) {
            Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Outlined.FolderOpen, null, Modifier.size(20.dp), tint = muted)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text("Print a file", style = MaterialTheme.typography.bodyLarge)
                    Text("G-code saved on the phone or in a cloud drive", style = MaterialTheme.typography.bodySmall, color = muted)
                }
            }
        }
    }
}
