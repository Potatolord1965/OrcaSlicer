package app.unifiedslicer.companion.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.unifiedslicer.companion.core.HubSnapshot
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.source.PrintJob
import app.unifiedslicer.companion.core.source.printBlockedReason
import app.unifiedslicer.companion.core.source.printableBy
import app.unifiedslicer.companion.data.SendState
import app.unifiedslicer.companion.ui.add.FormScaffold
import app.unifiedslicer.companion.ui.add.Hint
import app.unifiedslicer.companion.ui.add.PrimaryButton
import app.unifiedslicer.companion.ui.theme.LocalStudio
import app.unifiedslicer.companion.ui.theme.NeutralAccent
import app.unifiedslicer.companion.ui.theme.color

private fun sizeText(bytes: Long) = if (bytes >= 1 shl 20) "%.1f MB".format(bytes / 1048576.0) else "${(bytes + 1023) / 1024} KB"

/** Pick a printer for a printer-ready file, then watch the upload and start. */
@Composable
fun SendScreen(
    job: PrintJob?,
    accounts: List<Account>,
    snapshot: HubSnapshot?,
    send: SendState?,
    onSend: (PrinterStatus, PrintJob) -> Unit,
    onCancel: () -> Unit,
    onClose: () -> Unit,
) {
    FormScaffold("Send to printer", onClose) {
        if (send != null) {
            SendProgress(send, onCancel, onClose)
            return@FormScaffold
        }
        if (job == null) {
            Hint("No file selected.")
            return@FormScaffold
        }
        Panel(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text(job.name, style = MaterialTheme.typography.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(sizeText(job.file.length()), style = MaterialTheme.typography.bodySmall, color = LocalStudio.current.muted)
            }
        }
        val byId = accounts.associateBy { it.id }
        val printers = snapshot?.printers.orEmpty()
        val able = printers.filter { p -> byId[p.accountId]?.let { printableBy(it, job.name) } == true }
        var selected by rememberSaveable { mutableStateOf<String?>(null) }
        val ready = able.filter { it.state != PrinterState.OFFLINE && !it.state.active }
        val choice = able.firstOrNull { it.key == selected && it in ready } ?: ready.singleOrNull()

        if (able.isEmpty()) {
            Hint("None of your printers can take this file. The phone cannot slice, so the file has to be G-code made for the printer.")
        } else {
            SectionLabel("Printer")
            able.forEach { p ->
                val enabled = p in ready
                Panel(Modifier.fillMaxWidth(), onClick = if (enabled) ({ selected = p.key }) else null) {
                    Row(Modifier.padding(start = 4.dp, end = 14.dp, top = 6.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(selected = choice?.key == p.key, onClick = if (enabled) ({ selected = p.key }) else null, enabled = enabled)
                        Column(Modifier.weight(1f)) {
                            Text(p.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(p.via, style = MaterialTheme.typography.bodySmall, color = LocalStudio.current.muted)
                        }
                        Spacer(Modifier.width(8.dp))
                        StateBadge(p.state, p.brand.color())
                    }
                }
            }
        }
        // Brands that take no prints from this app: say why once.
        accounts.filter { a -> printers.any { it.accountId == a.id } }.mapNotNull { printBlockedReason(it) }.distinct().forEach { Hint(it) }

        if (choice != null) Hint("The print starts right away. Make sure the bed is clear and the right filament is loaded.")
        PrimaryButton(
            if (choice != null) "Print on ${choice.name}" else "Print",
            busy = false,
            enabled = choice != null,
            accent = choice?.brand?.color() ?: NeutralAccent,
        ) { choice?.let { onSend(it, job) } }
        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun SendProgress(send: SendState, onCancel: () -> Unit, onClose: () -> Unit) {
    val studio = LocalStudio.current
    Panel(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Text(send.file, style = MaterialTheme.typography.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text("to ${send.printer}", style = MaterialTheme.typography.bodySmall, color = studio.muted)
            Spacer(Modifier.height(16.dp))
            AccentProgress(send.percent, if (send.error != null) studio.bad else if (send.done) studio.good else NeutralAccent)
            Spacer(Modifier.height(10.dp))
            Text(
                send.error ?: send.status,
                style = MaterialTheme.typography.bodyMedium,
                color = if (send.error != null) studio.bad else MaterialTheme.colorScheme.onSurface,
            )
        }
    }
    if (send.running) {
        OutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text("Cancel") }
        Hint("You can leave this screen; the send keeps going while the app is open.")
    } else {
        PrimaryButton(if (send.error != null) "Try again" else "Done", busy = false, onClick = onClose)
    }
}
