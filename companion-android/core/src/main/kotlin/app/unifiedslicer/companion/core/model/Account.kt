package app.unifiedslicer.companion.core.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/** A signed-in cloud account or a directly reachable printer. Persisted (encrypted) by the app. */
@Serializable
sealed interface Account {
    val id: String
    val label: String
    val brand: Brand
    val kindLabel: String
}

@Serializable
@SerialName("bambu")
data class BambuAccount(
    override val id: String,
    override val label: String,
    val region: String = "global",
    val accessToken: String,
    val refreshToken: String = "",
    val uid: String,
) : Account {
    override val brand get() = Brand.BAMBU
    override val kindLabel get() = "Bambu Cloud"
}

@Serializable
@SerialName("elegoo")
data class ElegooAccount(
    override val id: String,
    override val label: String,
    val region: String = "global",
    val accessToken: String,
    val refreshToken: String = "",
    val accessExpireMs: Long = 0,
    val userId: String,
) : Account {
    override val brand get() = Brand.ELEGOO
    override val kindLabel get() = "Elegoo Cloud"
}

@Serializable
@SerialName("creality")
data class CrealityAccount(
    override val id: String,
    override val label: String,
    val region: String = "global",
    val token: String,
    val userId: String,
    val deviceUid: String,
) : Account {
    override val brand get() = Brand.CREALITY
    override val kindLabel get() = "Creality Cloud"
}

@Serializable
@SerialName("markforged")
data class MarkforgedAccount(
    override val id: String,
    override val label: String,
    val accessKey: String,
    val secretKey: String,
) : Account {
    override val brand get() = Brand.MARKFORGED
    override val kindLabel get() = "Markforged Eiger"
}

@Serializable
enum class LanKind(val label: String, val brand: Brand, val secretLabel: String?, val needsSerial: Boolean = false) {
    BAMBU("Bambu Lab (LAN mode)", Brand.BAMBU, "Access code", needsSerial = true),
    ELEGOO_CC2("Elegoo Centauri Carbon 2", Brand.ELEGOO, "Access code"),
    MOONRAKER("Klipper / Moonraker", Brand.KLIPPER, "API key (optional)"),
    OCTOPRINT("OctoPrint", Brand.OCTOPRINT, "API key"),
    PRUSALINK("PrusaLink", Brand.PRUSA, "API key"),
}

@Serializable
@SerialName("lan")
data class LanPrinter(
    override val id: String,
    override val label: String,
    val kind: LanKind,
    /** IP address, host name or base URL. */
    val host: String,
    val secret: String = "",
    val serial: String = "",
) : Account {
    override val brand get() = kind.brand
    override val kindLabel get() = "LAN · ${kind.label}"
}
