package app.unifiedslicer.companion

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import app.unifiedslicer.companion.ui.CompanionNav
import app.unifiedslicer.companion.ui.theme.StudioTheme
import app.unifiedslicer.companion.ui.theme.isDark
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private val app get() = application as CompanionApp
    private var openPrinter by mutableStateOf<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        openPrinter = intent?.getStringExtra(EXTRA_PRINTER)

        if (Build.VERSION.SDK_INT >= 33 && app.settings.notifications.value &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        )
            registerForActivityResult(ActivityResultContracts.RequestPermission()) {}.launch(Manifest.permission.POST_NOTIFICATIONS)

        setContent {
            val mode by app.settings.theme.collectAsStateWithLifecycle()
            val dark = isDark(mode)
            DisposableEffect(dark) {
                val bars = if (dark) SystemBarStyle.dark(android.graphics.Color.TRANSPARENT)
                else SystemBarStyle.light(android.graphics.Color.TRANSPARENT, android.graphics.Color.TRANSPARENT)
                enableEdgeToEdge(statusBarStyle = bars, navigationBarStyle = bars)
                onDispose {}
            }
            StudioTheme(dark) {
                androidx.compose.material3.Surface(color = androidx.compose.material3.MaterialTheme.colorScheme.background) {
                    CompanionNav(app, dark, openPrinter) { openPrinter = null }
                }
            }
        }

        // Live view: poll while visible, drop connections when hidden.
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                app.inForeground = true
                try {
                    while (true) {
                        app.printers.refresh()
                        delay(app.settings.refreshSeconds.value * 1000L)
                    }
                } finally {
                    app.inForeground = false
                    app.printers.disconnect()
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        intent.getStringExtra(EXTRA_PRINTER)?.let { openPrinter = it }
    }

    companion object {
        const val EXTRA_PRINTER = "printer"
    }
}
