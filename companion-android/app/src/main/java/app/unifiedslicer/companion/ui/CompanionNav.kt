package app.unifiedslicer.companion.ui

import android.net.Uri
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.outlined.AccountCircle
import androidx.compose.material.icons.outlined.Print
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.navArgument
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import app.unifiedslicer.companion.CompanionApp
import app.unifiedslicer.companion.R
import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.CrealityAccount
import app.unifiedslicer.companion.core.model.ElegooAccount
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.MarkforgedAccount
import app.unifiedslicer.companion.data.StatusWorker
import app.unifiedslicer.companion.ui.add.AddScreen
import app.unifiedslicer.companion.ui.add.BambuLoginScreen
import app.unifiedslicer.companion.ui.add.CrealityLoginScreen
import app.unifiedslicer.companion.ui.add.ElegooLoginScreen
import app.unifiedslicer.companion.ui.add.LanPrinterScreen
import app.unifiedslicer.companion.ui.add.MarkforgedScreen

private val replaceArg = listOf(navArgument("replace") { type = NavType.StringType; nullable = true; defaultValue = null })

private fun editRoute(a: Account) = when (a) {
    is BambuAccount -> "add/bambu?replace=${a.id}"
    is ElegooAccount -> "add/elegoo?replace=${a.id}"
    is CrealityAccount -> "add/creality?replace=${a.id}"
    is MarkforgedAccount -> "add/markforged?replace=${a.id}"
    is LanPrinter -> "add/lan/${a.kind.name}?replace=${a.id}"
}

@Composable
fun CompanionNav(app: CompanionApp, dark: Boolean, openPrinter: String?, onPrinterOpened: () -> Unit) {
    val nav = rememberNavController()
    val accounts by app.accounts.accounts.collectAsStateWithLifecycle()
    val snapshot by app.printers.snapshot.collectAsStateWithLifecycle()
    val refreshing by app.printers.refreshing.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableIntStateOf(0) }

    LaunchedEffect(openPrinter) {
        if (openPrinter != null) {
            nav.navigate("printer/" + Uri.encode(openPrinter))
            onPrinterOpened()
        }
    }

    fun saved(a: Account, nav: NavHostController) {
        app.accounts.add(a)
        tab = 0
        nav.popBackStack("home", inclusive = false)
    }

    NavHost(nav, startDestination = "home") {
        composable("home") {
            HomeScaffold(tab, onTab = { tab = it }, onAdd = { nav.navigate("add") }) {
                when (tab) {
                    0 -> PrintersScreen(
                        accounts, snapshot, refreshing,
                        onRefresh = { app.printers.refreshAsync() },
                        onOpen = { nav.navigate("printer/" + Uri.encode(it)) },
                        onAdd = { nav.navigate("add") },
                        onAccounts = { tab = 1 },
                    )
                    1 -> AccountsScreen(
                        accounts, snapshot,
                        onAdd = { nav.navigate("add") },
                        onEdit = { nav.navigate(editRoute(it)) },
                        onRemove = { app.accounts.remove(it.id) },
                    )
                    else -> SettingsScreen(app.settings, appVersion(app)) { StatusWorker.schedule(app, it) }
                }
            }
        }
        composable("printer/{key}") { entry ->
            val key = entry.arguments?.getString("key")?.let { Uri.decode(it) }
            PrinterDetailScreen(snapshot?.printers?.firstOrNull { it.key == key }) { nav.popBackStack() }
        }
        composable("add") { AddScreen(onBack = { nav.popBackStack() }, onPick = { nav.navigate("add/$it") }) }
        composable("add/bambu?replace={replace}", arguments = replaceArg) { e ->
            BambuLoginScreen(app.http, e.arguments?.getString("replace"), { nav.popBackStack() }) { saved(it, nav) }
        }
        composable("add/elegoo?replace={replace}", arguments = replaceArg) { e ->
            ElegooLoginScreen(e.arguments?.getString("replace"), dark, { nav.popBackStack() }) { saved(it, nav) }
        }
        composable("add/creality?replace={replace}", arguments = replaceArg) { e ->
            CrealityLoginScreen(app.http, e.arguments?.getString("replace"), { nav.popBackStack() }) { saved(it, nav) }
        }
        composable("add/markforged?replace={replace}", arguments = replaceArg) { e ->
            MarkforgedScreen(app.http, e.arguments?.getString("replace"), { nav.popBackStack() }) { saved(it, nav) }
        }
        composable("add/lan/{kind}?replace={replace}", arguments = replaceArg) { e ->
            val kind = runCatching { LanKind.valueOf(e.arguments?.getString("kind").orEmpty()) }.getOrDefault(LanKind.MOONRAKER)
            val existing = e.arguments?.getString("replace")?.let { id -> accounts.firstOrNull { it.id == id } as? LanPrinter }
            LanPrinterScreen(kind, existing, { nav.popBackStack() }) { saved(it, nav) }
        }
    }
}

private fun appVersion(app: CompanionApp): String =
    runCatching { app.packageManager.getPackageInfo(app.packageName, 0).versionName }.getOrNull().orEmpty()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun HomeScaffold(tab: Int, onTab: (Int) -> Unit, onAdd: () -> Unit, content: @Composable () -> Unit) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(painterResource(R.drawable.ic_logo), null, Modifier.size(22.dp), tint = MaterialTheme.colorScheme.onBackground)
                        Spacer(Modifier.width(10.dp))
                        Text("Unified", style = MaterialTheme.typography.titleLarge)
                    }
                },
                actions = { IconButton(onClick = onAdd) { Icon(Icons.Filled.Add, "Add printer or account") } },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background),
            )
        },
        bottomBar = {
            NavigationBar(containerColor = MaterialTheme.colorScheme.surfaceContainerLow) {
                val items = listOf(Icons.Outlined.Print to "Printers", Icons.Outlined.AccountCircle to "Accounts", Icons.Outlined.Settings to "Settings")
                items.forEachIndexed { i, (icon, label) ->
                    NavigationBarItem(
                        selected = tab == i,
                        onClick = { onTab(i) },
                        icon = { Icon(icon, null) },
                        label = { Text(label) },
                        colors = NavigationBarItemDefaults.colors(indicatorColor = MaterialTheme.colorScheme.secondaryContainer),
                    )
                }
            }
        },
    ) { padding ->
        androidx.compose.foundation.layout.Box(Modifier.padding(padding)) { content() }
    }
}
