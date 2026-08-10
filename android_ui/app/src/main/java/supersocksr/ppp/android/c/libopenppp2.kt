package supersocksr.ppp.android.c

import io.nekohasekai.sagernet.bg.VpnService

/**
 * JNI bridge class for libopenppp2.so
 * Package name and method signatures MUST match the native JNI exports exactly.
 * The native C++ exports are `Java_supersocksr_ppp_android_c_libopenppp2_*`,
 * so this class MUST live in package `supersocksr.ppp.android.c` with the
 * exact name `libopenppp2`.
 */
class libopenppp2 {
    companion object {
        init {
            System.loadLibrary("openppp2")
        }

        /**
         * Called from native code to protect a socket from VPN routing.
         * This binds the socket to the underlying network so traffic doesn't loop.
         *
         * ColorOS (realme/OPPO/OnePlus) installs `ip rule 13000 fwmark
         * 0xc022a/0xcffff lookup <vpn-table>` which hijacks the 0xc022a fwmark
         * that VpnService.protect() assigns, sending the "protected" socket
         * straight back into the tunnel. Direct DNS queries then exit from the
         * overseas tunnel egress and 223.5.5.5 returns overseas CDN nodes,
         * breaking DNS-based splitting. When the hijack rule is present AND our
         * uid still routes via the physical NIC (ColorOS excludes the VPN app
         * uid from its 13000 uidrange rules, falling through to `ip rule 31000
         * fwmark 0x0/0xffff iif lo lookup <wlan0>`), skip protect() so the
         * socket stays unmarked and exits over the physical network.
         *
         * MUST stay IO/log-free: native code calls this from the boost::context
         * fiber stack, and on Android 12+ ART CheckJNI aborts with an invalid
         * JNI transition frame whenever a fiber-triggered call touches a
         * jstring (android.util.Log is the first victim). The native bridge
         * (OpenPPP2VpnProtectBridge) already decides ColorOS devices natively;
         * this method is the non-ColorOS fallback.
         */
        @JvmStatic
        fun protect(sockfd: Int): Boolean {
            val service = VpnService.instance
            if (service == null) {
                return false
            }
            if (protectHijacked) {
                return true
            }
            return service.protect(sockfd)
        }

        /**
         * True when ColorOS ip-rule 13000 hijacks VpnService.protect()'s fwmark
         * (0xc022a) back into the VPN table, and our uid can still reach the
         * physical network unmarked.
         *
         * Detection keys on Build.MANUFACTURER/BRAND (realme/OPPO/OnePlus run
         * ColorOS, which installs the hijack rule). The `ip rule` cross-check
         * is diagnostic only and lives in ensureProtectHijackedChecked(),
         * because Runtime.exec()/ProcessBuilder from a JNI-attached native
         * thread (the VPN thread calling protect()) aborts ART with an invalid
         * JNI transition frame. This lazy must therefore stay IO/log-free so
         * it is safe to evaluate from the fiber stack.
         */
        private val protectHijacked: Boolean by lazy {
            val manufacturer = (android.os.Build.MANUFACTURER ?: "").lowercase()
            val brand = (android.os.Build.BRAND ?: "").lowercase()
            manufacturer.contains("realme") || manufacturer.contains("oppo") ||
                manufacturer.contains("oneplus") || manufacturer.contains("oplus") ||
                brand.contains("realme") || brand.contains("oppo") ||
                brand.contains("oneplus") || brand.contains("oplus")
        }

        /**
         * Pre-warms protectHijacked and runs the optional `ip rule`
         * cross-check. MUST be called from a normal JVM thread (VpnService
         * startup) before the native VPN thread starts: the native thread
         * cannot spawn processes inside protect() without aborting ART.
         */
        @JvmStatic
        fun ensureProtectHijackedChecked() {
            val hijacked = protectHijacked
            if (!hijacked) {
                return
            }
            try {
                val rule = Runtime.getRuntime()
                    .exec(arrayOf("ip", "rule"))
                    .inputStream.bufferedReader().readText()
                android.util.Log.i(
                    "openppp2",
                    "protect hijack: ip-rule confirm=" + rule.contains("fwmark 0xc022a/0xcffff")
                )
            } catch (e: Throwable) {
                android.util.Log.i("openppp2", "protect hijack: ip not readable, skip protect()")
            }
        }

        @JvmStatic
        fun isProtectReady(): Boolean = VpnService.instance != null

        /**
         * Called from native code whenever the runtime publishes a snapshot.
         */
        @JvmStatic
        fun runtime_snapshot(json: String) {
            VpnService.instance?.onRuntimeSnapshot(json)
        }

        /**
         * Called from native code after VPN run() starts successfully.
         */
        @JvmStatic
        fun start_exec(key: Int): Boolean {
            VpnService.instance?.onStarted(key)
            return true
        }

        /**
         * Called from native code for post execution callbacks.
         */
        @JvmStatic
        fun post_exec(sequence: Int): Boolean {
            return true
        }

        /**
         * Called from native C++ telemetry OTLP exporter (bounded queue).
         * Telemetry is disabled in this fork; always report failure so the
         * native side drops the payload immediately.
         */
        @JvmStatic
        fun telemetryHttpPost(url: String, body: ByteArray): Boolean {
            return false
        }

        // ========== Native methods ==========

        @JvmStatic
        external fun installNativeTelemetryHttpPost()

        @JvmStatic
        external fun setNativeTelemetryResourceAttribute(key: String, value: String)

        @JvmStatic
        external fun clearNativeTelemetryResourceAttributes()

        @JvmStatic
        external fun set_protect_enabled(enabled: Boolean): Boolean

        @JvmStatic
        external fun protect_socket_fd(fd: Int): Boolean

        @JvmStatic
        external fun get_default_ciphersuites(): String?

        @JvmStatic
        external fun set_root_path(path: String): Boolean

        @JvmStatic
        external fun set_app_configuration(configurations: String): Int

        @JvmStatic
        external fun get_app_configuration(): String?

        @JvmStatic
        external fun set_network_interface(
            tun: Int,
            mux: Int,
            vnet: Boolean,
            block_quic: Boolean,
            static_mode: Boolean,
            ip: String,
            mask: String,
            ipv6: String
        ): Int

        @JvmStatic
        external fun set_mux_acceleration(value: Int): Int

        @JvmStatic
        external fun get_network_interface(): String?

        @JvmStatic
        external fun set_bypass_ip_list(iplist: String): Boolean

        @JvmStatic
        external fun set_dns_rules_list(rules: String): Boolean

        @JvmStatic
        external fun set_geo_rules(rules: String, geosite: String, geoip: String): Boolean

        @JvmStatic
        external fun set_dns_bcl(turbo: Boolean, ttl: Int, dns: String): Boolean

        @JvmStatic
        external fun set_log_level(level: Int): Int

        @JvmStatic
        external fun get_bypass_ip_list(): String?

        @JvmStatic
        external fun run(key: Int): Int

        @JvmStatic
        external fun stop(): Int

        @JvmStatic
        external fun clear_configure()

        @JvmStatic
        external fun get_link_state(): Int

        @JvmStatic
        external fun get_runtime_snapshot(): String?

        @JvmStatic
        external fun get_aggligator_state(): Int

        @JvmStatic
        external fun get_duration_time(): Long

        @JvmStatic
        external fun get_last_error_code(): Int

        @JvmStatic
        external fun get_last_error_text(): String?

        @JvmStatic
        external fun get_ethernet_information(default_: Boolean): String?

        @JvmStatic
        external fun get_http_proxy_address_endpoint(): String?

        @JvmStatic
        external fun get_socks_proxy_address_endpoint(): String?

        @JvmStatic
        external fun link_of(url: String): String?

        @JvmStatic
        external fun if_subnet(ip1: String, ip2: String, mask: String): Boolean

        @JvmStatic
        external fun netmask_to_prefix(address: ByteArray): Int

        @JvmStatic
        external fun prefix_to_netmask(v4_or_v6: Boolean, prefix: Int): String?

        @JvmStatic
        external fun ip_address_string_is_invalid(address: String): Boolean

        @JvmStatic
        external fun bytes_to_address_string(address: ByteArray): String?

        @JvmStatic
        external fun string_to_address_bytes(address: String): ByteArray?

        @JvmStatic
        external fun socket_get_socket_type(fd: Int): Int

        @JvmStatic
        external fun post(sequence: Int): Boolean

        @JvmStatic
        external fun set_default_flash_type_of_service(flash_mode: Boolean): Boolean

        @JvmStatic
        external fun is_default_flash_type_of_service(): Int
    }
}
