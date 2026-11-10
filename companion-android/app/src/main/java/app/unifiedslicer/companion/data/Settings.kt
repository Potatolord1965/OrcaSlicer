package app.unifiedslicer.companion.data

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

enum class ThemeMode { DARK, LIGHT, SYSTEM }

/** Small user preferences. Dark is the default, as in the desktop app. */
class Settings(context: Context) {
    private val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    private val _theme = MutableStateFlow(runCatching { ThemeMode.valueOf(prefs.getString("theme", null) ?: "DARK") }.getOrDefault(ThemeMode.DARK))
    val theme: StateFlow<ThemeMode> = _theme.asStateFlow()

    private val _notifications = MutableStateFlow(prefs.getBoolean("notifications", true))
    /** Notify when a print finishes, fails or pauses. */
    val notifications: StateFlow<Boolean> = _notifications.asStateFlow()

    private val _backgroundChecks = MutableStateFlow(prefs.getBoolean("background", true))
    /** Check printers every 15 minutes while the app is closed (needed for notifications then). */
    val backgroundChecks: StateFlow<Boolean> = _backgroundChecks.asStateFlow()

    private val _refreshSeconds = MutableStateFlow(prefs.getInt("refresh", 10))
    val refreshSeconds: StateFlow<Int> = _refreshSeconds.asStateFlow()

    fun setTheme(v: ThemeMode) { _theme.value = v; prefs.edit().putString("theme", v.name).apply() }
    fun setNotifications(v: Boolean) { _notifications.value = v; prefs.edit().putBoolean("notifications", v).apply() }
    fun setBackgroundChecks(v: Boolean) { _backgroundChecks.value = v; prefs.edit().putBoolean("background", v).apply() }
    fun setRefreshSeconds(v: Int) { _refreshSeconds.value = v; prefs.edit().putInt("refresh", v).apply() }
}
