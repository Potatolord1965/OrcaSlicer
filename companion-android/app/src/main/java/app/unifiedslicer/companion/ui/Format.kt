package app.unifiedslicer.companion.ui

import app.unifiedslicer.companion.core.model.Temperature
import kotlin.math.roundToInt

fun formatDuration(seconds: Long): String {
    val m = (seconds + 59) / 60
    return when {
        m < 60 -> "${m}m"
        m < 60 * 24 -> "${m / 60}h ${m % 60}m"
        else -> "${m / (60 * 24)}d ${(m / 60) % 24}h"
    }
}

fun formatAgo(at: Long, now: Long = System.currentTimeMillis()): String {
    val s = (now - at) / 1000
    return when {
        s < 10 -> "just now"
        s < 60 -> "${s}s ago"
        s < 3600 -> "${s / 60} min ago"
        s < 86400 -> "${s / 3600} h ago"
        else -> "${s / 86400} d ago"
    }
}

fun formatTemp(t: Temperature?): String {
    if (t == null) return "–"
    val cur = "${t.current.roundToInt()}°"
    return t.target?.let { "$cur / ${it.roundToInt()}°" } ?: cur
}

fun etaClock(remainingSeconds: Long, now: Long = System.currentTimeMillis()): String {
    val f = java.text.DateFormat.getTimeInstance(java.text.DateFormat.SHORT)
    return f.format(java.util.Date(now + remainingSeconds * 1000))
}
