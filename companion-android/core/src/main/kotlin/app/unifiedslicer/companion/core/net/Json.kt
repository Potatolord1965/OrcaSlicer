package app.unifiedslicer.companion.core.net

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull

/** Lenient accessors for the loosely typed JSON printer clouds send. */
val LenientJson = Json { ignoreUnknownKeys = true; isLenient = true; encodeDefaults = true }

fun parseJson(text: String): JsonElement? = runCatching { LenientJson.parseToJsonElement(text) }.getOrNull()

fun JsonElement?.obj(key: String): JsonObject? = (this as? JsonObject)?.get(key) as? JsonObject
fun JsonElement?.arr(key: String): JsonArray? = (this as? JsonObject)?.get(key) as? JsonArray
fun JsonElement?.el(key: String): JsonElement? = (this as? JsonObject)?.get(key)?.takeUnless { it is JsonNull }

fun JsonElement?.str(key: String): String? {
    val p = el(key) as? JsonPrimitive ?: return null
    return p.content.takeIf { it.isNotEmpty() }
}

fun JsonElement?.num(key: String): Double? = el(key).asNumber()
fun JsonElement?.long(key: String): Long? = num(key)?.toLong()
fun JsonElement?.int(key: String): Int? = num(key)?.toInt()
fun JsonElement?.bool(key: String): Boolean? {
    val p = el(key) as? JsonPrimitive ?: return null
    return p.booleanOrNull ?: p.longOrNull?.let { it != 0L }
}

/** A number, or a string holding one. */
fun JsonElement?.asNumber(): Double? {
    val p = this as? JsonPrimitive ?: return null
    return p.doubleOrNull ?: p.content.trim().toDoubleOrNull()
}

/** Recursively merges [delta] into [base]; objects merge, everything else replaces. */
fun mergeJson(base: JsonObject, delta: JsonObject): JsonObject {
    val out = base.toMutableMap()
    for ((k, v) in delta) {
        val old = out[k]
        out[k] = if (old is JsonObject && v is JsonObject) mergeJson(old, v) else v
    }
    return JsonObject(out)
}
