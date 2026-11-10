package app.unifiedslicer.companion.core

import app.unifiedslicer.companion.core.model.BambuAccount
import app.unifiedslicer.companion.core.model.ElegooAccount
import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.net.defaultHttpClient
import app.unifiedslicer.companion.core.source.PrintJob
import app.unifiedslicer.companion.core.source.RtmConnector
import app.unifiedslicer.companion.core.source.RtmSession
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.source.creality.CrealityCloudSource
import app.unifiedslicer.companion.core.source.elegoo.ElegooPrinterChannel
import app.unifiedslicer.companion.core.source.elegoo.ElegooPrinterError
import app.unifiedslicer.companion.core.source.lan.MoonrakerSource
import app.unifiedslicer.companion.core.source.lan.OctoPrintSource
import app.unifiedslicer.companion.core.source.printBlockedReason
import app.unifiedslicer.companion.core.source.printableBy
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File
import java.time.ZoneOffset
import java.time.ZonedDateTime

class PrintTest {
    private val ctx = SourceContext(defaultHttpClient())
    private fun gcode() = File.createTempFile("job", ".gcode").apply { writeText("G28\nG1 X10\n"); deleteOnExit() }

    @Test fun whoPrintsWhat() {
        val elegoo = ElegooAccount("e", "x", accessToken = "t", userId = "1")
        val bambu = BambuAccount("b", "x", accessToken = "t", uid = "1")
        assertTrue(printableBy(elegoo, "Benchy.GCODE"))
        assertFalse(printableBy(elegoo, "model.stl"))
        assertFalse(printableBy(bambu, "plate.gcode.3mf"))
        assertNotNull(printBlockedReason(bambu))
        assertTrue(printableBy(LanPrinter("l", "MK4", LanKind.PRUSALINK, "h"), "a.bgcode"))
    }

    @Test fun moonrakerUploadsAndStarts() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("""{"result":{"print_started":true}}"""))
            val src = MoonrakerSource(LanPrinter("l", "Voron", LanKind.MOONRAKER, server.url("/").toString(), "key"), ctx)
            val seen = mutableListOf<Int>()
            src.print("d", PrintJob(gcode(), "cube.gcode")) { pct, _ -> pct?.let { seen += it } }
            val req = server.takeRequest()
            assertEquals("/server/files/upload", req.path)
            assertEquals("key", req.getHeader("X-Api-Key"))
            val body = req.body.readUtf8()
            assertTrue(body.contains("filename=\"cube.gcode\""))
            assertTrue(body.contains("name=\"print\""))
            assertTrue(body.contains("G1 X10"))
            assertEquals(100, seen.last())
        }
    }

    @Test fun octoprintReportsRefusal() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setResponseCode(409).setBody("busy"))
            val src = OctoPrintSource(LanPrinter("l", "Ender", LanKind.OCTOPRINT, server.url("/").toString(), "key"), ctx)
            try {
                src.print("d", PrintJob(gcode(), "cube.gcode")) { _, _ -> }
                fail("expected an error")
            } catch (e: java.io.IOException) {
                assertTrue(e.message!!.contains("409"))
            }
        }
    }

    @Test fun crealityUploadSigning() {
        // RFC 2104 test vector for HMAC-SHA1, base64 encoded.
        assertEquals("7/zfauXrL6LSdBbV8YTfnCWafHk=", CrealityCloudSource.ossSignature("Jefe", "what do ya want for nothing?"))
        assertTrue(Regex("model/slice/[0-9A-F]{32}\\.gcode\\.gz").matches(CrealityCloudSource.objectKey("cube", "abc")))
        assertEquals("Tue, 10 Nov 2026 04:05:06 GMT", CrealityCloudSource.httpDate(ZonedDateTime.of(2026, 11, 10, 4, 5, 6, 0, ZoneOffset.UTC)))
    }

    @Test fun elegooChannelMatchesRepliesAndErrors() = runBlocking {
        var deliver: (String, String) -> Unit = { _, _ -> }
        val sent = mutableListOf<Pair<String, String>>()
        var errorCode = 0
        val rtm = RtmConnector { _, _, _, onMessage ->
            deliver = onMessage
            object : RtmSession {
                override suspend fun publish(peer: String, message: String) {
                    sent += peer to message
                    val id = Regex("\"id\":(\\d+)").find(message)!!.groupValues[1]
                    onMessage("someone-else", """{"id":$id,"result":{"error_code":99}}""") // ignored: not the printer
                    onMessage(peer, """{"id":$id,"method":1057,"result":{"error_code":$errorCode}}""")
                }
                override fun close() {}
            }
        }
        val ch = ElegooPrinterChannel.open(rtm, "777", "rtm777", "tok", "SN1")
        assertNotNull(ch.request(ElegooPrinterChannel.M_DOWNLOAD_FILE))
        assertEquals("777SN1", sent.single().first)
        deliver("777SN1", """{"method":6006,"result":{"progress":40,"status":0}}""")
        assertEquals("40", ch.download!!["progress"].toString())
        errorCode = 1026
        try {
            ch.request(ElegooPrinterChannel.M_START_PRINT)
            fail("expected an error")
        } catch (e: ElegooPrinterError) {
            assertEquals(1026, e.code)
        }
    }
}
