/******************************************************************************
 *                                                                            *
 * openppp2 fork: OpenPPP2Instance replaces ProxyInstance (sing-box core).    *
 * It owns the openppp2 AppConfiguration JSON and the per-session VPN options *
 * JSON, and publishes traffic deltas sampled from the native runtime         *
 * snapshot (traffic.rx_bytes / traffic.tx_bytes).                            *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.bg.proto

import android.app.Service
import io.nekohasekai.sagernet.aidl.TrafficStats
import io.nekohasekai.sagernet.bg.AbstractInstance
import io.nekohasekai.sagernet.bg.GuardedProcessPool
import io.nekohasekai.sagernet.database.DataStore
import io.nekohasekai.sagernet.database.ProxyEntity
import io.nekohasekai.sagernet.fmt.internal.ConfigBean
import io.nekohasekai.sagernet.ktx.app
import org.json.JSONArray
import org.json.JSONObject
import supersocksr.ppp.android.c.libopenppp2

class OpenPPP2Config(
    val configJson: String,
    val vpnOptionsJson: String,
) {
    // sing-box config surface used by the legacy UI; openppp2 has no
    // route alerts / fakeDNS / uid dumper, so these stay inert.
    val alerts: List<Pair<Int, String>> = emptyList()
    val useFakeDNS = false
    val dumpUID = false
}

class OpenPPP2Instance(
    val profile: ProxyEntity,
    val service: Service,
) : AbstractInstance {

    lateinit var config: OpenPPP2Config
        private set

    var processes: GuardedProcessPool? = null

    // Traffic deltas (bytes since last sample), mirroring the old
    // ProxyInstance contract used by BaseService's bandwidth loop.
    @Volatile
    var uplinkProxy: Long = 0L
    @Volatile
    var downlinkProxy: Long = 0L

    private var lastTx = -1L
    private var lastRx = -1L

    override fun launch() {
        // The VPN lifecycle is driven by VpnService.startProcesses();
        // launch() is kept for the AbstractInstance contract.
    }

    fun init() {
        val bean = profile.requireBean()
        val configJson = if (bean is ConfigBean && bean.content.isNotBlank()) bean.content else "{}"
        config = OpenPPP2Config(configJson, buildVpnOptionsJson())
    }

    override fun close() {
        processes = null
    }

    fun persistStats() {
        // openppp2 keeps its own counters; no-op for the sing-box stats table.
    }

    fun uplinkDirect(): Long = 0L
    fun downlinkDirect(): Long = 0L

    /**
     * Sample the native runtime snapshot and produce the per-profile traffic
     * pair expected by BaseService.loop().
     */
    fun outboundStats(): Pair<TrafficStats, Map<Long, TrafficStats>> {
        var tx = 0L
        var rx = 0L
        try {
            val snapshot = libopenppp2.get_runtime_snapshot()
            if (!snapshot.isNullOrBlank()) {
                val root = JSONObject(snapshot)
                val traffic = root.optJSONObject("traffic")
                if (traffic != null) {
                    tx = traffic.optLong("tx_bytes", 0L)
                    rx = traffic.optLong("rx_bytes", 0L)
                }
            }
        } catch (_: Throwable) {
        }
        val now = System.currentTimeMillis()
        if (lastTx >= 0L && lastRx >= 0L) {
            uplinkProxy = (tx - lastTx).coerceAtLeast(0L)
            downlinkProxy = (rx - lastRx).coerceAtLeast(0L)
        }
        lastTx = tx
        lastRx = rx
        // Named args: (txRateProxy, rxRateProxy, txRateDirect, rxRateDirect, txTotal, rxTotal)
        return TrafficStats(txTotal = tx, rxTotal = rx) to emptyMap()
    }

    private fun buildVpnOptionsJson(): String {
        val options = JSONObject()
        val tunPrefix = DataStore.tunPrefix.coerceIn(16, 30)
        options.put("tunIp", DataStore.tunIp)
        options.put("tunMask", tunMaskFor(tunPrefix))
        options.put("tunPrefix", tunPrefix)
        options.put("route", "0.0.0.0")
        options.put("routePrefix", 0)
        options.put("dns1", DataStore.tunnelDns1)
        options.put("dns2", DataStore.tunnelDns2)
        options.put("dnsDirect1", DataStore.directDns1)
        options.put("dnsDirect2", DataStore.directDns2)
        options.put("mtu", DataStore.mtu)
        options.put("mark", 0)
        options.put("mux", DataStore.tunMux)
        options.put("muxAcceleration", DataStore.tunMuxAcceleration)
        options.put("vnet", DataStore.tunVnet)
        options.put("blockQuic", DataStore.blockQuic)
        options.put("staticMode", DataStore.tunStatic)
        options.put("proxyOnly", false)
        options.put("bypassIpList", "")
        options.put("dnsRulesList", "")
        options.put("routeMode", when (DataStore.bypassMode) {
            "ip" -> "basic"
            "geo" -> "geo"
            else -> ""
        })
        options.put("extraArgs", DataStore.extraArgs)

        // Per-app proxy policy.
        options.put("perAppProxyEnabled", DataStore.proxyApps)
        options.put("perAppProxyMode", if (DataStore.bypass) "deny" else "allow")
        val apps = JSONArray()
        for (pkg in DataStore.individual.split('\n').filter { it.isNotBlank() }) {
            apps.put(pkg)
        }
        options.put("perAppProxyApps", apps)

        // GEO routing preset: enabled only in geo bypass mode. The preset
        // YAML (rules/geo-rules.yaml) is materialized by VpnService.
        val geoRules = JSONObject()
        geoRules.put("enabled", DataStore.bypassMode == "geo")
        geoRules.put("country", "cn")
        options.put("geoRules", geoRules)

        return options.toString()
    }

    /** Convert a subnet prefix (16-30) to a dotted IPv4 netmask. */
    private fun tunMaskFor(prefix: Int): String {
        val p = prefix.coerceIn(0, 32)
        val mask = if (p == 0) 0L else (0xFFFFFFFFL shl (32 - p)) and 0xFFFFFFFFL
        return "%d.%d.%d.%d".format(
            (mask shr 24) and 0xFF, (mask shr 16) and 0xFF,
            (mask shr 8) and 0xFF, mask and 0xFF
        )
    }
}
