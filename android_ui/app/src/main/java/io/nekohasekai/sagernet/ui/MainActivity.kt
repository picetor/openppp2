/******************************************************************************
 *                                                                            *
 * Copyright (C) 2021 by nekohasekai <contact-sagernet@sekai.icu>             *
 * Copyright (C) 2021 by Max Lv <max.c.lv@gmail.com>                          *
 * Copyright (C) 2021 by Mygod Studio <contact-shadowsocks-android@mygod.be>  *
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
import android.content.pm.PackageManager
import android.Manifest
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.RemoteException
import android.provider.Settings
import android.text.util.Linkify
import android.view.KeyEvent
import android.view.MenuItem
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.annotation.IdRes
import androidx.appcompat.app.AlertDialog
import androidx.core.app.ActivityCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.setPadding
import androidx.core.view.updatePadding
import androidx.preference.PreferenceDataStore
import com.google.android.material.bottomappbar.BottomAppBar.FAB_ALIGNMENT_MODE_CENTER
import com.google.android.material.bottomappbar.BottomAppBar.FAB_ALIGNMENT_MODE_END
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.navigation.NavigationView
import com.google.android.material.snackbar.Snackbar
import io.nekohasekai.sagernet.*
import io.nekohasekai.sagernet.aidl.AppStats
import io.nekohasekai.sagernet.aidl.ISagerNetService
import io.nekohasekai.sagernet.aidl.TrafficStats
import io.nekohasekai.sagernet.bg.BaseService
import io.nekohasekai.sagernet.bg.SagerConnection
import io.nekohasekai.sagernet.database.*
import io.nekohasekai.sagernet.database.preference.OnPreferenceDataStoreChangeListener
import io.nekohasekai.sagernet.databinding.LayoutMainBinding
import io.nekohasekai.sagernet.fmt.AbstractBean
import io.nekohasekai.sagernet.fmt.Alerts
import io.nekohasekai.sagernet.fmt.PluginEntry
import io.nekohasekai.sagernet.group.GroupInterfaceAdapter
import io.nekohasekai.sagernet.ktx.*
import io.nekohasekai.sagernet.utils.PackageCache
import io.noties.markwon.Markwon
import java.io.BufferedReader
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.BufferedWriter
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.ConnectException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URL
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

class MainActivity : ThemedActivity(),
    SagerConnection.Callback,
    OnPreferenceDataStoreChangeListener,
    NavigationView.OnNavigationItemSelectedListener {

    lateinit var binding: LayoutMainBinding
    lateinit var navigation: NavigationView

    val userInterface by lazy { GroupInterfaceAdapter(this) }

    override val onBackPressedCallback = object : OnBackPressedCallback(enabled = false) {
        override fun handleOnBackPressed() {
            displayFragmentWithId(R.id.nav_configuration)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = LayoutMainBinding.inflate(layoutInflater)
        when (DataStore.fabStyle) {
            FabStyle.SagerNet -> {
                binding.stats.fabAlignmentMode = FAB_ALIGNMENT_MODE_END
                binding.stats.fabCradleMargin = 0F
                binding.stats.fabCradleRoundedCornerRadius = 0F
                binding.stats.cradleVerticalOffset = dp2px(8).toFloat()
            }
            FabStyle.Shadowsocks -> {
                binding.stats.fabAlignmentMode = FAB_ALIGNMENT_MODE_CENTER
                binding.stats.fabCradleMargin = dp2px(5).toFloat()
                binding.stats.fabCradleRoundedCornerRadius = dp2px(6).toFloat()
                binding.stats.cradleVerticalOffset = 0F
            }
        }

        binding.fab.initProgress(binding.fabProgress)
        if (themeResId !in intArrayOf(
                R.style.Theme_SagerNet_Black, R.style.Theme_SagerNet_LightBlack
            )
        ) {
            navigation = binding.navView
            binding.drawerLayout.removeView(binding.navViewBlack)
        } else {
            navigation = binding.navViewBlack
            binding.drawerLayout.removeView(binding.navView)
        }
        navigation.setNavigationItemSelectedListener(this)
        navigation.getHeaderView(0)?.findViewById<TextView>(R.id.nav_header_version)?.text =
            "v${BuildConfig.VERSION_NAME}"
        if (resources.configuration.layoutDirection == View.LAYOUT_DIRECTION_RTL) {
            ViewCompat.setOnApplyWindowInsetsListener(navigation) { v, insets ->
                val bars = insets.getInsets(
                    WindowInsetsCompat.Type.systemBars()
                            or WindowInsetsCompat.Type.displayCutout()
                )
                v.updatePadding(
                    top = bars.top,
                    right = bars.right,
                    bottom = bars.bottom,
                )
                insets
            }
        } else {
            ViewCompat.setOnApplyWindowInsetsListener(navigation) { v, insets ->
                val bars = insets.getInsets(
                    WindowInsetsCompat.Type.systemBars()
                            or WindowInsetsCompat.Type.displayCutout()
                )
                v.updatePadding(
                    top = bars.top,
                    left = bars.left,
                    bottom = bars.bottom,
                )
                insets
            }
        }

        if (savedInstanceState == null) {
            displayFragmentWithId(R.id.nav_configuration)
        }

        binding.fab.setOnClickListener {
            if (state.canStop) SagerNet.stopService() else connect.launch(
                null
            )
        }
        binding.stats.setOnClickListener { if (state == BaseService.State.Connected) binding.stats.testConnection() }

        setContentView(binding.root)

        changeState(BaseService.State.Idle)
        connection.connect(this, this)
        DataStore.configurationStore.registerChangeListener(this)

        if (intent?.action == Intent.ACTION_VIEW) {
            onNewIntent(intent)
        }

        runOnMainDispatcher {
            fun getLicenseKeyName(only: Boolean): String {
                return if (only) "gplv3OnlyAccepted" else "gplv3OrLaterAccepted"
            }
            val only = false
            if (DataStore.configurationStore.getBoolean(getLicenseKeyName(only)) != true) {
                DataStore.configurationStore.putBoolean(getLicenseKeyName(only), true)
                DataStore.configurationStore.remove(getLicenseKeyName(!only))
                AlertDialog.Builder(this@MainActivity).apply {
                    setTitle(R.string.license)
                    setView(
                        TextView(this@MainActivity).apply {
                            setPadding(dp2px(16))
                            text = getString(if (only) {
                                R.string.license_gpl_v3_only
                            } else {
                                R.string.license_gpl_v3_or_later
                            })
                            setTextIsSelectable(true)
                            Linkify.addLinks(this, Linkify.EMAIL_ADDRESSES or Linkify.WEB_URLS)
                        }
                    )
                    setPositiveButton(android.R.string.ok) { _, _ ->
                        requestPermissions()
                    }
                    setOnCancelListener { _ ->
                        requestPermissions()
                    }
                }.show()
            } else {
                requestPermissions()
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)

        val uri = intent.data ?: return

        runOnDefaultDispatcher {
            if (uri.scheme == "exclave" && uri.host == "subscription") {
                uri.getQueryParameter("url")?.let {
                    importSubscription(it)
                }
            } else {
                importProfile(uri)
            }
        }
    }

    private fun requestPermissions() {
        if (!DataStore.getInstalledPackagesInited) {
            // For bullshit Chinese OEMs
            DataStore.getInstalledPackagesInited = true
            runOnDefaultDispatcher {
                PackageCache.register()
            }
        }
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (!DataStore.postNotificationsPermissionRequested) {
                DataStore.postNotificationsPermissionRequested = true
                if (app.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                    permissions.add(Manifest.permission.POST_NOTIFICATIONS)
                }
            }
        }
        if (permissions.isNotEmpty()) {
            ActivityCompat.requestPermissions(this@MainActivity, permissions.toTypedArray(), 0)
        }
    }

    fun urlTest(): Int {
        if (state != BaseService.State.Connected || connection.service == null) {
            error("not started")
        }
        // Android's VpnService exempts the creating app's uid from VPN
        // routing (to avoid loops), so a plain socket from this UI process
        // goes out the physical NIC directly and cannot reach the tunnel.
        // The openppp2 core opens a local proxy on loopback (injected as
        // client.http-proxy.port / client.socks-proxy.port in
        // VpnService.startVpn); connecting to 127.0.0.1 is not VPN-routed,
        // and the proxy forwards the request into the tunnel from the
        // VpnService process. This is the only reliable way to measure real
        // latency through the tunnel from the UI process. Run in-process so
        // exceptions propagate to the UI callers (Binder cannot propagate
        // exceptions across processes).
        //
        // The HTTP inbound is optional (requireHttp, default off) while the
        // SOCKS5 inbound is on by default (requireSocks), so pick whichever
        // inbound is actually listening instead of assuming HTTP CONNECT.
        if (!DataStore.requireHttp && !DataStore.requireSocks) {
            error("no inbound proxy enabled")
        }
        val useHttpProxy = DataStore.requireHttp
        val url = URL(DataStore.connectionTestURL)
        val useTls = url.protocol.equals("https", ignoreCase = true)
        val host = url.host
        val port = if (url.port > 0) url.port else if (useTls) 443 else 80
        val path = (url.path.ifEmpty { "/" }) + (url.query?.let { "?$it" } ?: "")
        val proxyPort = if (useHttpProxy) DataStore.httpPort else DataStore.socksPort
        val start = System.currentTimeMillis()
        var raw: Socket? = null
        try {
            // The local proxy is opened by the VpnService core when the
            // tunnel comes up.  Right after the UI reports Connected the
            // listener may not be bound yet (a race of a few hundred ms), so
            // retry a handful of times on refusal instead of failing the
            // very first tap.  A failed connect() poisons the Android socket
            // instance (the next connect() on it throws SocketException:
            // Socket closed), so allocate a fresh socket for every attempt.
            var lastRefusal: ConnectException? = null
            for (attempt in 0 until 5) {
                val candidate = Socket()
                try {
                    candidate.connect(InetSocketAddress(InetAddress.getByName("127.0.0.1"), proxyPort), 20000)
                    raw = candidate
                    lastRefusal = null
                    break
                } catch (e: ConnectException) {
                    candidate.close()
                    lastRefusal = e
                    try {
                        Thread.sleep(500L)
                    } catch (_: InterruptedException) {
                        break
                    }
                } catch (e: Throwable) {
                    candidate.close()
                    throw e
                }
            }
            if (lastRefusal != null) throw lastRefusal
            raw!!.soTimeout = 20000
            if (useHttpProxy) {
                httpProxyConnect(raw!!, host, port)
            } else {
                socks5Connect(raw!!, host, port)
            }
            val socket: Socket = if (useTls) {
                val ssl = (SSLContext.getDefault().socketFactory as SSLSocketFactory)
                    .createSocket(raw, host, port, true) as SSLSocket
                ssl.startHandshake()
                ssl
            } else raw!!
            try {
                val out = BufferedWriter(OutputStreamWriter(socket.getOutputStream(), Charsets.US_ASCII))
                out.write("GET $path HTTP/1.1\r\n")
                out.write("Host: $host\r\n")
                out.write("User-Agent: openppp2-android\r\n")
                out.write("Connection: close\r\n\r\n")
                out.flush()
                val reader = BufferedReader(InputStreamReader(socket.getInputStream(), Charsets.US_ASCII))
                val statusLine = reader.readLine() ?: error("empty response")
                val code = statusLine.split(" ").getOrNull(1)?.toIntOrNull()
                    ?: error("malformed response: $statusLine")
                if (code !in 200..399) error("HTTP $code")
                return (System.currentTimeMillis() - start).toInt()
            } finally {
                if (socket !== raw) socket.close()
            }
        } finally {
            raw?.close()
        }
    }

    /** HTTP CONNECT handshake against the core's local HTTP proxy. */
    private fun httpProxyConnect(raw: Socket, host: String, port: Int) {
        val proxyOut = BufferedWriter(OutputStreamWriter(raw.getOutputStream(), Charsets.US_ASCII))
        val proxyIn = BufferedReader(InputStreamReader(raw.getInputStream(), Charsets.US_ASCII))
        proxyOut.write("CONNECT $host:$port HTTP/1.1\r\n")
        proxyOut.write("Host: $host:$port\r\n")
        proxyOut.write("User-Agent: openppp2-android\r\n")
        proxyOut.write("\r\n")
        proxyOut.flush()
        val proxyStatus = proxyIn.readLine() ?: error("proxy empty response")
        val proxyCode = proxyStatus.split(" ").getOrNull(1)?.toIntOrNull()
            ?: error("malformed proxy response: $proxyStatus")
        if (proxyCode !in 200..299) error("proxy HTTP $proxyCode")
        while (true) {
            val line = proxyIn.readLine() ?: error("proxy header EOF")
            if (line.isEmpty()) break
        }
    }

    /** SOCKS5 CONNECT handshake against the core's local SOCKS5 proxy. */
    private fun socks5Connect(raw: Socket, host: String, port: Int) {
        val out = DataOutputStream(BufferedOutputStream(raw.getOutputStream()))
        val input = BufferedInputStream(raw.getInputStream())
        val username = DataStore.socksUsername
        val password = DataStore.socksPassword
        val auth = !username.isNullOrEmpty() || !password.isNullOrEmpty()
        // Method negotiation: NO AUTH, plus USER/PASS when credentials exist.
        out.write(if (auth) byteArrayOf(5, 2, 0, 2) else byteArrayOf(5, 1, 0))
        out.flush()
        if (input.read() != 5) error("SOCKS5: bad version")
        when (input.read()) {
            0 -> { /* no auth */ }
            2 -> {
                val u = username.orEmpty().toByteArray(Charsets.ISO_8859_1)
                val p = password.orEmpty().toByteArray(Charsets.ISO_8859_1)
                val buf = ByteArrayOutputStream()
                buf.write(1)
                buf.write(u.size)
                buf.write(u)
                buf.write(p.size)
                buf.write(p)
                out.write(buf.toByteArray())
                out.flush()
                if (input.read() != 1 || input.read() != 0) error("SOCKS5: auth failed")
            }
            else -> error("SOCKS5: no acceptable auth method")
        }
        // CONNECT request: SOCKS5 01 00 03 <domain> <port BE>.
        val hostBytes = host.toByteArray(Charsets.ISO_8859_1)
        val req = ByteArrayOutputStream()
        req.write(5)
        req.write(1)
        req.write(0)
        req.write(3)
        req.write(hostBytes.size)
        req.write(hostBytes)
        req.write(port ushr 8)
        req.write(port and 0xFF)
        out.write(req.toByteArray())
        out.flush()
        if (input.read() != 5) error("SOCKS5: bad reply version")
        val rep = input.read()
        input.read() // reserved
        when (input.read()) { // address type
            1 -> repeat(4) { input.read() }
            3 -> repeat(input.read()) { input.read() }
            4 -> repeat(16) { input.read() }
            else -> error("SOCKS5: bad address type")
        }
        input.read()
        input.read() // bound port
        if (rep != 0) error("SOCKS5: connect failed (code $rep)")
    }

    suspend fun importSubscription(uri: String) {
        val group = ProxyGroup(type = GroupType.SUBSCRIPTION).apply {
            subscription = SubscriptionBean().apply {
                link = uri
                name = getString(R.string.subscription)
            }
        }

        onMainDispatcher {

            displayFragmentWithId(R.id.nav_group)

            MaterialAlertDialogBuilder(this@MainActivity).setTitle(R.string.subscription_import)
                .setMessage(getString(R.string.subscription_import_message, uri))
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    runOnDefaultDispatcher {
                        finishImportSubscription(group)
                    }
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()

        }

    }

    private suspend fun finishImportSubscription(subscription: ProxyGroup) {
        GroupManager.createGroup(subscription)
        // GroupUpdater.startUpdate(subscription, true)
    }

    suspend fun importProfile(uri: Uri) {
        val profile = try {
            parseShareLinks(uri.toString()).getOrNull(0) ?: error(getString(R.string.no_proxies_found))
        } catch (e: Exception) {
            onMainDispatcher {
                alert(e.readableMessage).show()
            }
            return
        }

        onMainDispatcher {
            MaterialAlertDialogBuilder(this@MainActivity).setTitle(R.string.profile_import)
                .setMessage(getString(R.string.profile_import_message, profile.displayName()))
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    runOnDefaultDispatcher {
                        finishImportProfile(profile)
                    }
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }

    }

    private suspend fun finishImportProfile(profile: AbstractBean) {
        val targetId = DataStore.selectedGroupForImport()

        ProfileManager.createProfile(targetId, profile)

        onMainDispatcher {
            displayFragmentWithId(R.id.nav_configuration)

            snackbar(resources.getQuantityString(R.plurals.added, 1, 1)).show()
        }
    }

    override fun missingPlugin(profileName: String, pluginName: String) {
        val pluginEntity = PluginEntry.find(pluginName)
        if (pluginEntity == null) {
            snackbar(getString(R.string.plugin_unknown, pluginName)).show()
            return
        }

        MaterialAlertDialogBuilder(this).setTitle(R.string.missing_plugin)
            .setMessage(
                getString(
                    R.string.profile_requiring_plugin, profileName, getString(pluginEntity.nameId)
                )
            ).show()
    }

    override fun onNavigationItemSelected(item: MenuItem): Boolean {
        if (item.isChecked) binding.drawerLayout.closeDrawers() else {
            return displayFragmentWithId(item.itemId)
        }
        return true
    }


    fun displayFragment(fragment: ToolbarFragment) {
        if (fragment !is LogcatFragment) {
            binding.fab.show()
        }
        supportFragmentManager.beginTransaction()
            .replace(R.id.fragment_holder, fragment)
            .commitAllowingStateLoss()
        binding.drawerLayout.closeDrawers()
    }

    fun displayFragmentWithId(@IdRes id: Int): Boolean {
        when (id) {
            R.id.nav_configuration -> {
                displayFragment(ConfigurationFragment())
                connection.bandwidthTimeout = connection.bandwidthTimeout
            }
            R.id.nav_group -> displayFragment(GroupFragment())
            R.id.nav_route -> displayFragment(RouteFragment())
            R.id.nav_settings -> displayFragment(SettingsFragment())
            R.id.nav_logcat -> displayFragment(LogcatFragment())
            R.id.nav_about -> displayFragment(AboutFragment())
            else -> return false
        }
        navigation.menu.findItem(id).isChecked = true
        return true
    }

    fun ruleCreated() {
        navigation.menu.findItem(R.id.nav_route).isChecked = true
        supportFragmentManager.beginTransaction()
            .replace(R.id.fragment_holder, RouteFragment())
            .commitAllowingStateLoss()
        if (SagerNet.started) {
            snackbar(getString(R.string.restart)).setAction(R.string.apply) {
                SagerNet.reloadService()
            }.show()
        }
    }

    var state = BaseService.State.Idle

    private fun changeState(
        state: BaseService.State,
        msg: String? = null,
        animate: Boolean = false,
    ) {
        val started = state == BaseService.State.Connected

        if (!started) {
            statsUpdated(emptyList())
        }

        binding.fab.changeState(state, this.state, animate)
        binding.stats.changeState(state)
        if (msg != null) snackbar(msg).show()
        this.state = state

        when (state) {
            BaseService.State.Connected, BaseService.State.Stopped -> {
                statsUpdated(emptyList())
                runOnDefaultDispatcher {
                    // refresh view
                    ProfileManager.postUpdate(DataStore.currentProfile)
                }
            }
            else -> {}
        }
    }

    override fun snackbarInternal(text: CharSequence): Snackbar {
        return Snackbar.make(binding.coordinator, text, Snackbar.LENGTH_LONG).apply {
            anchorView = binding.fab
        }
    }

    override fun stateChanged(state: BaseService.State, profileName: String?, msg: String?) {
        changeState(state, msg, true)
    }

    override fun statsUpdated(stats: List<AppStats>) {
        // The traffic statistics screen has been removed from the drawer;
        // stats are still published to the notification by the service.
    }

    override fun routeAlert(type: Int, routeName: String) {
        val markwon = Markwon.create(this)
        when (type) {
            Alerts.ROUTE_ALERT_NOT_VPN -> {
                val message = markwon.toMarkdown(getString(R.string.route_need_vpn, routeName))
                Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            }
            Alerts.ROUTE_ALERT_NEED_COARSE_LOCATION_ACCESS -> {
                val message = markwon.toMarkdown(getString(R.string.route_need_coarse_location, routeName))
                MaterialAlertDialogBuilder(this).setTitle(R.string.missing_permission)
                    .setMessage(message)
                    .setNeutralButton(R.string.open_settings) { _, _ ->
                        try {
                            startActivity(Intent().apply {
                                action = Settings.ACTION_APPLICATION_DETAILS_SETTINGS
                                data = Uri.fromParts(
                                    "package", packageName, null
                                )
                            })
                        } catch (e: Exception) {
                            snackbar(e.readableMessage).show()
                        }
                    }
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
            Alerts.ROUTE_ALERT_NEED_FINE_LOCATION_ACCESS -> {
                val message = markwon.toMarkdown(getString(R.string.route_need_fine_location, routeName))
                MaterialAlertDialogBuilder(this).setTitle(R.string.missing_permission)
                    .setMessage(message)
                    .setNeutralButton(R.string.open_settings) { _, _ ->
                        try {
                            startActivity(Intent().apply {
                                action = Settings.ACTION_APPLICATION_DETAILS_SETTINGS
                                data = Uri.fromParts(
                                    "package", packageName, null
                                )
                            })
                        } catch (e: Exception) {
                            snackbar(e.readableMessage).show()
                        }
                    }
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
            Alerts.ROUTE_ALERT_NEED_BACKGROUND_LOCATION_ACCESS -> {
                val message = markwon.toMarkdown(getString(R.string.route_need_background_location, routeName))
                MaterialAlertDialogBuilder(this).setTitle(R.string.missing_permission)
                    .setMessage(message)
                    .setNeutralButton(R.string.open_settings) { _, _ ->
                        try {
                            startActivity(Intent().apply {
                                action = Settings.ACTION_APPLICATION_DETAILS_SETTINGS
                                data = Uri.fromParts(
                                    "package", packageName, null
                                )
                            })
                        } catch (e: Exception) {
                            snackbar(e.readableMessage).show()
                        }
                    }
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
            Alerts.ROUTE_ALERT_LOCATION_DISABLED -> {
                val message = markwon.toMarkdown(getString(R.string.route_need_location_enabled, routeName))
                MaterialAlertDialogBuilder(this).setTitle(R.string.location_disabled)
                    .setMessage(message)
                    .setNeutralButton(R.string.open_settings) { _, _ ->
                        try {
                            startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
                        } catch (e: Exception) {
                            snackbar(e.readableMessage).show()
                        }
                    }
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
            Alerts.ROUTE_ALERT_ALL_PACKAGES_UNINSTALLED -> {
                val message = markwon.toMarkdown("Ignored $routeName because all packages are uninstalled.")
                Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            }
        }
    }

    val connection = SagerConnection(true)
    override fun onServiceConnected(service: ISagerNetService) = changeState(try {
        BaseService.State.entries[service.state].also {
            SagerNet.started = it.canStop
        }
    } catch (_: RemoteException) {
        BaseService.State.Idle
    })

    override fun onServiceDisconnected() = changeState(BaseService.State.Idle)
    override fun onBinderDied() {
        connection.disconnect(this)
        connection.connect(this, this)
    }

    private val connect = registerForActivityResult(VpnRequestActivity.StartService()) {
        if (it) snackbar(R.string.vpn_permission_denied).show()
    }

    override fun trafficUpdated(profileId: Long, stats: TrafficStats, isCurrent: Boolean) {
        if (profileId == 0L) return

        if (isCurrent) binding.stats.updateTraffic(
            stats.txRateProxy, stats.rxRateProxy
        )

        runOnDefaultDispatcher {
            ProfileManager.postTrafficUpdated(profileId, stats)
        }
    }

    override fun profilePersisted(profileId: Long) {
        runOnDefaultDispatcher {
            ProfileManager.postUpdate(profileId)
        }
    }

    override fun observatoryResultsUpdated(groupId: Long) {
        runOnDefaultDispatcher {
            GroupManager.postReload(groupId)
        }
    }

    override fun onPreferenceDataStoreChanged(store: PreferenceDataStore, key: String) {
        when (key) {
            Key.SERVICE_MODE -> onBinderDied()
            Key.PROXY_APPS, Key.BYPASS_MODE, Key.INDIVIDUAL -> {
                if (state.canStop) {
                    snackbar(getString(R.string.restart)).setAction(R.string.apply) {
                        SagerNet.reloadService()
                    }.show()
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        connection.bandwidthTimeout = 1000
        GroupManager.userInterface = userInterface
    }

    override fun onStop() {
        connection.bandwidthTimeout = 0
        GroupManager.userInterface = null
        super.onStop()
    }

    override fun onDestroy() {
        super.onDestroy()
        DataStore.configurationStore.unregisterChangeListener(this)
        connection.disconnect(this)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                if (super.onKeyDown(keyCode, event)) return true
                binding.drawerLayout.open()
                navigation.requestFocus()
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> {
                if (binding.drawerLayout.isOpen) {
                    binding.drawerLayout.close()
                    return true
                }
            }
        }

        if (super.onKeyDown(keyCode, event)) return true
        if (binding.drawerLayout.isOpen) return false

        val fragment = supportFragmentManager.findFragmentById(R.id.fragment_holder) as? ToolbarFragment
        return fragment != null && fragment.onKeyDown(keyCode, event)
    }

}