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
    // One-shot guard for the IPv6 lease probe (see rememberIpv6Lease).
    private var ipv6LeaseQueryFailed = false

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
            val stopResult = libopenppp2.stop()
            if (stopResult != 0) Logs.w("libopenppp2.stop returned $stopResult (timeout or not running)")
        } catch (e: Throwable) {
            Logs.w(e)
        }
        stopLinkStatePoller()
        LogCollector.stop()
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
                // The server answers with its IPv6 lease right after the
                // handshake; remember it so the next tunnel is built with that
                // address instead of the leak-block ULA.
                rememberIpv6Lease()
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
            }
            3 -> data.changeState(BaseService.State.Stopping)
            6 -> {
                if (data.state != BaseService.State.Stopped) {
                    data.changeState(BaseService.State.Stopped)
                }
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
        // IPv6 policy (VpnService routing is per address family and applies to
        // every app that is not excluded, so this is an all-or-nothing choice):
        //   block  : claim ::/0 and drop.  Leak-safe, but the phone's native
        //            IPv6 is unusable for every app, including bypassed ones.
        //   direct : do not claim ::/0.  Bypassed traffic keeps native IPv6;
        //            the core still strips AAAA for destinations the policy
        //            resolves over the tunnel, which has no IPv6 data plane.
        //   auto   : block only while the tunnel can really carry IPv6.  The
        //            Android IPv6 input path is still a stub, so auto == direct
        //            until that data plane exists.
        val ipv6Mode = options.optString("ipv6Mode", "auto")
        // auto stays on the leak-safe side until the core can report a
        // server-provided IPv6 data plane; direct is the explicit opt-out.
        val blockIpv6 = ipv6Mode != "direct"

        // The server hands this tunnel an IPv6 lease (NAT66/GUA).  Android's
        // kernel-side TUN address is fixed by VpnService.Builder, so without
        // this the core has to rewrite the source of every IPv6 packet between
        // the leak-block ULA and that lease.  Reusing the lease the server
        // assigned last time makes the TUN own the very address the server
        // NAT66s -- the Windows client does the same with ApplyClientAddress --
        // and the rewrite becomes a no-op.  A stale entry is harmless: the core
        // still translates whenever the server assigns something else.
        val tunIpv6Address = cachedIpv6Lease() ?: IPV6_BLOCK_ADDRESS

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
        Log.i(TAG, "vpn options tunIp=$vpnIp/$vpnPrefix mtu=$mtu mux=$effectiveMux proxyOnly=$proxyOnly routeMode=$routeMode ipv6=$ipv6Mode/$blockIpv6")

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

        if (!proxyOnly && blockIpv6) {
            builder.addAddress(tunIpv6Address, 128)
            builder.addRoute("::", 0)
            builder.allowFamily(OsConstants.AF_INET6)
            // Domestic IPv6 must leave through the local link, not the tunnel.
            // Android hands this client raw IPv6 packets and there is no
            // unprivileged way to inject them back onto the physical network
            // (the IPv4 direct path terminates TCP in lwIP and dials a
            // protected socket; the IPv6 path has no accept hook, so
            // OnIPv6PacketInput can only Nat() it to the server, which made a
            // domestic IPv6 site see the server's address).  Excluding the
            // domestic prefixes lets the kernel route them natively.  Only when
            // a bypass policy is active: a global-proxy profile must keep every
            // destination, domestic included, inside the tunnel.
            if (routeMode.isNotEmpty()) {
                applyDirectIpv6Exclusions(builder)
            }
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
            // Pre-warm the ColorOS hijack decision on this JVM thread before
            // the native VPN thread starts. The native thread must never run
            // the `ip rule` exec inside protect() - ART aborts with an invalid
            // JNI transition frame when a JNI-attached thread spawns a process.
            libopenppp2.ensureProtectHijackedChecked()
        } catch (e: Throwable) {
            Logs.w(e)
        }
        try {
            libopenppp2.set_protect_enabled(true)
        } catch (e: Throwable) {
            Logs.w(e)
        }

        // detachFd() transfers ownership of the fd to native code.
        val tunFd = vpnInterface!!.detachFd()
        vpnInterface = null

        val niResult = libopenppp2.set_network_interface(
            tunFd, effectiveMux, vnet, blockQuic, staticMode, vpnIp, vpnMask, tunIpv6Address
        )
        if (niResult != 0) {
            throw IllegalStateException(
                "set_network_interface failed: $niResult, error: ${libopenppp2.get_last_error_text()}"
            )
        }

        // Hand the IPv6 policy to the core.  A VpnService TUN cannot be
        // reconfigured from native code, so without this the client sees its
        // own route install fail and concludes it has no IPv6 data plane,
        // stripping every AAAA answer even when the server assigned one.
        try {
            libopenppp2.set_ipv6_mode(if (blockIpv6) 0 else 1)
        } catch (e: Throwable) {
            Logs.w(e)
        }

        if (effectiveMuxAcceleration > 0) {
            try {
                libopenppp2.set_mux_acceleration(effectiveMuxAcceleration)
            } catch (_: UnsatisfiedLinkError) {
            }
        }

        // Start VPN in background thread (run() is blocking). 8 MiB stack for
        // boost::asio deep handler chains.
        isRunning = true
        startLinkStatePoller()
        LogCollector.start(this)
        vpnThread = Thread(null, Runnable {
            var abnormalExit = false
            var exitMessage: String? = null
            try {
                Log.i(TAG, "vpnThread started, calling run(0)")
                val result = libopenppp2.run(0)
                Log.i(TAG, "libopenppp2.run returned=$result")
                if (result != 0) {
                    abnormalExit = true
                    exitMessage = libopenppp2.get_last_error_text()
                    Logs.w("libopenppp2 last error=$exitMessage")
                }
            } catch (e: Throwable) {
                abnormalExit = true
                exitMessage = e.message
                Logs.w(e)
            } finally {
                isRunning = false
                stopLinkStatePoller()
                LogCollector.stop()
                releaseWakeLock()
                try {
                    vpnInterface?.close()
                } catch (_: Throwable) {
                }
                vpnInterface = null
                if (data.state != BaseService.State.Stopped && data.state != BaseService.State.Stopping) {
                    // The VPN session ended without a user-initiated stop.
                    // Surface the reason and shut the foreground service down
                    // instead of silently leaving it in a ghost Connected state.
                    val message = exitMessage?.takeIf { it.isNotBlank() }
                        ?: if (abnormalExit) "VPN 意外退出" else "VPN 连接已断开"
                    data.changeState(BaseService.State.Stopped, message)
                    stopSelf()
                }
            }
        }, "openppp2-vpn-thread", VPN_THREAD_STACK).also { it.start() }
    }

    override fun onRevoke() = stopRunner()

    // ------------------------------------------------------------------
    // No root, no ip-rule surgery.
    //
    // This service used to spawn su(1) to delete netd's per-network fwmark
    // rules and to pin the vnet gateway loopback. A VPN app must not ask for
    // root: the same effects are either netd's job (per-app VPN, allowBypass)
    // or a plain bug in our own routing. The corresponding helpers were
    // removed; the client now runs with the routes VpnService gives it.
    // ------------------------------------------------------------------


    /**
     * The IPv6 lease the primary server assigned in an earlier session, if it
     * still looks like a usable address.  Getting it onto the TUN makes the
     * kernel address match the server's NAT66 lease; when it does not match
     * (first run, new server, changed assignment) the core keeps translating
     * between the leak-block ULA and the lease, so a wrong value never breaks
     * connectivity.
     */
    private fun cachedIpv6Lease(): String? {
        val value = try {
            DataStore.tunIpv6Lease.trim()
        } catch (_: Throwable) {
            return null
        }
        if (!value.contains(':')) return null
        return try {
            val address = java.net.InetAddress.getByName(value)
            if (address is java.net.Inet6Address && !address.isAnyLocalAddress) value else null
        } catch (_: Throwable) {
            null
        }
    }

    /** Persist the lease the server just announced, so the next tunnel uses it. */
    private fun rememberIpv6Lease() {
        val lease = try {
            libopenppp2.get_ipv6_lease()
        } catch (t: Throwable) {
            if (!ipv6LeaseQueryFailed) {
                ipv6LeaseQueryFailed = true
                Log.w(TAG, "ipv6 lease query failed", t)
            }
            null
        }
        if (lease.isNullOrBlank() || !lease.contains(':')) return
        if (lease == cachedIpv6Lease()) return
        try {
            DataStore.tunIpv6Lease = lease
            Log.i(TAG, "ipv6 lease remembered: $lease")
        } catch (e: Throwable) {
            Logs.w(e)
        }
    }

    /**
     * Excludes the domestic IPv6 prefixes (files/rules/ipv6.txt) from the VPN so
     * the kernel sends them over the physical network, while everything else
     * keeps using the tunnel.  Requires API 33 (VpnService.Builder
     * .addExcludeRoute); on older releases domestic IPv6 stays in the tunnel.
     */
    private fun applyDirectIpv6Exclusions(builder: BaseVpnService.Builder) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Log.i(TAG, "ipv6 direct exclusions need Android 13 (API 33); domestic IPv6 stays in the tunnel")
            return
        }
        val file = java.io.File(filesDir, "rules/ipv6.txt")
        if (!file.isFile || file.length() == 0L) {
            Log.i(TAG, "ipv6 direct exclusions skipped: ${file.path} is missing")
            return
        }
        var applied = 0
        var skipped = 0
        try {
            file.forEachLine { raw ->
                val line = raw.trim()
                if (line.isEmpty() || line.startsWith("#")) return@forEachLine
                val slash = line.indexOf('/')
                if (slash <= 0) {
                    skipped++
                    return@forEachLine
                }
                val host = line.substring(0, slash)
                val prefix = line.substring(slash + 1).toIntOrNull()
                if (prefix == null || prefix !in 0..128) {
                    skipped++
                    return@forEachLine
                }
                try {
                    val address = java.net.InetAddress.getByName(host)
                    if (address !is java.net.Inet6Address) {
                        skipped++
                        return@forEachLine
                    }
                    builder.excludeRoute(android.net.IpPrefix(address, prefix))
                    applied++
                } catch (_: Throwable) {
                    skipped++
                }
            }
        } catch (e: Throwable) {
            Logs.w(e)
        }
        Log.i(TAG, "ipv6 direct exclusions applied: $applied prefixes (skipped $skipped)")
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
