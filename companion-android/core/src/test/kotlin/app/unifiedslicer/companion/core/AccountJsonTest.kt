package app.unifiedslicer.companion.core

import app.unifiedslicer.companion.core.model.Account
import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.net.LenientJson
import kotlinx.serialization.builtins.ListSerializer
import org.junit.Assert.assertEquals
import org.junit.Test

class AccountJsonTest {
    @Test fun roundTrip() {
        val list: List<Account> = listOf(
            BambuAccount("b", "me@x", accessToken = "t", uid = "1"),
            LanPrinter("l", "Voron", LanKind.MOONRAKER, "10.0.0.2:7125"),
        )
        val ser = ListSerializer(Account.serializer())
        val text = LenientJson.encodeToString(ser, list)
        assertEquals(list, LenientJson.decodeFromString(ser, text))
    }
}
