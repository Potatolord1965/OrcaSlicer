package app.unifiedslicer.companion.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import app.unifiedslicer.companion.core.AccountProblem
import app.unifiedslicer.companion.core.HubSnapshot
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.ui.theme.LocalStudio
import app.unifiedslicer.companion.ui.theme.color

@Composable
fun AccountsScreen(
    accounts: List<Account>,
    snapshot: HubSnapshot?,
    onAdd: () -> Unit,
    onEdit: (Account) -> Unit,
    onRemove: (Account) -> Unit,
) {
    var confirm by remember { mutableStateOf<Account?>(null) }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(16.dp)) {
        item {
            Text("Accounts & printers", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(top = 8.dp))
            Text(
                "Sign-ins and access codes are stored encrypted on this phone only.",
                style = MaterialTheme.typography.bodySmall, color = LocalStudio.current.muted,
            )
        }
        val cloud = accounts.filter { it !is LanPrinter }
        val lan = accounts.filterIsInstance<LanPrinter>()
        if (cloud.isNotEmpty()) item { SectionLabel("Cloud") }
        items(cloud, key = { it.id }) { a -> AccountRow(a, snapshot, problem = snapshot?.problems?.get(a.id), onEdit, { confirm = a }) }
        if (lan.isNotEmpty()) item { SectionLabel("Local network") }
        items(lan, key = { it.id }) { a -> AccountRow(a, snapshot, problem = snapshot?.problems?.get(a.id), onEdit, { confirm = a }) }
        item {
            OutlinedButton(onClick = onAdd, modifier = Modifier.fillMaxWidth().padding(top = 20.dp)) {
                Icon(Icons.Filled.Add, null)
                Spacer(Modifier.width(8.dp))
                Text("Add printer or account")
            }
        }
    }
    confirm?.let { a ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text("Remove ${a.label}?") },
            text = { Text("Its printers disappear from the app. You can add it again any time.") },
            confirmButton = { TextButton(onClick = { onRemove(a); confirm = null }) { Text("Remove") } },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun AccountRow(
    a: Account,
    snapshot: HubSnapshot?,
    problem: AccountProblem?,
    onEdit: (Account) -> Unit,
    onRemove: () -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    val count = snapshot?.printers?.count { it.accountId == a.id } ?: 0
    Panel(Modifier.fillMaxWidth().padding(bottom = 8.dp)) {
        Row(Modifier.padding(start = 16.dp, top = 12.dp, bottom = 12.dp, end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(10.dp).clip(CircleShape).background(a.brand.color()))
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(a.label, style = MaterialTheme.typography.bodyLarge)
                val status = when {
                    problem?.signInExpired == true -> "Sign-in expired"
                    problem != null -> problem.message
                    a is LanPrinter -> a.kindLabel
                    else -> "${a.kindLabel} · " + if (count == 1) "1 printer" else "$count printers"
                }
                Text(
                    status, style = MaterialTheme.typography.bodySmall, maxLines = 2,
                    color = if (problem != null) LocalStudio.current.warn else LocalStudio.current.muted,
                )
            }
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, "More") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(
                        text = { Text(if (a is LanPrinter) "Edit" else "Sign in again") },
                        onClick = { menu = false; onEdit(a) },
                    )
                    DropdownMenuItem(text = { Text("Remove") }, onClick = { menu = false; onRemove() })
                }
            }
        }
    }
}
