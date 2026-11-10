package app.unifiedslicer.companion.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import app.unifiedslicer.companion.core.model.Brand
import app.unifiedslicer.companion.data.ThemeMode

/** Unified Slicer's neutral accent (AccentColor.hpp Base); printer screens swap in the brand colour. */
val NeutralAccent = Color(0xFF767676)

fun Brand.color() = Color(accent)

@Immutable
data class StudioColors(val muted: Color, val hairline: Color, val panel: Color, val good: Color, val warn: Color, val bad: Color)

val LocalStudio = staticCompositionLocalOf {
    StudioColors(Color.Gray, Color.DarkGray, Color.Black, Color.Green, Color.Yellow, Color.Red)
}

// Monochrome Studio: near-black / near-white surfaces, grey hairlines, colour only for accents and state.
private val DarkScheme = darkColorScheme(
    primary = Color(0xFFE6E6E8), onPrimary = Color(0xFF111214),
    secondary = NeutralAccent, onSecondary = Color.White,
    background = Color(0xFF0F0F11), onBackground = Color(0xFFEDEDEF),
    surface = Color(0xFF0F0F11), onSurface = Color(0xFFEDEDEF),
    surfaceVariant = Color(0xFF1C1C20), onSurfaceVariant = Color(0xFF9A9AA0),
    surfaceContainer = Color(0xFF17171A), surfaceContainerHigh = Color(0xFF1F1F23),
    surfaceContainerHighest = Color(0xFF26262B), surfaceContainerLow = Color(0xFF141416),
    outline = Color(0xFF3A3A40), outlineVariant = Color(0xFF2A2A2F),
    secondaryContainer = Color(0xFF2A2A2F), onSecondaryContainer = Color(0xFFEDEDEF),
    error = Color(0xFFE5484D),
)

private val LightScheme = lightColorScheme(
    primary = Color(0xFF17171A), onPrimary = Color.White,
    secondary = NeutralAccent, onSecondary = Color.White,
    background = Color(0xFFF6F6F7), onBackground = Color(0xFF121214),
    surface = Color(0xFFF6F6F7), onSurface = Color(0xFF121214),
    surfaceVariant = Color(0xFFECECEE), onSurfaceVariant = Color(0xFF66666C),
    surfaceContainer = Color(0xFFFFFFFF), surfaceContainerHigh = Color(0xFFF0F0F2),
    surfaceContainerHighest = Color(0xFFE8E8EB), surfaceContainerLow = Color(0xFFFAFAFB),
    outline = Color(0xFFD0D0D4), outlineVariant = Color(0xFFE2E2E5),
    secondaryContainer = Color(0xFFE6E6E9), onSecondaryContainer = Color(0xFF121214),
    error = Color(0xFFD13438),
)

private fun studio(scheme: ColorScheme, dark: Boolean) = StudioColors(
    muted = scheme.onSurfaceVariant,
    hairline = scheme.outlineVariant,
    panel = scheme.surfaceContainer,
    good = if (dark) Color(0xFF3DD68C) else Color(0xFF1F9D57),
    warn = if (dark) Color(0xFFF5B83D) else Color(0xFFB7791F),
    bad = scheme.error,
)

private val StudioType = Typography().let { t ->
    t.copy(
        headlineSmall = t.headlineSmall.copy(fontWeight = FontWeight.SemiBold, letterSpacing = (-0.2).sp),
        titleLarge = t.titleLarge.copy(fontWeight = FontWeight.SemiBold, letterSpacing = (-0.2).sp),
        titleMedium = t.titleMedium.copy(fontWeight = FontWeight.SemiBold),
        labelSmall = t.labelSmall.copy(letterSpacing = 0.6.sp),
    )
}

val NumberStyle = TextStyle(fontWeight = FontWeight.SemiBold, fontFeatureSettings = "tnum")

@Composable
fun isDark(mode: ThemeMode) = when (mode) {
    ThemeMode.DARK -> true
    ThemeMode.LIGHT -> false
    ThemeMode.SYSTEM -> isSystemInDarkTheme()
}

@Composable
fun StudioTheme(dark: Boolean, content: @Composable () -> Unit) {
    val scheme = if (dark) DarkScheme else LightScheme
    androidx.compose.runtime.CompositionLocalProvider(LocalStudio provides studio(scheme, dark)) {
        MaterialTheme(colorScheme = scheme, typography = StudioType, content = content)
    }
}
