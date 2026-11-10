package app.unifiedslicer.companion.data

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import app.unifiedslicer.companion.MainActivity
import app.unifiedslicer.companion.R
import app.unifiedslicer.companion.core.HubSnapshot
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus

/**
 * Remembers each printer's last state and posts a notification when a print finishes, fails or
 * pauses. Shared by the live view and the background check so neither notifies twice.
 */
class TransitionNotifier(private val context: Context, private val settings: Settings) {
    private val prefs = context.getSharedPreferences("last_states", Context.MODE_PRIVATE)

    init {
        val channel = NotificationChannel(CHANNEL, "Print status", NotificationManager.IMPORTANCE_DEFAULT).apply {
            description = "A print finished, failed or paused"
        }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    @Synchronized
    fun process(snapshot: HubSnapshot) {
        val edit = prefs.edit()
        for (p in snapshot.printers) {
            val old = prefs.getString(p.key, null)?.let { runCatching { PrinterState.valueOf(it) }.getOrNull() }
            // A printer that drops off-line keeps its last real state, so coming back does not re-notify.
            if (p.state == PrinterState.OFFLINE || p.state == PrinterState.UNKNOWN) continue
            edit.putString(p.key, p.state.name)
            if (old != null && old != p.state) message(old, p)?.let { notify(p, it) }
        }
        edit.apply()
    }

    private fun message(old: PrinterState, p: PrinterStatus): String? = when (p.state) {
        PrinterState.FINISHED -> if (old.active) "Print finished" else null
        PrinterState.FAILED -> if (old.active) "Print failed or was stopped" else null
        PrinterState.PAUSED -> if (old == PrinterState.PRINTING || old == PrinterState.PREPARING) "Print paused" else null
        else -> null
    }

    private fun notify(p: PrinterStatus, title: String) {
        if (!settings.notifications.value) return
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) return
        val open = PendingIntent.getActivity(
            context, p.key.hashCode(),
            Intent(context, MainActivity::class.java).putExtra(MainActivity.EXTRA_PRINTER, p.key)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val text = listOfNotNull(p.file, p.alert ?: p.stage).joinToString(" · ").ifEmpty { p.via }
        val n = NotificationCompat.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_logo)
            .setColor(p.brand.accent.toInt())
            .setContentTitle("${p.name}: $title")
            .setContentText(text)
            .setContentIntent(open)
            .setAutoCancel(true)
            .build()
        NotificationManagerCompat.from(context).notify(p.key.hashCode(), n)
    }

    private companion object {
        const val CHANNEL = "print_status"
    }
}
