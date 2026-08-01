package supersocksr.ppp.android.c

import supersocksr.ppp.android.NativeTelemetryTransport
import supersocksr.ppp.android.PppVpnService

/**
 * JNI bridge class for libopenppp2.so
 * Package name and method signatures MUST match the native JNI exports exactly.
 */
class libopenppp2 {
    companion object {
        init {
            System.loadLibrary("openppp2")
        }

        /**
         * Called from native code to protect a socket from VPN routing.
         * This binds the socket to the underlying network so traffic doesn't loop.
         */
        @JvmStatic
        fun protect(sockfd: Int): Boolean {
            val service = PppVpnService.instance
            if (service == null) {
                android.util.Log.w("openppp2", "protect failed: service missing fd=$sockfd")
                return false
            }

            // VpnService.protect() alone is not enough on some OEM ROMs
            // (ColorOS/realme): the per-app VPN rule at priority 13000
            // (fwmark 0x0/0x20000) matches BEFORE the physical-network rule
            // (16000, fwmark 0x101ab/0x1ffff) as long as bit 17 (0x20000,
            // NETWORK_FORCE_NO_VPN) is clear.  VpnService.protect() only sets
            // the netId mark (0x101ab), so the socket is still routed into the
            // tunnel and the direct DNS query loops back into the TUN until it
            // times out.  Network.bindSocket() goes through the full system
            // binder path which also raises the NO_VPN bit, so the socket then
            // matches rule 16000 and actually leaves via the physical NIC.
            val ok = service.protect(sockfd)
            var bound = false
            if (ok) {
                var pfd: android.os.ParcelFileDescriptor? = null
                try {
                    // getNetworkForUid is a hidden API; getActiveNetwork()
                    // returns the device's default (physical, non-VPN) network
                    // for this process, which is the network we want the direct
                    // DNS upstream socket to leave through.
                    val cm = service.getSystemService(android.content.Context.CONNECTIVITY_SERVICE)
                            as? android.net.ConnectivityManager
                    val network = cm?.activeNetwork
                    if (network != null) {
                        pfd = android.os.ParcelFileDescriptor.adoptFd(sockfd)
                        network.bindSocket(pfd.fileDescriptor)
                        bound = true
                    }
                } catch (e: Throwable) {
                    android.util.Log.w("openppp2", "protect bindSocket failed fd=$sockfd: ${e.message}")
                } finally {
                    // adoptFd() took ownership of sockfd; detach it so the
                    // native side can keep close()ing it without fdsan trips.
                    try {
                        pfd?.detachFd()
                    } catch (e: Throwable) {
                        // best effort; native will handle a closed fd
                    }
                }
            }
            android.util.Log.i("openppp2", "protect fd=$sockfd result=$ok bound=$bound")
            return ok
        }

        @JvmStatic
        fun isProtectReady(): Boolean = PppVpnService.instance != null

        /**
         * Called from native code whenever the runtime publishes a snapshot.
         * Runs on whichever thread produced the transition, so delivery order
         * is not guaranteed; the service orders by the snapshot's own fields.
         */
        @JvmStatic
        fun runtime_snapshot(json: String) {
            PppVpnService.instance?.onRuntimeSnapshot(json)
        }

        /**
         * Called from native code after VPN run() starts successfully.
         */
        @JvmStatic
        fun start_exec(key: Int): Boolean {
            PppVpnService.instance?.onStarted(key)
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
         */
        @JvmStatic
        fun telemetryHttpPost(url: String, body: ByteArray): Boolean {
            return NativeTelemetryTransport.enqueuePost(url, body)
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
