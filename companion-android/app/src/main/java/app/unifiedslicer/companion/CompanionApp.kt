package app.unifiedslicer.companion

import android.app.Application
import app.unifiedslicer.companion.core.net.defaultHttpClient
import app.unifiedslicer.companion.data.AccountStore
import app.unifiedslicer.companion.data.PrinterRepository
import app.unifiedslicer.companion.data.Settings
import app.unifiedslicer.companion.data.StatusWorker
import app.unifiedslicer.companion.data.TransitionNotifier

class CompanionApp : Application() {
    val http by lazy { defaultHttpClient() }
    lateinit var accounts: AccountStore private set
    lateinit var settings: Settings private set
    lateinit var notifier: TransitionNotifier private set
    lateinit var printers: PrinterRepository private set

    /** True while an activity is visible; the background check then leaves the work to the live view. */
    @Volatile var inForeground = false

    override fun onCreate() {
        super.onCreate()
        accounts = AccountStore(this)
        settings = Settings(this)
        notifier = TransitionNotifier(this, settings)
        printers = PrinterRepository(http, accounts, notifier)
        StatusWorker.schedule(this, settings.backgroundChecks.value)
    }
}
