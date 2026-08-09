package io.nekohasekai.sagernet.bg

import android.content.Context
import io.nekohasekai.sagernet.ktx.Logs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.File
import java.io.IOException
import java.io.InputStreamReader
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Background silent logcat collector.
 *
 * Runs only while the VPN service is alive. It streams the app's own logcat
 * tags (including the native `ppp` tag) into filesDir/logs/ppp-<ts>.log so
 * disconnects and quality problems can be diagnosed afterwards without the
 * user keeping the log screen open. Every line is flushed immediately, so a
 * crash never loses already-collected history. Files are read back by
 * filename (not by in-memory state), so the main process can attach logs
 * written by the `:bg` VPN process and vice versa.
 */
object LogCollector {

    private const val MAX_FILE_BYTES = 5L * 1024 * 1024
    private const val MAX_FILES = 3

    private val lock = Any()
    private var job: Job? = null
    private var process: Process? = null
    private var writer: BufferedWriter? = null
    private var currentFile: File? = null
    private var bytesWritten = 0L
    private var logDir: File? = null

    @Volatile
    private var collecting = false

    private val logTags = arrayOf(
        "AndroidRuntime:D",
        "ProxyInstance:D",
        "GuardedProcessPool:D",
        "VpnService:D",
        "Go:D",
        "exclave-core:D",
        "libexclavecore:D",
        "libnaive:D",
        "libshadowquic:D",
        "Exclave:D",
        "ppp:D",
        "*:S"
    )

    fun start(context: Context) {
        synchronized(lock) {
            if (job != null) return
            logDir = File(context.filesDir, "logs").apply { mkdirs() }
            collecting = true
            job = GlobalScope.launch(Dispatchers.IO) {
                try {
                    runCollector()
                } catch (e: Throwable) {
                    Logs.w(e)
                } finally {
                    synchronized(lock) {
                        collecting = false
                        closeWriter()
                        try {
                            process?.destroy()
                        } catch (_: Throwable) {
                        }
                        process = null
                        job = null
                    }
                }
            }
        }
    }

    fun stop() {
        synchronized(lock) {
            collecting = false
            job?.cancel()
            job = null
            closeWriter()
            try {
                process?.destroy()
            } catch (_: Throwable) {
            }
            process = null
        }
    }

    /** Newest collected log file, or null when nothing was written yet. */
    fun latestLogFile(context: Context): File? {
        return logDirOf(context).listFiles { f ->
            f.isFile && f.name.startsWith("ppp-") && f.name.endsWith(".log")
        }?.maxByOrNull { it.lastModified() }
    }

    /** Tail of the newest collected log (up to [maxBytes]) for crash reports and export. */
    fun snapshot(context: Context, maxBytes: Int = 512 * 1024): String {
        val file = latestLogFile(context) ?: return ""
        return try {
            val length = file.length()
            if (length <= 0L) return ""
            val start = (length - maxBytes).coerceAtLeast(0L)
            file.inputStream().use { input ->
                input.skip(start)
                val buf = ByteArray((length - start).toInt().coerceAtMost(1 shl 20))
                val read = input.read(buf)
                if (read <= 0) "" else String(buf, 0, read, Charsets.UTF_8)
            }
        } catch (e: Throwable) {
            ""
        }
    }

    private suspend fun runCollector() {
        val process = ProcessBuilder(
            listOf(
                "logcat",
                "-v", "threadtime",
                "-s", logTags.joinToString(",")
            )
        ).start()
        synchronized(lock) { this.process = process }
        val stdout = BufferedReader(InputStreamReader(process.inputStream))
        try {
            while (collecting) {
                val line = stdout.readLine() ?: break
                appendLine(line)
            }
        } finally {
            synchronized(lock) {
                closeWriter()
                try {
                    process.destroy()
                } catch (_: Throwable) {
                }
                this.process = null
            }
        }
    }

    private fun appendLine(line: String) {
        synchronized(lock) {
            var w = writer
            if (w == null) {
                currentFile = newLogFile()
                w = currentFile?.bufferedWriter(Charsets.UTF_8)
                writer = w
                bytesWritten = 0L
            }
            if (w == null) return
            try {
                w.write(line)
                w.newLine()
                w.flush()
                bytesWritten += line.toByteArray(Charsets.UTF_8).size + 1L
                if (bytesWritten > MAX_FILE_BYTES) {
                    closeWriter()
                }
            } catch (e: IOException) {
                closeWriter()
            }
        }
    }

    private fun newLogFile(): File? {
        val dir = logDir ?: return null
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val file = File(dir, "ppp-$stamp.log")
        val files = dir.listFiles { f ->
            f.isFile && f.name.startsWith("ppp-") && f.name.endsWith(".log")
        }?.sortedByDescending { it.lastModified() } ?: emptyList()
        for (old in files.drop(MAX_FILES - 1)) {
            try {
                old.delete()
            } catch (_: Throwable) {
            }
        }
        return file
    }

    private fun logDirOf(context: Context): File = File(context.filesDir, "logs")

    private fun closeWriter() {
        synchronized(lock) {
            try {
                writer?.close()
            } catch (_: Throwable) {
            }
            writer = null
            currentFile = null
        }
    }
}