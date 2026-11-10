package app.unifiedslicer.companion.data

import android.content.Context
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import app.unifiedslicer.companion.CompanionApp
import app.unifiedslicer.companion.core.PrinterHub
import app.unifiedslicer.companion.core.source.SourceContext
import java.util.concurrent.TimeUnit

/** Checks every printer every 15 minutes while the app is closed, so finished prints still notify. */
class StatusWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val app = applicationContext as CompanionApp
        val accounts = app.accounts.accounts.value
        // While the app is open its live view already watches (and renews tokens); two clients
        // renewing one Elegoo session at once would sign it out.
        if (app.inForeground || accounts.isEmpty()) return Result.success()
        val hub = PrinterHub(SourceContext(app.http) { app.accounts.update(it) })
        try {
            hub.setAccounts(accounts)
            app.notifier.process(hub.refresh(timeoutMs = 20_000))
        } finally {
            hub.close()
        }
        return Result.success()
    }

    companion object {
        private const val NAME = "printer-status"

        fun schedule(context: Context, enabled: Boolean) {
            val wm = WorkManager.getInstance(context)
            if (!enabled) {
                wm.cancelUniqueWork(NAME)
                return
            }
            val request = PeriodicWorkRequestBuilder<StatusWorker>(15, TimeUnit.MINUTES)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
                .build()
            wm.enqueueUniquePeriodicWork(NAME, ExistingPeriodicWorkPolicy.KEEP, request)
        }
    }
}
