package app.unifiedslicer.companion.data

import app.unifiedslicer.companion.core.HubSnapshot
import app.unifiedslicer.companion.core.PrinterHub
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import okhttp3.OkHttpClient

/** Live printer status for the UI. The activity drives [refresh] on a timer while it is visible. */
class PrinterRepository(http: OkHttpClient, private val store: AccountStore, private val notifier: TransitionNotifier) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val hub = PrinterHub(SourceContext(http) { store.update(it) })
    private val running = Mutex()

    private val _snapshot = MutableStateFlow<HubSnapshot?>(null)
    val snapshot: StateFlow<HubSnapshot?> = _snapshot.asStateFlow()

    private val _refreshing = MutableStateFlow(false)
    val refreshing: StateFlow<Boolean> = _refreshing.asStateFlow()

    init {
        scope.launch {
            store.accounts.collect {
                hub.setAccounts(it)
                refresh()
            }
        }
    }

    /** Refreshes every account; a call while one is running is dropped. */
    suspend fun refresh() {
        if (!running.tryLock()) return
        try {
            _refreshing.value = true
            val snap = hub.refresh()
            _snapshot.value = snap
            notifier.process(snap)
        } finally {
            _refreshing.value = false
            running.unlock()
        }
    }

    fun refreshAsync() { scope.launch { refresh() } }

    fun disconnect() { scope.launch { hub.disconnect() } }
}
