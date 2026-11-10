package app.unifiedslicer.companion.core.model

/** Printer brand. [accent] matches the desktop app's per-brand accent (AccentColor.hpp). */
enum class Brand(val label: String, val accent: Long) {
    BAMBU("Bambu Lab", 0xFF2FA84F),
    ELEGOO("Elegoo", 0xFF3A6FD8),
    CREALITY("Creality", 0xFFD8762C),
    MARKFORGED("Markforged", 0xFF8B8F95),
    PRUSA("Prusa", 0xFFFA6831),
    KLIPPER("Klipper", 0xFFB12F36),
    OCTOPRINT("OctoPrint", 0xFF13C100),
    OTHER("Printer", 0xFF767676),
}
