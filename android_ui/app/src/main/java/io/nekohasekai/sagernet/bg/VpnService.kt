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
import io.nekohasekai.sagernet.database.ProfileManager
import io.nekohasekai.sagernet.ktx.Logs
import io.nekohasekai.sagernet.ui.VpnRequestActivity
import io.nekohasekai.sagernet.utils.DefaultNetworkListener
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.launch
import org.json.JSONObject
import supersocksr.ppp.android.c.libopenppp2
import android.net.VpnService as BaseVpnService

/** Private/LAN CIDRs backing the "bypass private addresses" rule card.
 *  Concrete CIDRs keep the rule independent of GeoIP.dat categories
 *  (the stock v2fly geoip.dat has no "private" entry). */
private val PRIVATE_LAN_CIDRS = listOf(
    "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
    "169.254.0.0/16", "172.16.0.0/12", "192.168.0.0/16",
)

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
    private var activeGatewayLoopback: String? = null
    private var gatewayFixVerified = false
    private var gatewayFixLastFullCheck = 0L

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
        activeGatewayLoopback?.let { removeGatewayLoopbackFix(it) }
        activeGatewayLoopback = null
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
            2 -> {
                if (data.state == BaseService.State.Connecting) {
                    data.changeState(BaseService.State.Connected)
                }
                // netd assigns a NEW route-table id every time the VPN network
                // is (re)created, so the gateway-loopback rule installed at
                // startVpn() may target a stale table.  Re-check now that the
                // link is up and repair the rule to match the CURRENT table.
                ensureGatewayLoopbackFix()
            }
            3 -> data.changeState(BaseService.State.Stopping)
            6 -> {
                if (data.state != BaseService.State.Stopped) {
                    data.changeState(BaseService.State.Stopped)
                }
                gatewayFixVerified = false
            }
        }
    }

    private suspend fun startVpn() {
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
        val mux = options.optInt("mux", 0)
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
        val muxAcceleration = options.optInt("muxAcceleration", -1)
        // extraArgs may still override mux / mux-acceleration.
        var effectiveMuxAcceleration = muxAcceleration
        var effectiveMux = mux
        for (arg in extraArgs.split(Regex("\\s+"))) {
            val trimmed = arg.trim()
            if (trimmed.startsWith("--tun-mux-acceleration=")) {
                trimmed.removePrefix("--tun-mux-acceleration=").trim().toIntOrNull()
                    ?.let { effectiveMuxAcceleration = it }
            } else if (trimmed.startsWith("--tun-mux=")) {
                trimmed.removePrefix("--tun-mux=").trim().toIntOrNull()
                    ?.let { if (it > 0) effectiveMux = it }
            }
        }
        val directDns = listOf(dnsDirect1, dnsDirect2).map { it.trim() }.filter { it.isNotEmpty() }
        Log.i(TAG, "vpn options tunIp=$vpnIp/$vpnPrefix mtu=$mtu mux=$effectiveMux proxyOnly=$proxyOnly routeMode=$routeMode")

        // Anchor relative paths in the AppConfiguration JSON to filesDir.
        try {
            libopenppp2.set_root_path(filesDir.absolutePath)
        } catch (_: UnsatisfiedLinkError) {
        }

        // Sync the in-app LogLevel setting (0=NONE, 1=ERROR, 2=WARNING,
        // 3=INFO, 4=DEBUG) into the native core so LOG_INFO / LOG_DEBUG
        // output (e.g. DirectDNS diagnostics) follows the drawer log level.
        try {
            libopenppp2.set_log_level(DataStore.logLevel)
        } catch (_: UnsatisfiedLinkError) {
        }

        // Ensure bundled rule assets (GeoSite.dat / GeoIP.dat / ip.txt /
        // ipv6.txt / dns-rules.txt) exist under files/rules before the
        // native core tries to load them.
        ensureRuleAssets()

        // Apply the in-app inbound settings (SOCKS5 + HTTP proxy) on top of
        // the profile configuration so the toggles/ports/password actually
        // drive what the native core listens on:
        //  - requireSocks  -> socks-proxy.port = socksPort, bind 127.0.0.1
        //    by default; 0.0.0.0 when allowAccess is on (LAN reachable),
        //    username/password from the settings when set.
        //  - requireHttp   -> http-proxy.port = httpPort, bind 127.0.0.1
        //    by default; 0.0.0.0 when allowAccess is on (LAN reachable).
        // A port of 0 disables the corresponding local listener natively
        // (VEthernetLocalProxySwitcher::Open rejects bind_port <= MinPort),
        // so turning a toggle off fully disables that inbound.
        val effectiveConfigJson = run {
            val root = try {
                JSONObject(configJson)
            } catch (_: Throwable) {
                JSONObject()
            }
            val client = root.optJSONObject("client") ?: JSONObject().also { root.put("client", it) }
            val inboundBind = if (DataStore.allowAccess) "0.0.0.0" else "127.0.0.1"

            val httpProxy = client.optJSONObject("http-proxy") ?: JSONObject().also { client.put("http-proxy", it) }
            if (DataStore.requireHttp) {
                httpProxy.put("port", DataStore.httpPort)
                httpProxy.put("bind", inboundBind)
            } else {
                httpProxy.put("port", 0)
                httpProxy.put("bind", "127.0.0.1")
            }

            val socksProxy = client.optJSONObject("socks-proxy") ?: JSONObject().also { client.put("socks-proxy", it) }
            if (DataStore.requireSocks) {
                socksProxy.put("port", DataStore.socksPort)
                socksProxy.put("bind", inboundBind)
                socksProxy.put("username", DataStore.socksUsername ?: "")
                socksProxy.put("password", DataStore.socksPassword ?: "")
            } else {
                socksProxy.put("port", 0)
                socksProxy.put("bind", "127.0.0.1")
            }

            val tcp = root.optJSONObject("tcp") ?: JSONObject().also { root.put("tcp", it) }
            val connect = tcp.optJSONObject("connect") ?: JSONObject().also { tcp.put("connect", it) }
            if (connect.optInt("timeout", 0) <= 0) {
                connect.put("timeout", 15)
            }
            root.toString()
        }

        val configResult = libopenppp2.set_app_configuration(effectiveConfigJson)
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
            tunFd, effectiveMux, vnet, blockQuic, staticMode, vpnIp, vpnMask, IPV6_BLOCK_ADDRESS
        )
        if (niResult != 0) {
            throw IllegalStateException(
                "set_network_interface failed: $niResult, error: ${libopenppp2.get_last_error_text()}"
            )
        }

        if (effectiveMuxAcceleration > 0) {
            try {
                libopenppp2.set_mux_acceleration(effectiveMuxAcceleration)
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
            // vnet 回环修复：Android 系统 VpnService 自动规则（pref 12000）
            // 用 uidrange 排除 VPN 进程 uid，导致 vnet 模式的内核回环
            // SYN-ACK（目标=网关，如 10.0.0.1）被路由进空表丢弃。
            // 加一条高优先级规则：发往网关的包（只有回环流量）走 tun0 表。
            activeGatewayLoopback = gatewayFor(vpnIp, vpnMask)
            activeGatewayLoopback?.let { installGatewayLoopbackFix(it) }
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
                activeGatewayLoopback?.let { removeGatewayLoopbackFix(it) }
                activeGatewayLoopback = null
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

    // ------------------------------------------------------------------
    // vnet loopback fix (root).
    //
    // Android's per-VPN ip rules (pref 12000) carry
    //   uidrange 0-<vpnUid-1>/<vpnUid+1>-99999
    // which EXCLUDES the VPN process uid.  The vnet (kernel-loopback)
    // TCP path rewrites every LAN->WAN SYN to a loopback SYN targeting
    // the gateway (10.0.0.1:listenPort); the kernel's SYN-ACK reply is
    // owned by the VPN uid, so the 12000 rule does not match and the
    // packet falls through empty tables (99/98/97) into `unreachable`.
    // This high-priority rule forces packets destined to the gateway
    // (only the loopback handshake traffic) into the tun0 table.
    // ------------------------------------------------------------------
    private fun gatewayFor(ip: String, mask: String): String? {
        return try {
            val ipParts = ip.trim().split(".").map { it.toInt() and 0xff }
            val maskParts = mask.trim().split(".").map { it.toInt() and 0xff }
            if (ipParts.size != 4 || maskParts.size != 4) return null
            val net = IntArray(4) { i -> ipParts[i] and maskParts[i] }
            // gateway = network address + 1
            var carry = 1
            for (i in 3 downTo 0) {
                val v = net[i] + carry
                net[i] = v and 0xff
                carry = v shr 8
            }
            net.joinToString(".")
        } catch (e: Throwable) {
            null
        }
    }

    private fun installGatewayLoopbackFix(gateway: String) {
        if (!isRootAvailable()) {
            Log.i(TAG, "gateway loopback fix: root not available, skipping")
            return
        }
        val table = resolveVpnRouteTable()
        if (table == null) {
            Log.i(TAG, "gateway loopback fix: could not resolve vpn route table, skipping")
            return
        }
        // If the table is not routing through tun0 yet, leave it for
        // ensureGatewayLoopbackFix() (connected state) to retry with delay.
        if (!vpnTableHasDefault(table)) {
            Log.i(TAG, "gateway loopback fix: table $table has no tun0 default yet, will re-check at connected")
            return
        }
        val r = runRootCommand("ip rule add from all to $gateway lookup $table pref 11000")
        Log.i(TAG, "gateway loopback fix: add rule to $gateway table $table -> ${if (r != null) "ok" else "failed/dup"}")
        val show = runRootCommand("ip rule show pref 11000")
        Log.i(TAG, "gateway loopback fix: verify: ${show?.trim().orEmpty()}")
    }

    private fun removeGatewayLoopbackFix(gateway: String) {
        if (!isRootAvailable()) return
        // Try exact match first; fall back to pref-only match in case the table
        // number changed (e.g. after a package rename the netd table id differs).
        val table = resolveVpnRouteTable()
        if (table != null) {
            runRootCommand("ip rule del from all to $gateway lookup $table pref 11000")
        }
        runRootCommand("ip rule del from all to $gateway pref 11000")
        Log.i(TAG, "gateway loopback fix: removed rule to $gateway")
    }

    // Resolve the netd routing table used by our tun0 interface.  The table
    // id is NOT stable: it is derived from the per-VPN netId and changes when
    // the package/network is re-created, and netd may present it EITHER as a
    // numeric id (`table 1098`) or as the named table (`table tun0`), with the
    // numeric id coming from /data/misc/net/rt_tables.  Parse it from
    // `ip route show table all` instead of hard-coding any id.  We may return
    // either "tun0" or a numeric id; the kernel resolves both in
    // `ip rule add ... lookup <value>`.
    private fun resolveVpnRouteTable(): String? {
        val out = runRootCommand("ip route show table all") ?: return null
        // Numeric table id: `default dev tun0 ... table 1096`.
        Regex("default\\s+dev\\s+tun0\\b[^\n]*?table\\s+(\\d+)").find(out)?.let {
            return it.groupValues[1]
        }
        // Named table: `default dev tun0 ... table tun0` (rt_tables maps it).
        if (Regex("default\\s+dev\\s+tun0\\b[^\n]*?table\\s+tun0").containsMatchIn(out)) {
            return "tun0"
        }
        return null
    }

    // Does the resolved table currently carry a tun0 default route?  Guards
    // against installing the fix onto a stale/empty table (which makes the
    // SYN-ACK loopback silently drop -> curl 000).  Note: this Android
    // iproute2 cannot query a named table with `ip route show table tun0`
    // ("table id value is invalid"), so we work from the `table all` dump.
    private fun vpnTableHasDefault(table: String): Boolean {
        val out = runRootCommand("ip route show table all") ?: return false
        val def = Regex("default\\s+dev\\s+tun0\\b[^\n]*?table\\s+(\\S+)").find(out)
            ?: return false
        val shown = def.groupValues[1] // "tun0" or a numeric id
        if (table == shown) return true
        // numeric id that rt_tables maps to tun0 -> the dump prints "tun0"
        if (table.matches(Regex("\\d+")) && shown == "tun0") return true
        return false
    }

    // Re-install the gateway-loopback rule against the CURRENT netd table.
    // Called from publishLinkState() once the link is up, because netd may
    // have re-created the VPN network (assigning a new table id) after the
    // initial install inside startVpn().  Runs on the link-state poller
    // thread, so short sleeps for netd settling are safe.
    private fun ensureGatewayLoopbackFix() {
        val gateway = activeGatewayLoopback ?: return
        if (!isRootAvailable()) return

        // netd may still be finalizing the per-VPN table right after the link
        // comes up (we saw the table id / named-table display flip between
        // 1095/1096/tun0 while a second VPN app was competing).  Retry the
        // resolution until it yields a table that actually routes tun0.
        var table = resolveVpnRouteTable()
        var attempt = 0
        while ((table == null || !vpnTableHasDefault(table)) && attempt < 5) {
            Thread.sleep(1200)
            table = resolveVpnRouteTable()
            attempt++
        }
        if (table == null) {
            Log.i(TAG, "gateway loopback fix: re-check table unavailable after retries")
            return
        }
        if (!vpnTableHasDefault(table)) {
            Log.i(TAG, "gateway loopback fix: table $table has no tun0 default yet, retry later")
            return
        }

        val current = runRootCommand("ip rule show pref 11000")
        val already = current?.contains("to $gateway lookup $table") == true
        if (already) {
            gatewayFixVerified = true
            periodicRuleIntegrityCheck(gateway, table)
            return
        }
        // remove any stale rule (old table id) then add with the current one
        runRootCommand("ip rule del from all to $gateway pref 11000")
        val r = runRootCommand("ip rule add from all to $gateway lookup $table pref 11000")
        gatewayFixVerified = r != null
        Log.i(TAG, "gateway loopback fix: re-check -> table $table ${if (r != null) "ok" else "failed"}")
    }

    // Called on every connected poll (1s) but only does full work every ~15s:
    // netd can silently re-create the VPN network (new table id) while we are
    // up, which would leave our rule pointing at an empty table and break the
    // SYN-ACK loopback again.  Detect that and re-install immediately.
    private fun periodicRuleIntegrityCheck(gateway: String, table: String) {
        val now = SystemClock.elapsedRealtime()
        if (now - gatewayFixLastFullCheck < 15000) return
        gatewayFixLastFullCheck = now

        val show = runRootCommand("ip rule show pref 11000") ?: ""
        val ruleOk = show.contains("to $gateway lookup $table")
        val routeOk = vpnTableHasDefault(table)
        if (!ruleOk || !routeOk) {
            Log.i(TAG, "gateway loopback fix: stale rule detected (rule=$ruleOk route=$routeOk), re-installing")
            runRootCommand("ip rule del from all to $gateway pref 11000")
            val r = runRootCommand("ip rule add from all to $gateway lookup $table pref 11000")
            gatewayFixVerified = r != null
            gatewayFixLastFullCheck = 0
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

    /**
     * Copy the rule assets into files/rules so the native core can open them
     * by relative path:
     *   ./rules/GeoSite.dat   (geosite.dat -> GeoSite.dat is handled below)
     *   ./rules/GeoIP.dat
     *   ./rules/ip.txt, ./rules/ipv6.txt, ./rules/dns-rules.txt
     * The native core references "GeoSite.dat"/"GeoIP.dat" (capital letters);
     * the bundled files are named geosite.dat / geoip.dat, so the copies are
     * renamed to match what set_geo_rules() expects.
     *
     * Source priority: a user-updated copy in the writable assets directory
     * (imported or downloaded from the route-assets screen) wins over the
     * bundled APK copy.
     */
    private fun ensureRuleAssets() {
        try {
            val rulesDir = java.io.File(filesDir, "rules")
            if (!rulesDir.exists() && !rulesDir.mkdirs()) {
                Log.w(TAG, "ensureRuleAssets: cannot create $rulesDir")
                return
            }
            // assetName -> destination file name
            val mapping = linkedMapOf(
                "geosite.dat" to "GeoSite.dat",
                "geoip.dat" to "GeoIP.dat",
                "ip.txt" to "ip.txt",
                "ipv6.txt" to "ipv6.txt",
                "dns-rules.txt" to "dns-rules.txt",
            )
            for ((assetName, destName) in mapping) {
                val dest = java.io.File(rulesDir, destName)
                // 1) user-updated copy (route assets screen import/download)
                val userFile = java.io.File(SagerNet.application.externalAssets, assetName)
                if (userFile.isFile && userFile.length() > 0L &&
                    (!dest.isFile || dest.length() != userFile.length() ||
                        userFile.lastModified() > dest.lastModified())
                ) {
                    try {
                        userFile.inputStream().use { input ->
                            dest.outputStream().use { output -> input.copyTo(output) }
                        }
                        Log.i(TAG, "ensureRuleAssets: copied user $assetName -> rules/$destName")
                        continue
                    } catch (e: Throwable) {
                        Log.w(TAG, "ensureRuleAssets: failed to copy user $assetName: ${e.message}")
                    }
                }
                // 2) bundled APK copy (only when missing)
                if (dest.isFile && dest.length() > 0L) continue
                try {
                    assets.open("rules/$assetName").use { input ->
                        dest.outputStream().use { output -> input.copyTo(output) }
                    }
                    Log.i(TAG, "ensureRuleAssets: copied $assetName -> rules/$destName")
                } catch (e: Throwable) {
                    Log.w(TAG, "ensureRuleAssets: failed to copy $assetName: ${e.message}")
                }
            }
        } catch (e: Throwable) {
            Logs.w(e)
        }
    }

    private suspend fun writeGeoRulesPreset(country: String, directDns: List<String>): Boolean {
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
                appendLine("# Generated by OpenPPP2 Android from route rules.")
                appendLine("version: 1")
                appendLine("final: tunnel")
                appendLine()
                appendLine("direct_dns:")
                for (dns in dnsList) appendLine("  - $dns")
                appendLine()
                appendLine("rules:")
                // User-editable route rules (RuleEntity). outbound == -1 -> direct.
                val allRules = ProfileManager.getRules()
                val enabledRules = allRules.filter { it.enabled }
                if (allRules.isEmpty()) {
                    // No rules at all in the database — first launch fallback:
                    // private ranges (gated by the global LAN bypass switch)
                    // + the configured country.
                    if (DataStore.bypassLan) {
                        for (cidr in PRIVATE_LAN_CIDRS) appendLine("  - ip-cidr,$cidr,direct")
                    }
                    appendLine("  - domain-suffix,$country,direct")
                    appendLine("  - geosite,$country,direct")
                    appendLine("  - geoip,$country,direct")
                } else if (enabledRules.isEmpty()) {
                    // Rules exist but all are disabled — all traffic goes tunnel.
                    // final: tunnel handles the default route.
                } else {
                    for (rule in enabledRules) {
                        val action = if (rule.outbound == -1L) "direct" else "tunnel"
                        for (raw in rule.domains.split('\n')) {
                            val d = raw.trim()
                            if (d.isEmpty()) continue
                            appendLine("  - ${toGeoRule(d)},$action")
                        }
                        for (raw in rule.ip.split('\n')) {
                            val i = raw.trim()
                            if (i.isEmpty()) continue
                            // "geoip:private" has no entry in the stock v2fly
                            // GeoIP.dat and would fail the whole geo rule load;
                            // expand it to concrete private CIDRs instead.
                            if (i == "geoip:private") {
                                if (DataStore.bypassLan) {
                                    for (cidr in PRIVATE_LAN_CIDRS) appendLine("  - ip-cidr,$cidr,$action")
                                }
                                continue
                            }
                            appendLine("  - ${toGeoRule(i)},$action")
                        }
                    }
                }
            }
            rulesFile.writeText(content, Charsets.UTF_8)
            rulesFile.isFile && rulesFile.length() > 0L
        } catch (e: Throwable) {
            Logs.w(e)
            false
        }
    }

    /** Convert a SagerNet rule matcher (domain:xxx / geosite:cn / geoip:cn /
     * ip-cidr:...) into openppp2 geo-rules syntax. */
    private fun toGeoRule(matcher: String): String {
        val m = matcher.trim()
        return when {
            m.startsWith("domain-suffix:") -> "domain-suffix," + m.removePrefix("domain-suffix:")
            m.startsWith("domain:") -> "domain," + m.removePrefix("domain:")
            m.startsWith("geosite:") -> "geosite," + m.removePrefix("geosite:")
            m.startsWith("geoip:") -> "geoip," + m.removePrefix("geoip:")
            m.startsWith("ip-cidr:") -> "ip-cidr," + m.removePrefix("ip-cidr:")
            m.startsWith("ip:") -> "ip-cidr," + m.removePrefix("ip:")
            m.startsWith("domain-suffix,") || m.startsWith("domain,") ||
                m.startsWith("geosite,") || m.startsWith("geoip,") ||
                m.startsWith("ip-cidr,") -> m
            else -> "domain-suffix,$m"
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
