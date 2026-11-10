package app.unifiedslicer.companion.data

import app.unifiedslicer.companion.core.HubSnapshot
import app.unifiedslicer.companion.core.PrinterHub
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.source.PrintJob
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.update
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
    private val hub = PrinterHub(SourceContext(http, { store.update(it) }, AgoraRtm))
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

    private val _pendingFile = MutableStateFlow<PrintJob?>(null)
    /** A printer-ready file waiting for the user to pick a printer. */
    val pendingFile: StateFlow<PrintJob?> = _pendingFile.asStateFlow()

    private val _send = MutableStateFlow<SendState?>(null)
    /** The send in progress or just finished; null when there is none. */
    val send: StateFlow<SendState?> = _send.asStateFlow()
    private var sendJob: Job? = null

    fun offerFile(job: PrintJob?) {
        _pendingFile.value = job
        if (job != null && _send.value?.running != true) _send.value = null
    }

    /** Sends in the app's scope, so leaving the screen does not abort an upload. */
    fun startSend(printer: PrinterStatus, job: PrintJob) {
        if (_send.value?.running == true) return
        _send.value = SendState(printer.name, job.name, 0, "Starting…")
        sendJob = scope.launch {
            try {
                hub.print(printer.accountId, printer.deviceId, job) { pct, status ->
                    _send.update { s -> s?.copy(percent = pct ?: s.percent, status = status ?: s.status) }
                }
                _send.update { it?.copy(percent = 100, status = "Print started", running = false, done = true) }
                refresh()
            } catch (e: CancellationException) {
                _send.update { it?.copy(running = false, error = "Cancelled") }
            } catch (e: Exception) {
                _send.update { it?.copy(running = false, error = e.message ?: e.javaClass.simpleName) }
            }
        }
    }

    fun cancelSend() { sendJob?.cancel() }

    fun clearSend() { if (_send.value?.running != true) _send.value = null }

    fun refreshAsync() { scope.launch { refresh() } }

    fun disconnect() { scope.launch { hub.disconnect() } }
}

data class SendState(
    val printer: String,
    val file: String,
    val percent: Int,
    val status: String,
    val running: Boolean = true,
    val done: Boolean = false,
    val error: String? = null,
)
