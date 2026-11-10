package app.unifiedslicer.companion.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Layers
import androidx.compose.material.icons.outlined.Schedule
import androidx.compose.material.icons.outlined.WarningAmber
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.ui.theme.LocalStudio
import app.unifiedslicer.companion.ui.theme.NumberStyle
import app.unifiedslicer.companion.ui.theme.color

@Composable
fun stateColor(state: PrinterState, accent: Color): Color {
    val s = LocalStudio.current
    return when (state) {
        PrinterState.PRINTING, PrinterState.PREPARING -> accent
        PrinterState.PAUSED -> s.warn
        PrinterState.FINISHED -> s.good
        PrinterState.FAILED -> s.bad
        PrinterState.IDLE -> MaterialTheme.colorScheme.onSurface.copy(alpha = 0.75f)
        PrinterState.OFFLINE, PrinterState.UNKNOWN -> s.muted
    }
}

@Composable
fun StateBadge(state: PrinterState, accent: Color, modifier: Modifier = Modifier) {
    val c = stateColor(state, accent)
    Row(
        modifier
            .clip(RoundedCornerShape(50))
            .background(c.copy(alpha = 0.14f))
            .padding(horizontal = 10.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(7.dp).clip(CircleShape).background(c))
        Spacer(Modifier.width(6.dp))
        Text(state.label, style = MaterialTheme.typography.labelMedium, color = c)
    }
}

@Composable
fun Panel(modifier: Modifier = Modifier, onClick: (() -> Unit)? = null, content: @Composable () -> Unit) {
    val shape = RoundedCornerShape(16.dp)
    Box(
        modifier
            .clip(shape)
            .background(LocalStudio.current.panel)
            .border(1.dp, LocalStudio.current.hairline, shape)
            .let { if (onClick != null) it.clickable(onClick = onClick) else it },
    ) { content() }
}

@Composable
fun AccentProgress(progress: Int, accent: Color, modifier: Modifier = Modifier) {
    LinearProgressIndicator(
        progress = { progress.coerceIn(0, 100) / 100f },
        modifier = modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
        color = accent,
        trackColor = accent.copy(alpha = 0.16f),
        drawStopIndicator = {},
    )
}

@Composable
private fun Meta(icon: ImageVector, text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, null, Modifier.size(14.dp), tint = LocalStudio.current.muted)
        Spacer(Modifier.width(4.dp))
        Text(text, style = MaterialTheme.typography.bodySmall, color = LocalStudio.current.muted)
    }
}

@Composable
fun PrinterCard(p: PrinterStatus, onClick: () -> Unit) {
    val accent = p.brand.color()
    val muted = LocalStudio.current.muted
    Panel(Modifier.fillMaxWidth(), onClick = onClick) {
        Row(Modifier.height(IntrinsicSize.Min)) {
            Box(Modifier.width(4.dp).fillMaxHeight().background(accent))
            Column(Modifier.padding(start = 14.dp, end = 16.dp, top = 14.dp, bottom = 14.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(p.name, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(
                            listOf(p.model.ifEmpty { p.brand.label }, p.via).joinToString(" · "),
                            style = MaterialTheme.typography.bodySmall, color = muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                        )
                    }
                    Spacer(Modifier.width(8.dp))
                    StateBadge(p.state, accent)
                }
                if (p.state.active) {
                    Spacer(Modifier.height(12.dp))
                    Row(verticalAlignment = Alignment.Bottom) {
                        Text(p.file ?: p.stage ?: "", Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                        p.progress?.let { Text("$it%", style = MaterialTheme.typography.titleMedium.merge(NumberStyle)) }
                    }
                    Spacer(Modifier.height(8.dp))
                    AccentProgress(p.progress ?: 0, accent)
                    Spacer(Modifier.height(8.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                        p.remainingSeconds?.let { Meta(Icons.Outlined.Schedule, "${formatDuration(it)} left") }
                        if (p.layer != null && p.totalLayers != null) Meta(Icons.Outlined.Layers, "${p.layer}/${p.totalLayers}")
                        p.stage?.takeIf { p.file != null }?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = muted, maxLines = 1) }
                    }
                } else {
                    Spacer(Modifier.height(8.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
                        if (p.nozzle != null) Text("Nozzle ${formatTemp(p.nozzle)}", style = MaterialTheme.typography.bodySmall, color = muted)
                        if (p.bed != null) Text("Bed ${formatTemp(p.bed)}", style = MaterialTheme.typography.bodySmall, color = muted)
                        p.alert?.let { Meta(Icons.Outlined.WarningAmber, it) }
                    }
                }
            }
        }
    }
}

@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text.uppercase(),
        modifier.padding(top = 20.dp, bottom = 8.dp),
        style = MaterialTheme.typography.labelSmall,
        color = LocalStudio.current.muted,
    )
}
