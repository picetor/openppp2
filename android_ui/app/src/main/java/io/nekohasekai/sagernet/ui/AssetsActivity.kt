/******************************************************************************
 *                                                                            *
 * Copyright (C) 2021 by nekohasekai <contact-sagernet@sekai.icu>             *
 *                                                                            *
 * This program is free software: you can redistribute it and/or modify       *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 *  (at your option) any later version.                                       *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.       *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.ui

import android.content.Intent
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.format.DateFormat
import android.view.Menu
import android.view.MenuItem
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isInvisible
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.ItemTouchHelper
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.snackbar.Snackbar
import io.nekohasekai.sagernet.R
import io.nekohasekai.sagernet.SagerNet
import io.nekohasekai.sagernet.database.DataStore
import io.nekohasekai.sagernet.database.SagerDatabase
import io.nekohasekai.sagernet.databinding.LayoutAssetItemBinding
import io.nekohasekai.sagernet.databinding.LayoutAssetsBinding
import io.nekohasekai.sagernet.ktx.*
import io.nekohasekai.sagernet.widget.UndoSnackbarManager
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.*
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withContext

class AssetsActivity : ThemedActivity() {

    lateinit var adapter: AssetAdapter
    lateinit var layout: LayoutAssetsBinding
    lateinit var undoManager: UndoSnackbarManager<File>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val binding = LayoutAssetsBinding.inflate(layoutInflater)
        layout = binding
        setContentView(binding.root)

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.recycler_view)) { v, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars()
                        or WindowInsetsCompat.Type.displayCutout()
            )
            v.updatePadding(
                left = bars.left + dp2px(4),
                right = bars.right + dp2px(4),
                bottom = bars.bottom + dp2px(4),
            )
            insets
        }
        setSupportActionBar(findViewById(R.id.toolbar))
        supportActionBar?.apply {
            setTitle(R.string.route_assets)
            setDisplayHomeAsUpEnabled(true)
            setHomeAsUpIndicator(R.drawable.ic_navigation_close)
        }

        binding.recyclerView.layoutManager = FixedLinearLayoutManager(binding.recyclerView)
        adapter = AssetAdapter()
        binding.recyclerView.adapter = adapter

        binding.refreshLayout.setOnRefreshListener {
            adapter.reloadAssets()
            binding.refreshLayout.isRefreshing = false
        }
        binding.refreshLayout.setColorSchemeColors(getColorAttr(R.attr.primaryOrTextPrimary))

        undoManager = UndoSnackbarManager(this, adapter)

        ItemTouchHelper(object : ItemTouchHelper.SimpleCallback(
            0, ItemTouchHelper.START
        ) {

            override fun getSwipeDirs(
                recyclerView: RecyclerView, viewHolder: RecyclerView.ViewHolder
            ): Int {
                val index = viewHolder.adapterPosition
                if (index < internalFiles.size + ipRuleFiles.size) return 0
                return super.getSwipeDirs(recyclerView, viewHolder)
            }

            override fun onSwiped(viewHolder: RecyclerView.ViewHolder, direction: Int) {
                val index = viewHolder.adapterPosition
                adapter.remove(index)
                undoManager.remove(index to (viewHolder as AssetHolder).file)
            }

            override fun onMove(
                recyclerView: RecyclerView,
                viewHolder: RecyclerView.ViewHolder,
                target: RecyclerView.ViewHolder
            ) = false

        }).attachToRecyclerView(binding.recyclerView)
    }

    override fun snackbarInternal(text: CharSequence): Snackbar {
        return Snackbar.make(layout.coordinator, text, Snackbar.LENGTH_LONG)
    }

    val internalFiles = arrayOf("geoip.dat", "geosite.dat")

    // Bundled IP routing rule files (also shipped inside the APK under
    // assets/rules).  They can be replaced by importing a custom file;
    // the update button restores the bundled copy.
    val ipRuleFiles = arrayOf("ip.txt", "ipv6.txt", "dns-rules.txt")

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.import_asset_menu, menu)
        return true
    }

    val importFile = registerForActivityResult(ActivityResultContracts.GetContent()) { file ->
        if (file != null) {
            val fileName = contentResolver.query(file, null, null, null, null)?.use { cursor ->
                cursor.moveToFirst()
                cursor.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME).let(cursor::getString)
            }?.takeIf { it.isNotBlank() } ?: file.pathSegments.last()
                .substringAfterLast('/')
                .substringAfter(':')

            val supportedRuleFile = fileName.endsWith(".dat") || fileName.endsWith(".txt") ||
                fileName.endsWith(".yaml") || fileName.endsWith(".yml") || fileName.endsWith(".json") ||
                fileName == "index.html" || fileName == "index.js" || fileName == "root_store.certs"
            if (!supportedRuleFile) {
                runOnMainDispatcher {
                    alert(getString(R.string.route_not_asset, fileName)).show()
                }
                return@registerForActivityResult
            }

            runOnDefaultDispatcher {
                val outFile = File(app.externalAssets, fileName).apply {
                    parentFile?.mkdirs()
                }

                contentResolver.openInputStream(file)?.use(outFile.outputStream())

                File(outFile.parentFile, outFile.nameWithoutExtension + ".version.txt").apply {
                    if (isFile) delete()
                }

                adapter.reloadAssets()
            }

        }
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        when (item.itemId) {
            R.id.action_import_file -> {
                startFilesForResult(importFile, "*/*")
                return true
            }
            R.id.action_import_url -> {
                startActivity(Intent(this, AssetEditActivity::class.java))
                adapter.reloadAssets()
                return true
            }
        }
        return false
    }

    inner class AssetAdapter : RecyclerView.Adapter<AssetHolder>(),
        UndoSnackbarManager.Interface<File> {

        val assets = ArrayList<File>()

        init {
            reloadAssets()
        }

        fun reloadAssets() {
            assets.clear()
            assets.add(File(app.externalAssets, "geoip.dat"))
            assets.add(File(app.externalAssets, "geosite.dat"))
            ipRuleFiles.forEach { assets.add(File(app.externalAssets, it)) }

            val managedAssets = SagerDatabase.assetDao.getAll().associateBy { it.name }
            managedAssets.forEach {
                assets.add(File(app.externalAssets, it.key))
            }

            val unmanagedAssets = app.externalAssets.listFiles()?.filter {
                it.isFile && it.name.endsWith(".dat") && it.name !in internalFiles && it !in assets
            }
            if (unmanagedAssets != null) assets.addAll(unmanagedAssets)

            layout.refreshLayout.post {
                notifyDataSetChanged()
            }
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): AssetHolder {
            return AssetHolder(LayoutAssetItemBinding.inflate(layoutInflater, parent, false))
        }

        override fun onBindViewHolder(holder: AssetHolder, position: Int) {
            holder.bind(assets[position])
        }

        override fun getItemCount(): Int {
            return assets.size
        }

        fun remove(index: Int) {
            assets.removeAt(index)
            notifyItemRemoved(index)
        }

        override fun undo(actions: List<Pair<Int, File>>) {
            for ((index, item) in actions) {
                assets.add(index, item)
                notifyItemInserted(index)
            }
        }

        override fun commit(actions: List<Pair<Int, File>>) {
            val groups = actions.map { it.second }.toTypedArray()
            runOnDefaultDispatcher {
                groups.forEach {
                    it.deleteRecursively()
                    SagerDatabase.assetDao.delete(it.name)
                }
            }
        }

    }

    val updating = AtomicInteger()

    inner class AssetHolder(val binding: LayoutAssetItemBinding) : RecyclerView.ViewHolder(binding.root) {
        lateinit var file: File

        fun bind(file: File) {
            this.file = file

            binding.assetName.text = file.name
            val versionFile = File(file.parentFile, "${file.nameWithoutExtension}.version.txt")
            val isGeo = file.name in internalFiles
            val isIpRule = file.name in ipRuleFiles
            val runtimeName = when (file.name) {
                "geoip.dat" -> "GeoIP.dat"
                "geosite.dat" -> "GeoSite.dat"
                else -> file.name
            }
            val runtimeFile = File(filesDir, "rules/$runtimeName")

            val localVersion = if (isIpRule) {
                getString(if (runtimeFile.isFile && runtimeFile.length() > 0L) R.string.route_asset_loaded else R.string.route_asset_not_loaded)
            } else if (file.isFile) {
                if (versionFile.isFile) {
                    versionFile.readText().trim()
                } else {
                    DateFormat.getDateFormat(app).format(Date(file.lastModified()))
                }
            } else if (runtimeFile.isFile && runtimeFile.length() > 0L) {
                getString(R.string.route_asset_loaded)
            } else {
                getString(R.string.route_asset_not_loaded)
            }

            binding.assetStatus.text = if (isIpRule) {
                localVersion
            } else {
                getString(R.string.route_asset_status, localVersion)
            }

            val assetEntity = SagerDatabase.assetDao.get(file.name)
            binding.rulesUpdate.isInvisible = isIpRule || (!isGeo && assetEntity == null)
            binding.rulesUpdate.text = getString(
                R.string.group_update
            )
            binding.rulesUpdate.setOnClickListener {
                updating.incrementAndGet()
                layout.refreshLayout.isEnabled = false
                binding.subscriptionUpdateProgress.isInvisible = false
                binding.rulesUpdate.isInvisible = true
                runOnDefaultDispatcher {
                    runCatching {
                        when {
                            isGeo -> updateAsset(file, versionFile, localVersion, assetEntity?.url)
                            else -> updateCustomAsset(file, assetEntity!!.url)
                        }
                    }.onFailure {
                        onMainDispatcher {
                            snackbar(it.readableMessage).show()
                        }
                    }

                    onMainDispatcher {
                        binding.rulesUpdate.isInvisible = false
                        binding.subscriptionUpdateProgress.isInvisible = true
                        if (updating.decrementAndGet() == 0) {
                            layout.refreshLayout.isEnabled = true
                        }
                    }
                }
            }

            binding.edit.isVisible = !isIpRule && (isGeo || assetEntity != null)
            binding.edit.setOnClickListener {
                startActivity(Intent(this@AssetsActivity, AssetEditActivity::class.java).apply {
                    putExtra(AssetEditActivity.EXTRA_ASSET_NAME, file.name)
                })
                adapter.reloadAssets()
            }

        }

    }

    suspend fun updateAsset(file: File, versionFile: File, localVersion: String, customUrl: String? = null) {
        if (!customUrl.isNullOrBlank()) {
            updateCustomAsset(file, customUrl)
            return
        }
        val repo: String
        var fileName = file.name
        when (DataStore.rulesProvider) {
            3 -> return updateGeoAsset(file, versionFile)
            0 -> {
                if (file.name == internalFiles[0]) {
                    repo = "v2fly/geoip"
                } else {
                    repo = "v2fly/domain-list-community"
                    fileName = "dlc.dat"
                }
            }
            1 -> repo = "Loyalsoldier/v2ray-rules-dat"
            2 -> repo = "Chocolate4U/Iran-v2ray-rules"
            4 -> repo = "runetfreedom/russia-v2ray-rules-dat"
            else -> error("invalid asset provider")
        }

        try {
            val response = fetchJson("https://api.github.com/repos/$repo/releases/latest")

            val release = parseJson(response).asJsonObject
            val tagName = release.getString("tag_name") ?: error("tag_name not found in release ${release["url"]}")

            if (tagName == localVersion) {
                onMainDispatcher {
                    snackbar(R.string.route_asset_no_update).show()
                }
                return
            }

            val releaseAssets = release.getArray("assets")
            val assetToDownload = releaseAssets?.find { it.getString("name") == fileName }
                ?: error("File $fileName not found in release ${release["url"]}")
            val browserDownloadUrl = assetToDownload.getString("browser_download_url")
                ?: error("browser_download_url not found for $fileName")

            downloadTo(browserDownloadUrl, file, versionFile, tagName)
        } catch (e: Exception) {
            Logs.w(e)
            throw e
        }
    }

    suspend fun updateGeoAsset(file: File, versionFile: File) {
        try {
            val url = if (file.name == internalFiles[0]) DataStore.rulesGeoipUrl else DataStore.rulesGeositeUrl
            try {
                updateCustomAsset(file, url)
            } catch (e: Exception) {
                val fallback = jsdelivrFallback(url)
                if (fallback == null) throw e
                Logs.w("geo update primary source failed, trying jsdelivr CDN: ${e.readableMessage}")
                updateCustomAsset(file, fallback)
            }
        } finally {
            if (versionFile.isFile) {
                versionFile.delete()
            }
        }
    }

    /** Convert a GitHub releases/latest/download URL to its jsdelivr CDN mirror. */
    private fun jsdelivrFallback(url: String): String? {
        val m = Regex("https://github\\.com/([\\w.-]+/[\\w.-]+)/releases/latest/download/(.+)").find(url)
            ?: return null
        return "https://cdn.jsdelivr.net/gh/${m.groupValues[1]}@release/${m.groupValues[2]}"
    }

    /** Restore a bundled IP rule file (ip.txt / ipv6.txt / dns-rules.txt)
     *  from the APK assets back into the writable assets directory. */
    suspend fun restoreBundledAsset(file: File) {
        withContext(Dispatchers.IO) {
            val name = file.name
            val outFile = File(app.externalAssets, name).apply { parentFile?.mkdirs() }
            try {
                assets.open("rules/$name").use { input ->
                    outFile.outputStream().use { output -> input.copyTo(output) }
                }
                File(outFile.parentFile, "${outFile.nameWithoutExtension}.version.txt").apply {
                    if (isFile) delete()
                }
            } catch (e: Exception) {
                Logs.w(e)
                throw e
            }
        }
        adapter.reloadAssets()
        onMainDispatcher {
            snackbar(R.string.route_asset_updated).show()
        }
    }

    suspend fun updateCustomAsset(file: File, url: String) {
        try {
            downloadTo(url, file, null, null)
        } catch (e: Exception) {
            Logs.w(e)
            throw e
        }
    }

    /** Fetch a small UTF-8 document (JSON / YAML) as text. */
    private suspend fun fetchJson(url: String): String = withContext(Dispatchers.IO) {
        val connection = openRuleConnection(url).apply {
            connectTimeout = 15000
            readTimeout = 30000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "openppp2-android")
            setRequestProperty("Accept", "application/json,text/plain,*/*")
        }
        try {
            val code = connection.responseCode
            if (code !in 200..299) {
                error("HTTP $code")
            }
            connection.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                .removePrefix("\uFEFF")
        } finally {
            connection.disconnect()
        }
    }

    /** Download a binary file, write it into place and record its version tag. */
    private suspend fun downloadTo(url: String, file: File, versionFile: File?, tagName: String?) {
        withTimeout(120000) {
            withContext(Dispatchers.IO) {
                val connection = openRuleConnection(url).apply {
                    // Rule sources may only be reachable through the active VPN.
                    connectTimeout = 20000
                    readTimeout = 60000
                    instanceFollowRedirects = true
                    setRequestProperty("User-Agent", "openppp2-android")
                    setRequestProperty("Accept", "*/*")
                }
                try {
                    val code = connection.responseCode
                    if (code !in 200..299) {
                        error("HTTP $code")
                    }
                    val cacheFile = File(file.parentFile, file.name + ".tmp")
                    cacheFile.parentFile?.mkdirs()
                    cacheFile.delete()
                    connection.inputStream.use { input ->
                        cacheFile.outputStream().use { output ->
                            input.copyTo(output)
                        }
                    }
                    if (!cacheFile.renameTo(file)) {
                        cacheFile.copyTo(file, overwrite = true)
                        cacheFile.delete()
                    }
                    versionFile?.writeText(tagName ?: "")
                } finally {
                    connection.disconnect()
                }
            }
        }
        adapter.reloadAssets()
        onMainDispatcher {
            snackbar(R.string.route_asset_updated).show()
        }
    }

    private fun openRuleConnection(url: String): HttpURLConnection {
        return URL(url).openConnection() as HttpURLConnection
    }

    override fun onSupportNavigateUp(): Boolean {
        finish()
        return true
    }

    override fun onResume() {
        super.onResume()

        if (::adapter.isInitialized) {
            adapter.reloadAssets()
        }
    }


}
