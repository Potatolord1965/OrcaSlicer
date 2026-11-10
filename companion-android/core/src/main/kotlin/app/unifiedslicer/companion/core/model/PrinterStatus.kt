package app.unifiedslicer.companion.core.model

import kotlinx.serialization.Serializable

@Serializable
enum class PrinterState(val label: String, val active: Boolean) {
    OFFLINE("Offline", false),
    IDLE("Idle", false),
    PREPARING("Preparing", true),
    PRINTING("Printing", true),
    PAUSED("Paused", true),
    FINISHED("Finished", false),
    FAILED("Failed", false),
    UNKNOWN("Unknown", false),
}

data class Temperature(val current: Double, val target: Double? = null)

/** One printer's last known state, as shown in the app. */
data class PrinterStatus(
    val accountId: String,
    val deviceId: String,
    val name: String,
    val model: String = "",
    val brand: Brand,
    /** Where the data comes from, e.g. "Bambu Cloud" or "LAN". */
    val via: String,
    val state: PrinterState,
    /** Finer stage reported by the printer, e.g. "Heating bed". */
    val stage: String? = null,
    val progress: Int? = null,
    val remainingSeconds: Long? = null,
    val layer: Int? = null,
    val totalLayers: Int? = null,
    val file: String? = null,
    val nozzle: Temperature? = null,
    val bed: Temperature? = null,
    val chamber: Temperature? = null,
    /** Printer-side alert, e.g. an error code. */
    val alert: String? = null,
    val updatedAt: Long = System.currentTimeMillis(),
) {
    val key: String get() = "$accountId/$deviceId"
}
