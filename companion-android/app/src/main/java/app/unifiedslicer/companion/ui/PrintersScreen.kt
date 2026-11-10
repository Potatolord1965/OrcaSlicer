package app.unifiedslicer.companion.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.unifiedslicer.companion.R
import app.unifiedslicer.companion.core.HubSnapshot
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.ui.theme.LocalStudio

private fun order(s: PrinterState) = when (s) {
    PrinterState.PRINTING, PrinterState.PREPARING, PrinterState.PAUSED -> 0
    PrinterState.FAILED -> 1
    PrinterState.FINISHED -> 2
    PrinterState.IDLE -> 3
    PrinterState.UNKNOWN -> 4
    PrinterState.OFFLINE -> 5
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PrintersScreen(
    accounts: List<Account>,
    snapshot: HubSnapshot?,
    refreshing: Boolean,
    onRefresh: () -> Unit,
    onOpen: (String) -> Unit,
    onAdd: () -> Unit,
    onAccounts: () -> Unit,
) {
    if (accounts.isEmpty()) {
        EmptyState(onAdd)
        return
    }
    val printers = snapshot?.printers.orEmpty().sortedWith(compareBy({ order(it.state) }, { it.name.lowercase() }))
    val problems = snapshot?.problems.orEmpty().values.toList()
    val muted = LocalStudio.current.muted
    PullToRefreshBox(isRefreshing = refreshing, onRefresh = onRefresh, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            item {
                val active = printers.count { it.state.active }
                val online = printers.count { it.state != PrinterState.OFFLINE }
                Column(Modifier.padding(top = 8.dp, bottom = 6.dp)) {
                    Text(
                        when {
                            snapshot == null -> "Connecting…"
                            active > 0 -> if (active == 1) "1 printer working" else "$active printers working"
                            else -> "All quiet"
                        },
                        style = MaterialTheme.typography.headlineSmall,
                    )
                    if (snapshot != null) Text(
                        "$online of ${printers.size} online · updated ${formatAgo(snapshot.at)}",
                        style = MaterialTheme.typography.bodySmall, color = muted,
                    )
                }
            }
            items(problems, key = { "problem-" + it.accountId }) { pr ->
                val name = accounts.firstOrNull { it.id == pr.accountId }?.let { "${it.kindLabel} (${it.label})" } ?: "Account"
                Panel(Modifier.fillMaxWidth(), onClick = onAccounts) {
                    Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Outlined.ErrorOutline, null, tint = LocalStudio.current.warn)
                        Spacer(Modifier.width(12.dp))
                        Column {
                            Text(name, style = MaterialTheme.typography.bodyMedium)
                            Text(
                                if (pr.signInExpired) "Sign-in expired. Tap to sign in again." else pr.message,
                                style = MaterialTheme.typography.bodySmall, color = muted, maxLines = 2,
                            )
                        }
                    }
                }
            }
            items(printers, key = { it.key }) { p -> PrinterCard(p) { onOpen(p.key) } }
            if (snapshot != null && printers.isEmpty() && problems.isEmpty()) item {
                Text("No printers found on your accounts yet.", Modifier.padding(top = 24.dp), color = muted)
            }
        }
    }
}

@Composable
private fun EmptyState(onAdd: () -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box(contentAlignment = Alignment.Center) {
            Icon(painterResource(R.drawable.ic_logo), null, Modifier.size(72.dp), tint = MaterialTheme.colorScheme.onBackground)
        }
        Spacer(Modifier.height(24.dp))
        Text("All your printers, one place", style = MaterialTheme.typography.titleLarge, textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))
        Text(
            "Sign in to Bambu Lab, Elegoo, Creality or Markforged, or add a printer on your network.",
            style = MaterialTheme.typography.bodyMedium, color = LocalStudio.current.muted, textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(24.dp))
        Button(onClick = onAdd, colors = ButtonDefaults.buttonColors()) { Text("Add printer or account") }
    }
}
