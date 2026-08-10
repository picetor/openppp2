package io.nekohasekai.sagernet.ui

import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.view.LayoutInflater
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.Toolbar
import androidx.core.content.FileProvider
import androidx.core.view.isVisible
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import io.nekohasekai.sagernet.BuildConfig
import io.nekohasekai.sagernet.R
import io.nekohasekai.sagernet.databinding.ItemLogFileBinding
import io.nekohasekai.sagernet.databinding.LayoutLogFilesBinding
import io.nekohasekai.sagernet.ktx.*
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Management UI for the background collected log files (filesDir/logs/).
 * Lists every ppp-*.log written by LogCollector, with preview, share and
 * delete actions; no logcat console output here.
 */
class LogFilesFragment : ToolbarFragment(R.layout.layout_log_files),
    Toolbar.OnMenuItemClickListener {

    private lateinit var binding: LayoutLogFilesBinding
    private val logFiles = mutableListOf<File>()
    private val timeFormat = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        binding = LayoutLogFilesBinding.bind(view)
        toolbar.setTitle(R.string.log_files)
        toolbar.inflateMenu(R.menu.log_files_menu)
        toolbar.setOnMenuItemClickListener(this)

        binding.logFilesRecycler.layoutManager = LinearLayoutManager(requireContext())
        binding.logFilesRecycler.adapter = LogFileAdapter()
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        val dir = File(requireContext().filesDir, "logs")
        logFiles.clear()
        dir.listFiles { f ->
            f.isFile && f.name.startsWith("ppp-") && f.name.endsWith(".log")
        }?.sortedByDescending { it.lastModified() }?.let { logFiles.addAll(it) }
        binding.logFilesRecycler.adapter?.notifyDataSetChanged()
        binding.logFilesEmpty.isVisible = logFiles.isEmpty()
    }

    private fun formatSize(bytes: Long): String {
        return when {
            bytes >= 1024L * 1024L -> String.format(Locale.US, "%.1f MB", bytes / 1024.0 / 1024.0)
            bytes >= 1024L -> String.format(Locale.US, "%.1f KB", bytes / 1024.0)
            else -> "$bytes B"
        }
    }

    private fun shareFile(file: File) {
        val uri = FileProvider.getUriForFile(
            requireContext(), BuildConfig.APPLICATION_ID + ".cache", file
        )
        val intent = Intent(Intent.ACTION_SEND).setType("text/plain")
            .setFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            .putExtra(Intent.EXTRA_STREAM, uri)
            .putExtra(Intent.EXTRA_SUBJECT, file.name)
        startActivity(Intent.createChooser(intent, file.name))
    }

    private fun shareAll() {
        val uris = logFiles.map {
            FileProvider.getUriForFile(requireContext(), BuildConfig.APPLICATION_ID + ".cache", it)
        }
        if (uris.isEmpty()) return
        val intent = Intent(Intent.ACTION_SEND_MULTIPLE).setType("text/plain")
            .setFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            .putParcelableArrayListExtra(Intent.EXTRA_STREAM, ArrayList(uris))
        startActivity(Intent.createChooser(intent, getString(R.string.share_all_log_files)))
    }

    private fun deleteFile(file: File) {
        AlertDialog.Builder(requireContext())
            .setMessage(R.string.delete_log_file_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                file.delete()
                refresh()
            }
            .show()
    }

    private fun preview(file: File) {
        val previewBytes = 128 * 1024L
        onDefaultDispatcher {
            val length = file.length()
            val skip = (length - previewBytes).coerceAtLeast(0L)
            val text = try {
                file.inputStream().use { input ->
                    input.skip(skip)
                    val buf = ByteArray((length - skip).coerceAtMost(previewBytes).toInt())
                    val read = input.read(buf)
                    if (read <= 0) "" else String(buf, 0, read, Charsets.UTF_8)
                }
            } catch (e: Throwable) {
                ""
            }
            val hint = if (skip > 0) {
                getString(R.string.log_files_tail_hint, formatSize(length))
            } else null
            onMainDispatcher {
                if (!isAdded) return@onMainDispatcher
                val content = buildString {
                    if (hint != null) append(hint).append("\n\n")
                    append(text)
                }
                val textView = TextView(requireContext()).apply {
                    this.text = content
                    textSize = 10f
                    typeface = Typeface.MONOSPACE
                    setTextIsSelectable(true)
                    setPadding(dp2px(16), dp2px(8), dp2px(16), 0)
                }
                val scroll = ScrollView(requireContext()).apply {
                    addView(textView)
                }
                AlertDialog.Builder(requireContext())
                    .setTitle(file.name)
                    .setView(scroll)
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
        }
    }

    private inner class LogFileAdapter : RecyclerView.Adapter<LogFileAdapter.Holder>() {

        inner class Holder(val row: ItemLogFileBinding) : RecyclerView.ViewHolder(row.root)

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder =
            Holder(ItemLogFileBinding.inflate(LayoutInflater.from(parent.context), parent, false))

        override fun getItemCount(): Int = logFiles.size

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val file = logFiles[position]
            holder.row.logFileName.text = file.name
            holder.row.logFileMeta.text =
                "${formatSize(file.length())} · ${timeFormat.format(Date(file.lastModified()))}"
            holder.row.root.setOnClickListener { preview(file) }
            holder.row.logFileShare.setOnClickListener { shareFile(file) }
            holder.row.logFileDelete.setOnClickListener { deleteFile(file) }
        }
    }

    override fun onMenuItemClick(item: MenuItem): Boolean {
        when (item.itemId) {
            R.id.action_share_all_log_files -> {
                shareAll()
                true
            }
            R.id.action_clear_all_log_files -> {
                if (logFiles.isEmpty()) true
                else {
                    AlertDialog.Builder(requireContext())
                        .setMessage(R.string.clear_all_log_files_confirm)
                        .setNegativeButton(android.R.string.cancel, null)
                        .setPositiveButton(android.R.string.ok) { _, _ ->
                            for (file in logFiles) file.delete()
                            refresh()
                        }
                        .show()
                    true
                }
            }
            else -> false
        }
    }
}