package app.unifiedslicer.companion.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.Temperature
import app.unifiedslicer.companion.ui.theme.LocalStudio
import app.unifiedslicer.companion.ui.theme.NumberStyle
import app.unifiedslicer.companion.ui.theme.color

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PrinterDetailScreen(p: PrinterStatus?, onBack: () -> Unit) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(p?.name ?: "Printer", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") } },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background),
            )
        },
    ) { padding ->
        if (p == null) {
            Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
                Text("This printer is no longer on your accounts.", color = LocalStudio.current.muted)
            }
            return@Scaffold
        }
        val accent = p.brand.color()
        val muted = LocalStudio.current.muted
        Column(
            Modifier.fillMaxSize().padding(padding).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(8.dp))
            StateBadge(p.state, accent)
            Spacer(Modifier.height(20.dp))
            Box(contentAlignment = Alignment.Center) {
                val shown = p.progress != null && (p.state.active || p.state == PrinterState.FINISHED)
                CircularProgressIndicator(
                    progress = { if (shown) (p.progress ?: 0) / 100f else 0f },
                    modifier = Modifier.size(200.dp),
                    color = stateColor(p.state, accent),
                    trackColor = accent.copy(alpha = 0.14f),
                    strokeWidth = 12.dp,
                    strokeCap = StrokeCap.Round,
                    gapSize = 0.dp,
                )
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        if (shown) "${p.progress}%" else p.state.label,
                        style = MaterialTheme.typography.displaySmall.merge(NumberStyle).copy(fontSize = if (shown) 44.sp else 28.sp),
                    )
                    p.remainingSeconds?.let {
                        Text("${formatDuration(it)} left", style = MaterialTheme.typography.bodyMedium, color = muted)
                        Text("done ~${etaClock(it)}", style = MaterialTheme.typography.bodySmall, color = muted)
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
            p.file?.let {
                Text(it, style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            p.stage?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = accent) }
            p.alert?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = LocalStudio.current.bad) }
            Spacer(Modifier.height(20.dp))

            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                TempTile("Nozzle", p.nozzle, Modifier.weight(1f))
                TempTile("Bed", p.bed, Modifier.weight(1f))
                if (p.chamber != null) TempTile("Chamber", p.chamber, Modifier.weight(1f))
            }
            Spacer(Modifier.height(10.dp))
            Panel(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(horizontal = 16.dp, vertical = 6.dp)) {
                    if (p.layer != null && p.totalLayers != null) InfoRow("Layer", "${p.layer} / ${p.totalLayers}")
                    InfoRow("Model", p.model.ifEmpty { p.brand.label })
                    InfoRow("Connection", p.via)
                    InfoRow("Serial / ID", p.deviceId)
                    InfoRow("Updated", formatAgo(p.updatedAt), last = true)
                }
            }
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun TempTile(label: String, t: Temperature?, modifier: Modifier) {
    Panel(modifier) {
        Column(Modifier.padding(14.dp)) {
            Text(label.uppercase(), style = MaterialTheme.typography.labelSmall, color = LocalStudio.current.muted)
            Spacer(Modifier.height(6.dp))
            Text(t?.let { "${it.current.toInt()}°" } ?: "–", style = MaterialTheme.typography.titleLarge.merge(NumberStyle))
            t?.target?.let { Text("target ${it.toInt()}°", style = MaterialTheme.typography.bodySmall, color = LocalStudio.current.muted) }
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String, last: Boolean = false) {
    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp)) {
        Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium, color = LocalStudio.current.muted)
        Text(value, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
    if (!last) HorizontalDivider(color = LocalStudio.current.hairline)
}
