package app.unifiedslicer.companion.core

import app.unifiedslicer.companion.core.model.LanKind
import app.unifiedslicer.companion.core.model.LanPrinter
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.net.defaultHttpClient
import app.unifiedslicer.companion.core.net.mergeJson
import app.unifiedslicer.companion.core.net.parseJson
import app.unifiedslicer.companion.core.source.SourceContext
import app.unifiedslicer.companion.core.source.bambu.BambuReport
import app.unifiedslicer.companion.core.source.creality.CrealityCloudSource
import app.unifiedslicer.companion.core.source.elegoo.ElegooCloudSource
import app.unifiedslicer.companion.core.source.elegoo.ElegooStatus
import app.unifiedslicer.companion.core.source.lan.MoonrakerSource
import app.unifiedslicer.companion.core.source.lan.OctoPrintSource
import app.unifiedslicer.companion.core.source.lan.PrusaLinkSource
import app.unifiedslicer.companion.core.source.markforged.EigerSource
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ParsersTest {
    private fun obj(s: String) = parseJson(s)!!.jsonObject
    private val ctx = SourceContext(defaultHttpClient())

    @Test fun bambuReport() {
        val p = obj("""{"gcode_state":"RUNNING","mc_percent":42,"mc_remaining_time":75,"layer_num":10,"total_layer_num":200,
            "subtask_name":"Benchy","nozzle_temper":219.5,"nozzle_target_temper":220,"bed_temper":"60","bed_target_temper":60,"print_error":0}""")
        val s = BambuReport.toStatus("a", "SN", "X1C", "X1 Carbon", "Bambu Cloud", p, 1L)
        assertEquals(PrinterState.PRINTING, s.state)
        assertEquals(42, s.progress)
        assertEquals(4500L, s.remainingSeconds)
        assertEquals("Benchy", s.file)
        assertEquals(60.0, s.bed!!.current, 0.0)
        assertNull(s.alert)
    }

    @Test fun bambuDeltaMerge() {
        val full = obj("""{"gcode_state":"RUNNING","mc_percent":10,"nozzle_temper":200}""")
        val merged = mergeJson(full, obj("""{"mc_percent":11}"""))
        val s = BambuReport.toStatus("a", "SN", "P1S", "", "LAN", merged, 1L)
        assertEquals(11, s.progress)
        assertEquals(200.0, s.nozzle!!.current, 0.0)
    }

    @Test fun elegooCloudReport() {
        val data = parseJson("""{"machine_status":[{"reportLinkKey":"status","reportValue":"2","updateTime":1700000000000},
            {"reportLinkKey":"sub_status","reportValue":"1405","updateTime":1700000000500},{"reportLinkKey":"progress","reportValue":"3"}],
            "print_status":[{"reportLinkKey":"filename","reportValue":"\"cube.gcode\""},{"reportLinkKey":"remaining_time_sec","reportValue":"600"}],
            "extruder":[{"reportLinkKey":"temperature","reportValue":"150.2"},{"reportLinkKey":"target","reportValue":"220"}]}""")
        val (report, updated) = ElegooCloudSource.parseReport(data)
        assertEquals(1700000000500L, updated)
        val s = ElegooStatus.toStatus("a", "SN", "Potato Printer", "CC2", "Elegoo Cloud", report, updated)
        assertEquals(PrinterState.PREPARING, s.state)
        assertEquals("Heating bed", s.stage)
        assertEquals("cube.gcode", s.file)
        assertEquals(600L, s.remainingSeconds)
        assertEquals(220.0, s.nozzle!!.target!!, 0.0)
    }

    @Test fun elegooStates() {
        assertEquals(PrinterState.IDLE, ElegooStatus.state(1, 0))
        assertEquals(PrinterState.FINISHED, ElegooStatus.state(1, 2077))
        assertEquals(PrinterState.PAUSED, ElegooStatus.state(2, 2502))
        assertEquals(PrinterState.PRINTING, ElegooStatus.state(2, 2075))
    }

    @Test fun elegooLoginReport() {
        val a = ElegooCloudSource.accountFromLogin(parseJson("""{"accessToken":"t","refreshToken":"r","userId":123,"nickname":"Nate"}"""), "id")!!
        assertEquals("123", a.userId)
        assertEquals("Nate", a.label)
        assertNull(ElegooCloudSource.accountFromLogin(parseJson("""{"nickname":"x"}"""), "id"))
        assertTrue(ElegooCloudSource.isElegooPage("https://www.nexprint.com/account/slicer-login"))
        assertTrue(!ElegooCloudSource.isElegooPage("https://nexprint.com.evil.io/"))
        assertTrue(!ElegooCloudSource.isElegooPage("http://www.nexprint.com/"))
    }

    @Test fun crealityTelemetry() {
        val t = CrealityCloudSource.parseTelemetry(parseJson("""{"nozzleTemp":[{"ts":1,"value":"210"}],"state":[{"ts":1,"value":1}],"x":{"a":1}}""")!!)
        assertEquals(setOf("nozzleTemp", "state"), t.keys)
        assertEquals(PrinterState.PAUSED, CrealityCloudSource.state(5))
    }

    @Test fun eigerStates() {
        assertEquals(PrinterState.PREPARING, EigerSource.state("Printing", "heating"))
        assertEquals(PrinterState.FAILED, EigerSource.state("Out of Material", null))
        assertEquals(PrinterState.IDLE, EigerSource.state("Ready", null))
    }

    @Test fun moonraker() {
        val src = MoonrakerSource(LanPrinter("l", "Voron", LanKind.MOONRAKER, "10.0.0.2"), ctx)
        val s = src.parse(parseJson("""{"print_stats":{"state":"printing","filename":"a.gcode","print_duration":600,"info":{"current_layer":5,"total_layer":50}},
            "virtual_sdcard":{"progress":0.25},"extruder":{"temperature":240.1,"target":240},"heater_bed":{"temperature":100,"target":0}}"""))
        assertEquals(PrinterState.PRINTING, s.state)
        assertEquals(25, s.progress)
        assertEquals(1800L, s.remainingSeconds)
        assertNull(s.bed!!.target)
    }

    @Test fun octoprint() {
        val src = OctoPrintSource(LanPrinter("l", "Ender", LanKind.OCTOPRINT, "octopi.local", "key"), ctx)
        val s = src.parse(
            parseJson("""{"state":{"flags":{"operational":true,"printing":true}},"temperature":{"tool0":{"actual":200,"target":205},"bed":{"actual":60,"target":60}}}""")!!,
            parseJson("""{"job":{"file":{"name":"x.gcode"}},"progress":{"completion":50.5,"printTimeLeft":100}}""")!!,
        )
        assertEquals(PrinterState.PRINTING, s.state)
        assertEquals(50, s.progress)
        assertEquals("x.gcode", s.file)
    }

    @Test fun prusaLink() {
        val src = PrusaLinkSource(LanPrinter("l", "MK4", LanKind.PRUSALINK, "10.0.0.3", "key"), ctx)
        val s = src.parse(parseJson("""{"printer":{"state":"PRINTING","temp_nozzle":215,"target_nozzle":215,"temp_bed":60,"target_bed":60},"job":{"id":1,"progress":12,"time_remaining":3600}}""")!!,
            parseJson("""{"display_name":"part.bgcode"}"""))
        assertEquals(PrinterState.PRINTING, s.state)
        assertEquals("part.bgcode", s.file)
        assertEquals(3600L, s.remainingSeconds)
    }

    @Test fun emptyReportIsUnknown() {
        val s = ElegooStatus.toStatus("a", "SN", "n", "m", "v", JsonObject(emptyMap()), 0)
        assertEquals(PrinterState.UNKNOWN, s.state)
    }
}
