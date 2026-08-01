package supersocksr.ppp.android

import android.content.Context
import android.os.Build
import android.os.Environment
import android.util.Log
import java.io.File
import java.io.RandomAccessFile
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object PppLog {
    private const val FILE_NAME = "openppp2-vpn.log"
    private const val OLD_FILE_NAME = "openppp2-vpn.log.old"
    private const val TAG = "OpenPPP2Log"
    // Roll the log once it grows past this size so a long-lived VPN session
    // cannot balloon the file into gigabytes. The previous generation is kept
    // as .old so diagnosis still has recent history.
    private const val MAX_FILE_SIZE = 2L * 1024 * 1024
    private const val EXPORT_TAIL_BYTES = 4L * 1024 * 1024
    private val formatter = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
    private val lock = Any()

    fun path(context: Context): String {
        return File(context.filesDir, FILE_NAME).absolutePath
    }

    fun read(context: Context): String {
        val file = File(context.filesDir, FILE_NAME)
        if (!file.exists()) return ""
        return file.readText()
    }

    fun clear(context: Context) {
        synchronized(lock) {
            File(context.filesDir, FILE_NAME).writeText("")
        }
    }

    fun write(context: Context, message: String) {
        val line = "${formatter.format(Date())} $message\n"
        Log.e(TAG, message)
        synchronized(lock) {
            val file = File(context.filesDir, FILE_NAME)
            // The `:vpn` process and the UI process both append here. The
            // synchronized block plus an atomic-ish rename keeps corruption
            // unlikely; losing a line during roll is acceptable for a log.
            if (file.length() > MAX_FILE_SIZE) {
                val old = File(context.filesDir, OLD_FILE_NAME)
                if (old.exists()) old.delete()
                file.renameTo(old)
            }
            file.appendText(line)
        }
    }

    fun write(context: Context, message: String, throwable: Throwable) {
        write(context, "$message\n${Log.getStackTraceString(throwable)}")
    }

    /**
     * Builds a self-contained diagnostic export: device info, the full (or
     * tail of) vpn.log, and the recent native logcat lines produced by the
     * ppp/openppp2/libopenppp2 tags. The file lands in the app-external
     * Download/OpenPPP2 folder, which needs no storage permission and can be
     * shared through [androidx.core.content.FileProvider] or pulled with adb.
     *
     * Returns the absolute path of the written file, or null on failure.
     */
    fun export(context: Context): String? {
        return try {
            val dir = File(
                context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS)
                    ?: context.filesDir,
                "OpenPPP2",
            )
            if (!dir.exists()) dir.mkdirs()

            val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            val file = File(dir, "openppp2-export-$ts.log")

            val sb = StringBuilder(256 * 1024)
            sb.appendLine("OPENPPP2 diagnostic export")
            sb.appendLine("generated: ${formatter.format(Date())}")
            sb.appendLine("device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})")
            sb.appendLine("android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            sb.appendLine("package: ${context.packageName}")
            sb.appendLine("vpn.log: ${path(context)}")
            sb.appendLine()

            sb.appendLine("===== vpn.log =====")
            val log = File(context.filesDir, FILE_NAME)
            if (log.exists()) {
                sb.append(tail(log, EXPORT_TAIL_BYTES))
            } else {
                sb.appendLine("(no vpn.log yet)")
            }

            val oldLog = File(context.filesDir, OLD_FILE_NAME)
            if (oldLog.exists()) {
                sb.appendLine()
                sb.appendLine("===== vpn.log (previous generation .old) =====")
                sb.append(tail(oldLog, EXPORT_TAIL_BYTES))
            }

            sb.appendLine()
            sb.appendLine("===== logcat (ppp/openppp2/libopenppp2) =====")
            sb.append(captureLogcat())

            file.writeText(sb.toString())
            Log.e(TAG, "exported diagnostic log to ${file.absolutePath} (${file.length()} bytes)")
            file.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "export failed", e)
            null
        }
    }

    /** Reads the last [maxBytes] of a file as UTF-8 text (byte-aligned). */
    private fun tail(file: File, maxBytes: Long): String {
        return try {
            val length = file.length()
            if (length <= 0) return "(empty)"
            val start = (length - maxBytes).coerceAtLeast(0L)
            RandomAccessFile(file, "r").use { raf ->
                raf.seek(start)
                // Skip a partial first line so the tail starts clean.
                if (start > 0) {
                    var b = raf.read()
                    while (b != -1 && b != '\n'.code) b = raf.read()
                }
                val remaining = length - raf.filePointer
                val bytes = ByteArray(remaining.coerceAtMost(Int.MAX_VALUE.toLong()).toInt())
                raf.readFully(bytes)
                String(bytes, Charsets.UTF_8)
            }
        } catch (e: Exception) {
            "(failed to read tail: ${e.message})"
        }
    }

    /**
     * Captures recent logcat lines from the native engine (which logs under
     * the `ppp`, `openppp2` and `libopenppp2` tags from the `:vpn` process)
     * plus our own Kotlin tag. Reading logcat needs no permission; the lines
     * are capped to keep the export small.
     */
    private fun captureLogcat(maxLines: Int = 4000): String {
        return try {
            val process = ProcessBuilder("logcat", "-d", "-v", "time")
                .redirectErrorStream(true)
                .start()
            val text = process.inputStream.bufferedReader().use { it.readText() }
            process.waitFor()
            val matched = text.lineSequence()
                .filter { line ->
                    line.contains(" ppp ") ||
                        line.contains("openppp2") ||
                        line.contains("libopenppp2") ||
                        line.contains("OpenPPP2Log")
                }
                .toList()
            val capped = matched.takeLast(maxLines)
            if (capped.isEmpty()) "(no matching logcat lines)" else capped.joinToString("\n")
        } catch (e: Exception) {
            "(logcat capture failed: ${e.message})"
        }
    }
}
