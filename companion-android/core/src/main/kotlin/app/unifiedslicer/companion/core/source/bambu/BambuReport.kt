package app.unifiedslicer.companion.core.source.bambu

import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.Temperature
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.long
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.str
import kotlinx.serialization.json.JsonObject

/** Bambu printers' MQTT "print" report (push_status), shared by cloud and LAN mode. */
object BambuReport {
    fun state(gcodeState: String?): PrinterState = when (gcodeState?.uppercase()) {
        "IDLE" -> PrinterState.IDLE
        "PREPARE", "SLICING", "INIT" -> PrinterState.PREPARING
        "RUNNING" -> PrinterState.PRINTING
        "PAUSE" -> PrinterState.PAUSED
        "FINISH" -> PrinterState.FINISHED
        "FAILED" -> PrinterState.FAILED
        else -> PrinterState.UNKNOWN
    }

    // stg_cur: the printer's current calibration / preparation step.
    private val STAGES = mapOf(
        1 to "Auto bed leveling", 2 to "Heating bed", 3 to "Vibration compensation",
        4 to "Changing filament", 7 to "Heating nozzle", 8 to "Calibrating extrusion",
        9 to "Scanning bed surface", 10 to "Inspecting first layer", 13 to "Homing",
        14 to "Cleaning nozzle", 16 to "Paused by user", 17 to "Front cover fell off",
        19 to "Calibrating extrusion flow", 20 to "Nozzle temperature malfunction",
        21 to "Bed temperature malfunction", 22 to "Unloading filament", 24 to "Loading filament",
        25 to "Calibrating motor noise", 29 to "Cooling chamber", 35 to "Paused: nozzle clog",
    )

    fun toStatus(
        accountId: String,
        deviceId: String,
        name: String,
        model: String,
        via: String,
        print: JsonObject,
        updatedAt: Long,
    ): PrinterStatus {
        val state = state(print.str("gcode_state"))
        val printing = state.active || state == PrinterState.FINISHED
        val err = print.long("print_error") ?: 0L
        fun temp(cur: String, target: String?) =
            print.num(cur)?.let { Temperature(it, target?.let { t -> print.num(t) }) }
        return PrinterStatus(
            accountId = accountId,
            deviceId = deviceId,
            name = name,
            model = model,
            brand = Brand.BAMBU,
            via = via,
            state = state,
            stage = print.int("stg_cur")?.let { STAGES[it] }?.takeIf { state.active },
            progress = print.int("mc_percent")?.takeIf { printing },
            remainingSeconds = print.long("mc_remaining_time")?.times(60)?.takeIf { state.active && it > 0 },
            layer = print.int("layer_num")?.takeIf { printing },
            totalLayers = print.int("total_layer_num")?.takeIf { printing && it > 0 },
            file = (print.str("subtask_name") ?: print.str("gcode_file"))?.takeIf { printing },
            nozzle = temp("nozzle_temper", "nozzle_target_temper"),
            bed = temp("bed_temper", "bed_target_temper"),
            chamber = print.num("chamber_temper")?.takeIf { it > 0 }?.let { Temperature(it) },
            alert = if (err != 0L) "Error %08X".format(err) else null,
            updatedAt = updatedAt,
        )
    }
}
