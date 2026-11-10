package app.unifiedslicer.companion.core.source.elegoo

import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.core.model.PrinterState
import app.unifiedslicer.companion.core.model.PrinterStatus
import app.unifiedslicer.companion.core.model.Temperature
import app.unifiedslicer.companion.core.net.int
import app.unifiedslicer.companion.core.net.long
import app.unifiedslicer.companion.core.net.num
import app.unifiedslicer.companion.core.net.obj
import app.unifiedslicer.companion.core.net.str
import kotlinx.serialization.json.JsonObject

/**
 * The Centauri Carbon 2 status object (method 6000/1002 on the printer, report-data in the
 * cloud): machine_status, print_status, extruder, heater_bed, ztemperature_sensor, ...
 */
object ElegooStatus {
    private val SUB_STATUS = mapOf(
        1081 to "Downloading file", 1082 to "Downloading file", 1086 to "Downloading file",
        1041 to "Heating nozzle", 1045 to "Heating nozzle", 1096 to "Heating nozzle",
        1405 to "Heating bed", 1906 to "Heating bed", 2801 to "Homing", 2802 to "Homing",
        2901 to "Auto bed leveling", 2902 to "Auto bed leveling", 2501 to "Pausing", 2502 to "Paused",
        2505 to "Paused", 2401 to "Resuming", 2402 to "Resuming", 2075 to "Printing",
        2077 to "Print finished", 2503 to "Stopping", 2504 to "Stopped",
    )
    private val PREPARING = setOf(1041, 1045, 1096, 1405, 1906, 2801, 2802, 2901, 2902, 1081, 1082, 1086)

    fun state(status: Int?, sub: Int?): PrinterState = when (status) {
        2 -> when (sub) {
            2501, 2502, 2505 -> PrinterState.PAUSED
            2077 -> PrinterState.FINISHED
            2503, 2504 -> PrinterState.FAILED
            in PREPARING -> PrinterState.PREPARING
            else -> PrinterState.PRINTING
        }
        1 -> if (sub == 2077) PrinterState.FINISHED else PrinterState.IDLE
        0 -> PrinterState.PREPARING
        14 -> PrinterState.FAILED
        null -> PrinterState.UNKNOWN
        else -> PrinterState.IDLE
    }

    fun toStatus(
        accountId: String,
        deviceId: String,
        name: String,
        model: String,
        via: String,
        s: JsonObject,
        updatedAt: Long,
    ): PrinterStatus {
        val ms = s.obj("machine_status")
        val ps = s.obj("print_status")
        val sub = ms.int("sub_status")
        val state = state(ms.int("status"), sub)
        val printing = state.active || state == PrinterState.FINISHED
        fun temp(group: String) = s.obj(group)?.let { g ->
            g.num("temperature")?.let { Temperature(it, g.num("target")?.takeIf { t -> t > 0 }) }
        }
        val alert = ms.int("exception_status")?.takeIf { it != 0 }
        return PrinterStatus(
            accountId = accountId,
            deviceId = deviceId,
            name = name,
            model = model,
            brand = Brand.ELEGOO,
            via = via,
            state = state,
            stage = sub?.let { SUB_STATUS[it] }?.takeIf { state.active },
            progress = ms.int("progress")?.takeIf { printing },
            remainingSeconds = ps.long("remaining_time_sec")?.takeIf { state.active && it > 0 },
            layer = ps.int("current_layer")?.takeIf { printing },
            totalLayers = ps.int("total_layer")?.takeIf { printing && it > 0 },
            file = ps.str("filename")?.takeIf { printing },
            nozzle = temp("extruder"),
            bed = temp("heater_bed"),
            chamber = temp("ztemperature_sensor"),
            alert = alert?.let { "Alert $it" },
            updatedAt = updatedAt,
        )
    }
}
