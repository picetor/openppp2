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
        options.put("tunIp", "10.0.0.2")
        options.put("tunMask", "255.255.255.0")
        options.put("tunPrefix", 24)
        options.put("route", "0.0.0.0")
        options.put("routePrefix", 0)
        options.put("dns1", "8.8.8.8")
        options.put("dns2", "8.8.4.4")
        options.put("dnsDirect1", "")
        options.put("dnsDirect2", "")
        options.put("mtu", DataStore.mtu)
        options.put("mark", 0)
        options.put("mux", 0)
        options.put("vnet", false)
        options.put("blockQuic", false)
        options.put("staticMode", false)
        options.put("proxyOnly", false)
        options.put("bypassIpList", "")
        options.put("dnsRulesList", "")
        options.put("routeMode", "")
        options.put("extraArgs", "")

        // Per-app proxy policy.
        options.put("perAppProxyEnabled", DataStore.proxyApps)
        options.put("perAppProxyMode", if (DataStore.bypass) "deny" else "allow")
        val apps = JSONArray()
        for (pkg in DataStore.individual.split('\n').filter { it.isNotBlank() }) {
            apps.put(pkg)
        }
        options.put("perAppProxyApps", apps)

        // GEO routing preset: single tunnel final, user-configured country
        // (if any) direct. The preset YAML is materialized by VpnService.
        val geoRules = JSONObject()
        geoRules.put("enabled", false)
        geoRules.put("country", "cn")
        options.put("geoRules", geoRules)

        return options.toString()
    }
}
