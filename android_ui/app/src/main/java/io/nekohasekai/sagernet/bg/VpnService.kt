/******************************************************************************
 *                                                                            *
 * openppp2 fork: VpnService now drives the libopenppp2.so native core        *
 * instead of sing-box's Tun2ray. The Exclave service shell (BaseService      *
 * state machine, binder, notifications) is preserved.                        *
 *                                                                            *
 * Startup sequence (mirrors the proven openppp2 PppVpnService):              *
 *   set_root_path -> set_app_configuration -> set_dns_bcl                    *
 *   -> set_bypass_ip_list / set_dns_rules_list / set_geo_rules               *
 *   -> VpnService.Builder.establish() -> detachFd                            *
 *   -> set_protect_enabled -> set_network_interface -> set_mux_acceleration  *
 *   -> run(0) on a dedicated 8 MiB-stack thread                              *
 *                                                                            *
 * Root fix (Nokia 9 / Android 9): delete the netd 11500 fwmark 0xc3 bypass   *
 * rules so Chrome sockets cannot dodge the tunnel.                           *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.bg

import android.annotation.SuppressLint
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Network
import android.net.ProxyInfo
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.os.PowerManager
import android.os.SystemClock
import android.system.OsConstants
import android.util.Log
import io.nekohasekai.sagernet.Action
import io.nekohasekai.sagernet.Key
import io.nekohasekai.sagernet.R
import io.nekohasekai.sagernet.SagerNet
import io.nekohasekai.sagernet.bg.proto.OpenPPP2Instance
import io.nekohasekai.sagernet.database.DataStore
import io.nekohasekai.sagernet.ktx.Logs
import io.nekohasekai.sagernet.ui.VpnRequestActivity
import io.nekohasekai.sagernet.utils.DefaultNetworkListener
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.launch
import org.json.JSONObject
import supersocksr.ppp.android.c.libopenppp2
import android.net.VpnService as BaseVpnService

@SuppressLint("VpnServicePolicy")
class VpnService : BaseVpnService(),
    BaseService.Interface {

    companion object {
        const val TAG = "OpenPPP2VpnService"
        const val DEFAULT_MTU = 1400
        private const val IPV6_BLOCK_ADDRESS = "fd00:6f70:656e:7070::2"
        private const val VPN_THREAD_STACK = 32L * 1024 * 1024

        @Volatile
        var instance: VpnService? = null
            private set

        @Volatile
        var isRunning = false
            private set
    }

    private var vpnInterface: ParcelFileDescriptor? = null
    private var vpnThread: Thread? = null
    private var linkStateThread: HandlerThread? = null
    private var linkStateHandler: Handler? = null
    override var wakeLock: PowerManager.WakeLock? = null
    private var active = false

    @Volatile
    var underlyingNetwork: Network? = null
        set(value) {
            field = value
            if (active && Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP_MR1) {
                setUnderlyingNetworks(underlyingNetworks)
            }
        }
    private val underlyingNetworks
        get() = underlyingNetwork?.let { arrayOf(it) }
    private var networkListenerIsRunning = false

    override suspend fun startProcesses() {
        startVpn()
        super.startProcesses()
    }

    @SuppressLint("WakelockTimeout")
    override fun acquireWakeLock() {
        wakeLock = SagerNet.power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "openppp2:vpn")
            .apply { acquire() }
    }

    @Suppress("EXPERIMENTAL_API_USAGE")
    override fun killProcesses() {
        data.proxy?.close()
        try {
            libopenppp2.stop()
        } catch (e: Throwable) {
            Logs.w(e)
        }
        removeTun0OverrideRules()
        stopLinkStatePoller()
        vpnThread?.interrupt()
        vpnThread = null
        try {
            vpnInterface?.close()
        } catch (_: Throwable) {
        }
        vpnInterface = null
        isRunning = false
        active = false
        super.killProcesses()
        networkListenerIsRunning = false
        GlobalScope.launch(Dispatchers.Default) { DefaultNetworkListener.stop(this) }
    }

    override fun onBind(intent: Intent) = when (intent.action) {
        SERVICE_INTERFACE -> super<BaseVpnService>.onBind(intent)
        else -> super<BaseService.Interface>.onBind(intent)
    }

    override val data = BaseService.Data(this)
    override val tag = "OpenPPP2VpnService"
    override fun createNotification(profileName: String) =
        ServiceNotification(this, profileName, "service-vpn", true)

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (DataStore.serviceMode == Key.MODE_VPN) {
            if (prepare(this) != null) {
                startActivity(
                    Intent(
                        this, VpnRequestActivity::class.java
                    ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            } else return super<BaseService.Interface>.onStartCommand(intent, flags, startId)
        }
        stopRunner()
        return Service.START_NOT_STICKY
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
        data.binder.close()
    }

    override suspend fun preInit() {
        networkListenerIsRunning = true
        DefaultNetworkListener.start(this) {
            if (networkListenerIsRunning) {
                underlyingNetwork = it
            }
        }
    }

    inner class NullConnectionException : NullPointerException(),
        BaseService.ExpectedException {
        override fun getLocalizedMessage() = getString(R.string.reboot_required)
    }

    /**
     * Native -> Kotlin bridge target used by libopenppp2.start_exec().
     */
    fun onStarted(key: Int) {
        Log.i(TAG, "VPN started with key: $key")
        if (data.state == BaseService.State.Connecting) {
            data.changeState(BaseService.State.Connected)
        }
    }

    /**
     * Native -> Kotlin bridge target used by libopenppp2.runtime_snapshot().
     * The Exclave UI does not need the full snapshot; log phase changes at
     * debug level only.
     */
    fun onRuntimeSnapshot(json: String) {
        if (json.isBlank()) return
        try {
            val root = JSONObject(json)
            val phase = root.optString("phase")
            if (phase.isNotEmpty() && phase != "idle") {
                Log.d(TAG, "runtime phase=$phase")
            }
            val lastError = root.optJSONObject("last_error")
            if (lastError != null && !lastError.isNull("user_message_key")) {
                val message = lastError.optString("user_message_key", "")
                if (message.isNotEmpty()) {
                    Logs.w("openppp2 runtime error: $message")
                }
            }
        } catch (_: Throwable) {
        }
    }

    private fun startLinkStatePoller() {
        if (linkStateThread != null) return
        val t = HandlerThread("openppp2-linkstate").also { it.start() }
        linkStateThread = t
        val h = Handler(t.looper)
        linkStateHandler = h
        h.post(object : Runnable {
            override fun run() {
                if (!isRunning) return
                val ls = try {
                    libopenppp2.get_link_state()
                } catch (e: Throwable) {
                    Logs.w(e)
                    6
                }
                publishLinkState(ls)
                linkStateHandler?.postDelayed(this, 1000L)
            }
        })
    }

    private fun stopLinkStatePoller() {
        linkStateHandler?.removeCallbacksAndMessages(null)
        linkStateHandler = null
        linkStateThread?.quitSafely()
        linkStateThread = null
    }

    private fun publishLinkState(value: Int) {
        // 1 connecting / 2 connected / 3 stopping / 6 stopped
        when (value) {
            1 -> if (data.state == BaseService.State.Stopped || data.state == BaseService.State.Idle) {
                data.changeState(BaseService.State.Connecting)
            }
            2 -> if (data.state == BaseService.State.Connecting) {
                data.changeState(BaseService.State.Connected)
            }
            3 -> data.changeState(BaseService.State.Stopping)
            6 -> if (data.state != BaseService.State.Stopped) {
                data.changeState(BaseService.State.Stopped)
            }
        }
    }

    private fun startVpn() {
        instance = this
        val proxy = data.proxy as? OpenPPP2Instance ?: error("core not started")
        val configJson = proxy.config.configJson
        val vpnOptionsJson = proxy.config.vpnOptionsJson

        val options = JSONObject(vpnOptionsJson)
        val vpnIp = options.optString("tunIp", "10.0.0.2")
        val vpnMask = options.optString("tunMask", "255.255.255.0")
        val vpnPrefix = options.optInt("tunPrefix", 24)
        val route = options.optString("route", "0.0.0.0")
        val routePrefix = options.optInt("routePrefix", 0)
        val dns1 = options.optString("dns1", "8.8.8.8")
        val dns2 = options.optString("dns2", "8.8.4.4")
        val dnsDirect1 = options.optString("dnsDirect1", "")
        val dnsDirect2 = options.optString("dnsDirect2", "")
        val mtu = options.optInt("mtu", 1400)
        val mark = options.optInt("mark", 0)
        var mux = options.optInt("mux", 0)
        val vnet = options.optBoolean("vnet", false)
        val blockQuic = options.optBoolean("blockQuic", false)
        val staticMode = options.optBoolean("staticMode", false)
        val proxyOnly = options.optBoolean("proxyOnly", false)
        val bypassIpList = options.optString("bypassIpList", "")
        val dnsRulesList = options.optString("dnsRulesList", "")
        val routeMode = options.optString("routeMode", "")
        val extraArgs = options.optString("extraArgs", "")

        // 基础分流（ip）模式：优先使用 files/rules/ 下的桌面三文件。
        var effectiveBypass = bypassIpList
        var effectiveDnsRules = dnsRulesList
        if (routeMode == "basic") {
            val rulesDir = java.io.File(filesDir, "rules")
            val ipFile = java.io.File(rulesDir, "ip.txt")
            if (ipFile.isFile && ipFile.length() > 0L) {
                val ipv6File = java.io.File(rulesDir, "ipv6.txt")
                val combined = buildString {
                    append(ipFile.readText(Charsets.UTF_8).trim())
                    if (ipv6File.isFile && ipv6File.length() > 0L) {
                        append("\n")
                        append(ipv6File.readText(Charsets.UTF_8).trim())
                    }
                }
                effectiveBypass = combined
            }
            val dnsFile = java.io.File(rulesDir, "dns-rules.txt")
            if (dnsFile.isFile && dnsFile.length() > 0L) {
                effectiveDnsRules = dnsFile.readText(Charsets.UTF_8)
            }
        }
        var muxAcceleration = -1
        for (arg in extraArgs.split(Regex("\\s+"))) {
            val trimmed = arg.trim()
            if (trimmed.startsWith("--tun-mux-acceleration=")) {
                trimmed.removePrefix("--tun-mux-acceleration=").trim().toIntOrNull()
                    ?.let { muxAcceleration = it }
            } else if (trimmed.startsWith("--tun-mux=")) {
                trimmed.removePrefix("--tun-mux=").trim().toIntOrNull()
                    ?.let { if (it > 0) mux = it }
            }
        }
        val directDns = listOf(dnsDirect1, dnsDirect2).map { it.trim() }.filter { it.isNotEmpty() }
        Log.i(TAG, "vpn options tunIp=$vpnIp/$vpnPrefix mtu=$mtu mux=$mux proxyOnly=$proxyOnly routeMode=$routeMode")

        // Anchor relative paths in the AppConfiguration JSON to filesDir.
        try {
            libopenppp2.set_root_path(filesDir.absolutePath)
        } catch (_: UnsatisfiedLinkError) {
        }

        val configResult = libopenppp2.set_app_configuration(configJson)
        Log.i(TAG, "set_app_configuration result=$configResult")
        if (configResult != 0) {
            throw IllegalStateException(
                "set_app_configuration failed: $configResult, error: ${libopenppp2.get_last_error_text()}"
            )
        }

        // Sync VPN DNS servers into the native vdns upstream list.
        run {
            val configRoot = try {
                JSONObject(configJson)
            } catch (_: Throwable) {
                JSONObject()
            }
            val udpDns = configRoot.optJSONObject("udp")?.optJSONObject("dns")
            val turbo = udpDns?.optBoolean("turbo", false) ?: false
            val ttl = (udpDns?.optInt("ttl", 60) ?: 60).coerceAtLeast(1)
            val dnsBcl = listOf(dns1, dns2).filter { it.isNotBlank() }.joinToString(",")
            if (dnsBcl.isNotBlank()) {
                libopenppp2.set_dns_bcl(turbo, ttl, dnsBcl)
            }
        }

        if (effectiveBypass.isNotBlank()) {
            libopenppp2.set_bypass_ip_list(effectiveBypass)
        }
        if (effectiveDnsRules.isNotBlank()) {
            libopenppp2.set_dns_rules_list(effectiveDnsRules)
        }

        // GEO routing preset (single tunnel final).
        val geoRules = options.optJSONObject("geoRules")
        val geoEnabled = !proxyOnly && geoRules?.optBoolean("enabled", false) == true
        if (geoEnabled) {
            val geoCountry = normalizeGeoCountry(geoRules.optString("country", "cn"))
            if (!writeGeoRulesPreset(geoCountry, directDns)) {
                throw IllegalStateException("generate GEO preset failed: $geoCountry")
            }
            val geoResult = libopenppp2.set_geo_rules(
                "./rules/geo-rules.yaml", "./rules/GeoSite.dat", "./rules/GeoIP.dat"
            )
            if (!geoResult) {
                throw IllegalStateException("set_geo_rules failed")
            }
        }

        val builder = Builder()
            .setSession(getString(R.string.app_name))
            .addAddress(vpnIp, vpnPrefix)
            .allowFamily(OsConstants.AF_INET)
            .setMtu(mtu)
            .setBlocking(true)

        if (proxyOnly) {
            builder.addRoute(vpnIp, vpnPrefix)
        } else {
            builder.addRoute(route, routePrefix)
        }

        if (!proxyOnly) {
            builder.addAddress(IPV6_BLOCK_ADDRESS, 128)
            builder.addRoute("::", 0)
            builder.allowFamily(OsConstants.AF_INET6)
        }

        if (!proxyOnly) {
            if (dns1.isNotBlank()) builder.addDnsServer(dns1)
            if (dns2.isNotBlank()) builder.addDnsServer(dns2)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP && mark != 0) {
            builder.setConfigureIntent(SagerNet.configureIntent(this))
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            builder.setMetered(false)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP_MR1) {
            builder.setUnderlyingNetworks(underlyingNetworks)
        }

        // ---- Per-app proxy ----
        val perAppEnabled = options.optBoolean("perAppProxyEnabled", false)
        val perAppMode = options.optString("perAppProxyMode", "allow")
        val perAppApps = options.optJSONArray("perAppProxyApps")
        if (perAppEnabled && perAppApps != null && perAppApps.length() > 0) {
            var added = 0
            var skipped = 0
            for (i in 0 until perAppApps.length()) {
                val pkg = perAppApps.optString(i, "")
                if (pkg.isBlank() || pkg == this.packageName) {
                    skipped++
                    continue
                }
                try {
                    if (perAppMode == "deny") {
                        builder.addDisallowedApplication(pkg)
                    } else {
                        builder.addAllowedApplication(pkg)
                    }
                    added++
                } catch (e: PackageManager.NameNotFoundException) {
                    skipped++
                } catch (e: Throwable) {
                    skipped++
                    Logs.w(e)
                }
            }
            if (perAppMode == "deny") {
                try {
                    builder.addDisallowedApplication(this.packageName)
                } catch (_: Throwable) {
                }
            }
            Log.i(TAG, "per-app proxy mode=$perAppMode applied=$added skipped=$skipped")
        } else {
            try {
                builder.addDisallowedApplication(this.packageName)
            } catch (_: Throwable) {
            }
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && DataStore.appendHttpProxy && DataStore.requireHttp) {
            builder.setHttpProxy(android.net.ProxyInfo.buildDirectProxy("127.0.0.1", DataStore.httpPort))
        }
        if (DataStore.allowAppsBypassVpn) {
            builder.allowBypass()
        }

        vpnInterface = builder.establish()
        if (vpnInterface == null) {
            throw NullConnectionException()
        }
        active = true

        // Enable the native protect(fd) bridge.
        try {
            libopenppp2.set_protect_enabled(true)
        } catch (e: Throwable) {
            Logs.w(e)
        }

        // detachFd() transfers ownership of the fd to native code.
        val tunFd = vpnInterface!!.detachFd()
        vpnInterface = null

        val niResult = libopenppp2.set_network_interface(
            tunFd, mux, vnet, blockQuic, staticMode, vpnIp, vpnMask, IPV6_BLOCK_ADDRESS
        )
        if (niResult != 0) {
            throw IllegalStateException(
                "set_network_interface failed: $niResult, error: ${libopenppp2.get_last_error_text()}"
            )
        }

        if (muxAcceleration > 0) {
            try {
                libopenppp2.set_mux_acceleration(muxAcceleration)
            } catch (_: UnsatisfiedLinkError) {
            }
        }

        // ---- Force all traffic through the tunnel (root) ----
        // Android 8+ lets apps bind sockets to a physical network (Chrome
        // binds WiFi netId 195 -> fwmark 0xc3). netd installs `ip rule 11500
        // fwmark 0xc3 lookup wlan0` ABOVE the VPN rule (12000, tun0), so
        // those sockets bypass the tunnel. With root we delete the 11500
        // rules. Verified on Nokia 9 / Android 9.
        if (!proxyOnly) {
            installTun0OverrideRules()
        }

        // Start VPN in background thread (run() is blocking). 8 MiB stack for
        // boost::asio deep handler chains.
        isRunning = true
        startLinkStatePoller()
        vpnThread = Thread(null, Runnable {
            try {
                Log.i(TAG, "vpnThread started, calling run(0)")
                val result = libopenppp2.run(0)
                Log.i(TAG, "libopenppp2.run returned=$result")
                if (result != 0) {
                    Logs.w("libopenppp2 last error=${libopenppp2.get_last_error_text()}")
                }
            } catch (e: Throwable) {
                Logs.w(e)
            } finally {
                isRunning = false
                removeTun0OverrideRules()
                stopLinkStatePoller()
                releaseWakeLock()
                try {
                    vpnInterface?.close()
                } catch (_: Throwable) {
                }
                vpnInterface = null
                if (data.state != BaseService.State.Stopped && data.state != BaseService.State.Stopping) {
                    data.changeState(BaseService.State.Stopped)
                }
            }
        }, "openppp2-vpn-thread", VPN_THREAD_STACK).also { it.start() }
    }

    override fun onRevoke() = stopRunner()

    // ------------------------------------------------------------------
    // Root-based routing override (ported from openppp2 PppVpnService).
    // ------------------------------------------------------------------

    private fun installTun0OverrideRules() {
        if (!isRootAvailable()) {
            Log.i(TAG, "tun0 override: root not available, skipping (Chrome may bypass VPN)")
            return
        }
        val commands = listOf(
            "ip rule del from all fwmark 0xc3/0xffff iif lo lookup wlan0 pref 11500",
            "ip rule del from all fwmark 0x100c3/0x1ffff iif lo lookup wlan0 pref 11500"
        )
        val results = runRootCommands(commands)
        var deleted = 0
        for ((cmd, ok) in commands.zip(results)) {
            if (ok) {
                deleted++
                Log.i(TAG, "tun0 override: deleted $cmd")
            }
        }
        Log.i(TAG, "tun0 override deleted $deleted/${commands.size} bypass rules")
        val remaining = runRootCommand("ip rule show | grep 0xc3")
        Log.i(TAG, "tun0 override: remaining 0xc3 rules: ${remaining?.trim().orEmpty()}")
    }

    private fun removeTun0OverrideRules() {
        if (!isRootAvailable()) return
        val commands = listOf(
            "ip rule add from all fwmark 0xc3/0xffff iif lo lookup wlan0 pref 11500",
            "ip rule add from all fwmark 0x100c3/0x1ffff iif lo lookup wlan0 pref 11500"
        )
        val results = runRootCommands(commands)
        for ((cmd, ok) in commands.zip(results)) {
            if (ok) {
                Log.i(TAG, "tun0 override: restored $cmd")
            }
        }
    }

    private fun isRootAvailable(): Boolean {
        return try {
            val process = ProcessBuilder("su", "-c", "id").redirectErrorStream(true).start()
            val output = process.inputStream.bufferedReader().readText().trim()
            process.waitFor() == 0 && output.contains("uid=0")
        } catch (e: Throwable) {
            false
        }
    }

    private fun runRootCommand(cmd: String): String? {
        return try {
            val process = ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start()
            val output = process.inputStream.bufferedReader().readText().trim()
            val ok = process.waitFor() == 0
            if (ok) output else null
        } catch (e: Throwable) {
            null
        }
    }

    private fun runRootCommands(commands: List<String>): List<Boolean> {
        return commands.map { cmd -> runRootCommand(cmd) != null }
    }

    private fun normalizeGeoCountry(value: String?): String {
        val country = value.orEmpty().trim().lowercase(java.util.Locale.ROOT)
        return if (country.matches(Regex("^[a-z]{2}$"))) country else "cn"
    }

    private fun writeGeoRulesPreset(country: String, directDns: List<String>): Boolean {
        return try {
            val rulesDir = java.io.File(filesDir, "rules")
            if (!rulesDir.exists() && !rulesDir.mkdirs()) return false
            val rulesFile = java.io.File(rulesDir, "geo-rules.yaml")
            val dnsList = if (directDns.isNotEmpty()) directDns else if (country == "cn") {
                listOf("local", "223.5.5.5", "119.29.29.29")
            } else {
                listOf("local", "1.1.1.1", "8.8.8.8")
            }
            val content = buildString {
                appendLine("# Generated by OpenPPP2 Android. Do not edit.")
                appendLine("version: 1")
                appendLine("final: tunnel")
                appendLine()
                appendLine("direct_dns:")
                for (dns in dnsList) appendLine("  - $dns")
                appendLine()
                appendLine("rules:")
                appendLine("  - ip-cidr,10.0.0.0/8,direct")
                appendLine("  - ip-cidr,100.64.0.0/10,direct")
                appendLine("  - ip-cidr,127.0.0.0/8,direct")
                appendLine("  - ip-cidr,169.254.0.0/16,direct")
                appendLine("  - ip-cidr,172.16.0.0/12,direct")
                appendLine("  - ip-cidr,192.168.0.0/16,direct")
                appendLine("  - domain-suffix,$country,direct")
                appendLine("  - geosite,$country,direct")
                appendLine("  - geoip,$country,direct")
            }
            rulesFile.writeText(content, Charsets.UTF_8)
            rulesFile.isFile && rulesFile.length() > 0L
        } catch (e: Throwable) {
            Logs.w(e)
            false
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { lock ->
            if (lock.isHeld) {
                lock.release()
            }
        }
        wakeLock = null
    }
}
