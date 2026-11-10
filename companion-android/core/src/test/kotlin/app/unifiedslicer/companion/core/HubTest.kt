package app.unifiedslicer.companion.core

import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.net.AuthException
import app.unifiedslicer.companion.core.net.defaultHttpClient
import app.unifiedslicer.companion.core.source.PrinterSource
import app.unifiedslicer.companion.core.source.SourceContext
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class HubTest {
    private class Fake(override val account: Account, var fail: Exception? = null) : PrinterSource {
        var closed = false
        override suspend fun refresh(): List<PrinterStatus> {
            fail?.let { throw it }
            return listOf(PrinterStatus(account.id, "d", account.label, "", Brand.OTHER, "test", PrinterState.IDLE))
        }
        override fun close() { closed = true }
    }

    @Test fun keepsLastGoodAndReportsProblems() = runTest {
        val fakes = mutableMapOf<String, Fake>()
        val hub = PrinterHub(SourceContext(defaultHttpClient())) { a, _ -> Fake(a).also { fakes[a.id] = it } }
        val a = LanPrinter("a", "A", LanKind.OCTOPRINT, "h")
        val b = LanPrinter("b", "B", LanKind.OCTOPRINT, "h")
        hub.setAccounts(listOf(a, b))
        assertEquals(2, hub.refresh().printers.size)

        fakes.getValue("b").fail = AuthException("expired")
        val snap = hub.refresh()
        assertEquals(2, snap.printers.size)
        assertTrue(snap.problems.getValue("b").signInExpired)

        hub.setAccounts(listOf(a))
        assertTrue(fakes.getValue("b").closed)
        assertEquals(listOf("a"), hub.refresh().printers.map { it.accountId })
    }
}
