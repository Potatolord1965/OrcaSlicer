package app.unifiedslicer.companion.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import app.unifiedslicer.companion.core.source.PrintJob
import java.io.File

/** Files waiting to be sent live in the app's cache; only the newest few are kept. */
object PrintFiles {
    private fun dir(context: Context) = File(context.cacheDir, "print-files").apply { mkdirs() }

    /** A name printers and URLs cope with: no path, no control or reserved characters. */
    fun safeName(raw: String?): String {
        val name = raw.orEmpty().substringAfterLast('/').substringAfterLast('\\')
            .replace(Regex("[\\u0000-\\u001f\"*:<>?|#%&]"), "_").trim().trim('.')
        return name.ifEmpty { "print.gcode" }.takeLast(120)
    }

    fun newFile(context: Context, name: String): File {
        val d = dir(context)
        d.listFiles().orEmpty().sortedByDescending { it.lastModified() }.drop(4).forEach { it.deleteRecursively() }
        // One folder per file keeps the original name on disk.
        return File(File(d, System.currentTimeMillis().toString()).apply { mkdirs() }, safeName(name))
    }

    /** Copies a document the user picked into the cache. */
    fun import(context: Context, uri: Uri): PrintJob {
        val name = context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        } ?: uri.lastPathSegment
        val file = newFile(context, safeName(name))
        (context.contentResolver.openInputStream(uri) ?: throw java.io.IOException("Cannot open the file")).use { input ->
            file.outputStream().use { input.copyTo(it) }
        }
        return PrintJob(file, file.name)
    }

    /** Sliced files a printer can run as they are. */
    fun isPrinterReady(name: String) = listOf(".gcode", ".gco", ".g", ".bgcode", ".3mf").any { name.endsWith(it, ignoreCase = true) }
}
