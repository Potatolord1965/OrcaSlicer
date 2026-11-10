package app.unifiedslicer.companion.core

import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.source.createSource
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeout

/** Why an account's printers could not be read. */
data class AccountProblem(val accountId: String, val message: String, val signInExpired: Boolean)

data class HubSnapshot(
    val printers: List<PrinterStatus>,
    val problems: Map<String, AccountProblem>,
    val at: Long = System.currentTimeMillis(),
)

/**
 * Keeps one [PrinterSource] per account and refreshes them all in parallel. Accounts that fail
 * keep their last known printers so a dropped connection does not empty the list.
 */
class PrinterHub(private val ctx: SourceContext, private val factory: (Account, SourceContext) -> PrinterSource = ::createSource) {
    private val lock = Mutex()
    private val sources = LinkedHashMap<String, Pair<Account, PrinterSource>>()
    private val lastGood = HashMap<String, List<PrinterStatus>>()

    /** Makes the hub follow [accounts]: new ones get a source, removed or edited ones are closed. */
    suspend fun setAccounts(accounts: List<Account>) = lock.withLock {
        val wanted = accounts.associateBy { it.id }
        sources.keys.toList().forEach { id ->
            val (old, src) = sources.getValue(id)
            val now = wanted[id]
            // A source that renewed its own token reports an updated account; only a real edit restarts it.
            if (now == null || (now != old && now != src.account)) {
                src.close()
                sources.remove(id)
                lastGood.remove(id)
            }
        }
        accounts.forEach { a -> if (a.id !in sources) sources[a.id] = a to factory(a, ctx) }
        // Keep the user's order.
        val ordered = accounts.mapNotNull { a -> sources[a.id]?.let { a.id to it } }
        sources.clear(); ordered.forEach { (k, v) -> sources[k] = v }
    }

    suspend fun refresh(timeoutMs: Long = 25_000): HubSnapshot = coroutineScope {
        val current = lock.withLock { sources.values.map { it.second } }
        val results = current.map { src ->
            async {
                val id = src.account.id
                try {
                    val list = withTimeout(timeoutMs) { src.refresh() }
                    lastGood[id] = list
                    id to Result.success(list)
                } catch (e: CancellationException) {
                    if (e is kotlinx.coroutines.TimeoutCancellationException) id to Result.failure(java.io.IOException("Timed out"))
                    else throw e
                } catch (e: Exception) {
                    id to Result.failure(e)
                }
            }
        }.awaitAll()
        val printers = mutableListOf<PrinterStatus>()
        val problems = mutableMapOf<String, AccountProblem>()
        for ((id, r) in results) {
            r.onSuccess { printers += it }
            r.onFailure { e ->
                printers += lastGood[id].orEmpty()
                problems[id] = AccountProblem(id, e.message ?: e.javaClass.simpleName, e is AuthException)
            }
        }
        HubSnapshot(printers, problems)
    }

    /** Drops open connections (e.g. when the app goes to the background); the next refresh reconnects. */
    suspend fun disconnect() = lock.withLock {
        sources.keys.toList().forEach { id ->
            val (_, src) = sources.getValue(id)
            src.close()
            sources[id] = src.account to factory(src.account, ctx)
        }
    }

    suspend fun close() = lock.withLock {
        sources.values.forEach { it.second.close() }
        sources.clear()
    }
}
