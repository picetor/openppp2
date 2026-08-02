package supersocksr.ppp.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import java.io.File
import org.json.JSONObject

/**
 * Shell/ADB-debuggable VPN control interface.
 *
 * PppVpnService is exported=false (correct security default), so a shell
 * process cannot `am startservice` it directly. This receiver bridges the gap:
 * it is exported=true and forwards every received command to PppVpnService in
 * the `:vpn` process via startForegroundService/startService.
 *
 * Usage from `adb shell` (root not required):
 *
 *   # 1) Stage configuration files into the app's private files dir.
 *   #    `run-as` is required because /data/local/tmp is not readable by the
 *   #    app uid (u0_a350).
 *   adb shell "run-as supersocksr.ppp.android sh -c 'cat > files/debug_config.json'" < vpn_config_real.json
 *   adb shell "run-as supersocksr.ppp.android sh -c 'cat > files/debug_options.json'" < vpn_options.json
 *
 *   # 2) Connect (reads files/debug_config.json + files/debug_options.json).
 *   adb shell am broadcast -a supersocksr.ppp.android.DEBUG_CONTROL --es cmd connect
 *
 *   #    Override any option (staticMode, mux, vnet, routeMode, ...):
 *   adb shell am broadcast -a supersocksr.ppp.android.DEBUG_CONTROL \
 *       --es cmd connect --ez staticMode false --ez vnet true
 *
 *   # 3) Disconnect.
 *   adb shell am broadcast -a supersocksr.ppp.android.DEBUG_CONTROL --es cmd disconnect
 *
 *   # 4) Reconnect with the last active config (if any) plus overrides.
 *   adb shell am broadcast -a supersocksr.ppp.android.DEBUG_CONTROL \
 *       --es cmd reconnect --ez staticMode false
 *
 *   # 5) Status: writes files/debug_status.txt (readable via run-as).
 *   adb shell am broadcast -a supersocksr.ppp.android.DEBUG_CONTROL --es cmd status
 *   adb shell "run-as supersocksr.ppp.android cat files/debug_status.txt"
 *
 *   # 6) Dump last log tail into files/debug_log_tail.txt.
 *   adb shell am broadcast -a supersocksr.ppp.android.DEBUG_CONTROL --es cmd logtail
 */
class DebugControlReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "DebugControl"
        const val ACTION_DEBUG = "supersocksr.ppp.android.DEBUG_CONTROL"
        private const val EXTRA_CMD = "cmd"
        private const val EXTRA_CONFIG_JSON = "config"
        private const val EXTRA_OPTIONS_JSON = "options"
        private const val CONFIG_FILE = "debug_config.json"
        private const val OPTIONS_FILE = "debug_options.json"
        private const val STATUS_FILE = "debug_status.txt"
        private const val LOGTAIL_FILE = "debug_log_tail.txt"
        private const val MAX_LOGTAIL = 200

        // Prefixes recognized in the intent extra bag. Everything starting
        // with one of these is applied as a JSON override on top of the
        // staged options file. Example: --ez staticMode false
        private val OVERRIDE_KEYS = setOf(
            "staticMode", "mux", "vnet", "blockQuic", "proxyOnly",
            "routeMode", "muxAcceleration", "mtu", "mark",
            "dns1", "dns2", "dnsDirect1", "dnsDirect2",
            "tunIp", "tunMask", "tunPrefix", "route", "routePrefix",
            "extraArgs", "bypassIpList", "dnsRulesList",
        )
    }

    override fun onReceive(context: Context, intent: Intent) {
        val cmd = intent.getStringExtra(EXTRA_CMD) ?: run {
            Log.w(TAG, "missing $EXTRA_CMD extra")
            return
        }
        PppLog.write(context, "DebugControl cmd=$cmd")
        try {
            when (cmd) {
                "connect" -> handleConnect(context, intent)
                "reconnect" -> handleReconnect(context, intent)
                "disconnect" -> handleDisconnect(context)
                "status" -> handleStatus(context)
                "logtail" -> handleLogTail(context)
                else -> Log.w(TAG, "unknown cmd=$cmd")
            }
        } catch (e: Throwable) {
            Log.e(TAG, "$cmd failed", e)
            PppLog.write(context, "DebugControl $cmd failed: ${e.message}", e)
        }
    }

    private fun handleConnect(context: Context, intent: Intent) {
        val config = intent.getStringExtra(EXTRA_CONFIG_JSON)
            ?: readFile(context, CONFIG_FILE)
        if (config.isNullOrBlank()) {
            Log.e(TAG, "connect: no config (extra 'config' or files/$CONFIG_FILE)")
            writeStatus(context, "ERROR connect: no config available")
            return
        }

        var options = intent.getStringExtra(EXTRA_OPTIONS_JSON)
            ?: readFile(context, OPTIONS_FILE)
        if (options.isNullOrBlank()) options = "{}"
        options = applyOverrides(options, intent)

        // startForegroundService requires the app to be in foreground on
        // some API levels; fall back to startService when it is unavailable.
        val service = Intent(context, PppVpnService::class.java).apply {
            action = PppVpnService.ACTION_CONNECT
            putExtra(PppVpnService.EXTRA_CONFIG, config)
            putExtra(PppVpnService.EXTRA_VPN_OPTIONS, options)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(service)
        } else {
            context.startService(service)
        }
        writeStatus(context, "connect dispatched (options=${compact(options)})")
    }

    private fun handleReconnect(context: Context, intent: Intent) {
        // Reuse whatever the vpn process considers active (mirrored via the
        // pending fields), or fall back to the staged debug files.
        val service = Intent(context, PppVpnService::class.java).apply {
            action = PppVpnService.ACTION_CONNECT
        }
        val config = readFile(context, CONFIG_FILE)
        if (!config.isNullOrBlank()) {
            service.putExtra(PppVpnService.EXTRA_CONFIG, config)
            var options = readFile(context, OPTIONS_FILE) ?: "{}"
            options = applyOverrides(options, intent)
            service.putExtra(PppVpnService.EXTRA_VPN_OPTIONS, options)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(service)
        } else {
            context.startService(service)
        }
        writeStatus(context, "reconnect dispatched")
    }

    private fun handleDisconnect(context: Context) {
        val service = Intent(context, PppVpnService::class.java).apply {
            action = PppVpnService.ACTION_DISCONNECT
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startService(service)
        } else {
            context.startService(service)
        }
        writeStatus(context, "disconnect dispatched")
    }

    private fun handleStatus(context: Context) {
        val sb = StringBuilder()
        sb.appendLine("time=${System.currentTimeMillis()}")
        sb.appendLine("vpnAlive=${PppStateStore.isVpnAlive(context)}")
        sb.appendLine("heartbeatAgeMs=${PppStateStore.heartbeatAgeMs(context)}")
        sb.appendLine("state=${PppStateStore.get(context)}")
        val snap = PppStateStore.getRuntimeSnapshotIfAlive(context)
        sb.appendLine("snapshot=${snap ?: "(null)"}")
        val err = PppStateStore.getLastError(context)
        if (!err.isNullOrBlank()) sb.appendLine("lastError=$err")
        writeStatus(context, sb.toString())
    }

    private fun handleLogTail(context: Context) {
        val lines = PppLog.read(context).split("\n")
        val tail = lines.takeLast(MAX_LOGTAIL).joinToString("\n")
        val f = File(context.filesDir, LOGTAIL_FILE)
        f.writeText(tail, Charsets.UTF_8)
        Log.i(TAG, "wrote $LOGTAIL_FILE (${tail.length} chars)")
    }

    private fun applyOverrides(optionsJson: String, intent: Intent): String {
        val obj = try {
            JSONObject(optionsJson)
        } catch (e: Throwable) {
            JSONObject()
        }
        var changed = false
        for (key in OVERRIDE_KEYS) {
            if (intent.hasExtra(key)) {
                val v = intent.getExtras()?.get(key)
                if (v is Boolean) obj.put(key, v)
                else if (v is Int) obj.put(key, v)
                else if (v is Long) obj.put(key, v)
                else if (v is Double) obj.put(key, v)
                else if (v is String) obj.put(key, v)
                else if (v != null) obj.put(key, v.toString())
                changed = true
            }
        }
        if (!changed) return optionsJson
        return obj.toString()
    }

    private fun readFile(context: Context, name: String): String? {
        val f = File(context.filesDir, name)
        if (!f.isFile || f.length() == 0L) return null
        return f.readText(Charsets.UTF_8)
    }

    private fun writeStatus(context: Context, line: String) {
        try {
            val f = File(context.filesDir, STATUS_FILE)
            f.writeText("$line\n", Charsets.UTF_8)
        } catch (e: Throwable) {
            Log.e(TAG, "writeStatus failed", e)
        }
    }

    private fun compact(json: String): String {
        return if (json.length > 120) json.take(117) + "..." else json
    }
}
