#pragma once

#include <ppp/stdafx.h>
#include <ppp/threading/BufferswapAllocator.h>

#include <json/json.h>

/**
 * @file AppConfiguration.h
 * @brief Application configuration model and serialization APIs.
 */

namespace ppp {
    namespace configurations {
        /**
         * @brief Stores runtime and networking configuration for the application.
         */
        class AppConfiguration final {
        public:
            /**
             * @brief Port mapping rule configuration.
             *
             * Describes one static port-forwarding entry that maps a local
             * service port to a remote port exposed through the virtual Ethernet
             * tunnel.  Both TCP and UDP mappings are supported.
             */
            struct MappingConfiguration final {
                bool                                                        protocol_tcp_or_udp; ///< True selects TCP mapping; false selects UDP mapping.
                ppp::string                                                 local_ip;            ///< Local bind address for the forwarded service (empty = any).
                int                                                         local_port;          ///< Local port of the service being forwarded.
                ppp::string                                                 remote_ip;           ///< Remote peer address to reach through the tunnel (may be empty).
                int                                                         remote_port;         ///< Remote port exposed on the tunnel peer side.
            };

            /**
             * @brief Route source configuration for client route imports.
             *
             * Specifies an external route file whose prefixes the
             * client should install after establishing the tunnel.
             */
            struct RouteConfiguration final {
#if defined(_LINUX)
                ppp::string                                                 nic;   ///< Linux NIC name used as the route output interface.
#endif
                uint32_t                                                    ngw;   ///< Next-hop gateway IPv4 address in host byte order; 0 means use tunnel default.
                ppp::string                                                 path;  ///< Path to a static route file listing CIDR prefixes to import.
            };

            /**
             * @brief Canonical client routing policy model.
             *
             * The model is present under @c client.routing when the canonical
             * object was supplied.  It carries only IP and DNS policy sources:
             * each @c ip and @c dns item may identify a file or contain inline
             * text.  The independent @c client.proxy-only flag controls runtime
             * mode.  During the compatibility period, canonical routes are
             * mirrored to the legacy @c client.routes, while legacy-only input
             * is projected into this model for serialization.
             */
            struct ClientRoutingConfiguration final {
                bool                                                        configured;  ///< True when client.routing was supplied as a JSON object.
                ppp::vector<ppp::string>                                    bypass;      ///< IP bypass source files or inline text.
                ppp::vector<RouteConfiguration>                             routes;      ///< Canonical IP route sources.
                ppp::vector<ppp::string>                                    dns_rules;   ///< DNS rule source files or inline text.
            };

            /**
             * @brief IPv6 address assignment mode for server-side data plane.
             *
             * Controls which IPv6 provisioning strategy the server uses when
             * allocating an IPv6 prefix or address for a connected client.
             */
            enum IPv6Mode {
                IPv6Mode_None  = 0,   ///< IPv6 provisioning is disabled; clients receive IPv4 only.
                IPv6Mode_Nat66 = 1,   ///< NAT66 is used to translate a ULA prefix to a global prefix.
                IPv6Mode_Gua   = 2,   ///< Globally Unique Address (GUA) is delegated directly to the client.
            };

        public:
            int                                                             concurrent;   ///< Number of concurrent IO worker threads; typically set to CPU core count.
            int                                                             cdn[2];       ///< CDN acceleration port pair [plain, tls]; 0 disables the respective CDN path.
            struct {
                ppp::string                                                 public_;      ///< Public-facing IPv4/IPv6 address advertised to remote peers.
                ppp::string                                                 interface_;   ///< Local network interface address used for binding sockets.
            }                                                               ip;           ///< IP address binding configuration.
            struct {
                struct {
                    int                                                     timeout;      ///< Idle timeout in seconds before a UDP session is torn down.
                }                                                           inactive;
                struct {
                    int                                                     timeout;      ///< DNS query timeout in seconds.
                    int                                                     ttl;          ///< DNS response TTL in seconds used for local cache entries.
                    bool                                                    turbo;        ///< Enable turbo (parallel/aggressive) DNS query mode.
                    bool                                                    cache;        ///< Enable local DNS response caching.
                    bool                                                    prefer_ipv4 = false; ///< Strip AAAA records from DNS responses when A records exist.
                    ppp::string                                             redirect;     ///< Upstream DNS server address to redirect queries to; empty = system default.
                }                                                           dns;
                struct {
                    int                                                     port;         ///< UDP listen port; 0 lets the OS choose an ephemeral port.
                }                                                           listen;
                struct {
                    int                                                     keep_alived[2]; ///< Keep-alive interval range [min, max] in seconds for static UDP mappings.
                    bool                                                    dns;            ///< Forward DNS traffic through the static channel.
                    bool                                                    quic;           ///< Enable QUIC acceleration for static UDP sessions.
                    bool                                                    icmp;           ///< Allow ICMP echo through the static channel.
                    int                                                     aggligator;     ///< Aggligator aggregation factor; 0 disables UDP link aggregation.
                    ppp::unordered_set<ppp::string>                         servers;        ///< Set of static server addresses used as tunnel uplink endpoints.
                }                                                           static_;
                int                                                         cwnd;           ///< UDP congestion window size in packets; 0 means system default.
                int                                                         rwnd;           ///< UDP receive window size in packets; 0 means system default.
            }                                                               udp;            ///< UDP protocol and session parameters.
            struct {
                struct {
                    int                                                     timeout;        ///< Idle timeout in seconds before an established TCP session is closed.
                }                                                           inactive;
                struct {
                    int                                                     timeout;        ///< TCP connect handshake timeout in seconds.
                    int                                                     nexcept;        ///< Maximum allowed consecutive connect exceptions before circuit-break.
                }                                                           connect;
                struct {
                    int                                                     port;           ///< TCP listen port; 0 lets the OS choose an ephemeral port.
                }                                                           listen;
                bool                                                        turbo;          ///< Enable TCP turbo mode (Nagle disabled, immediate flush).
                int                                                         backlog;        ///< Listen backlog depth for the TCP acceptor socket.
                int                                                         cwnd;           ///< TCP congestion window size override; 0 means kernel default.
                int                                                         rwnd;           ///< TCP receive window size override; 0 means kernel default.
                bool                                                        fast_open;      ///< Enable TCP Fast Open (TFO) for reduced handshake latency.
            }                                                               tcp;            ///< TCP protocol and session parameters.
            struct {
                struct {
                    int                                                     timeout;        ///< Idle timeout in seconds for a MUX sub-channel.
                }                                                           inactive;
                struct {
                    int                                                     timeout;        ///< MUX connect handshake timeout in seconds.
                }                                                           connect;
                ppp::string                                                 mode;           ///< MUX transmit scheduler mode: compat, flow, balance, or stripe.
                int                                                         congestions;    ///< MUX congestion control level; higher values reduce burst aggressiveness.
                int                                                         keep_alived[2]; ///< MUX keep-alive interval range [min, max] in seconds.
                bool                                                        turbo;          ///< Enables the experimental dynamic carrier pool for flow mode; default false.
                struct {
                    struct {
                        int                                                 bytes;          ///< Per-connection reorder buffer byte cap (flow v2); strictly > 0.
                        int                                                 timeout;        ///< Per-connection gap wait timeout in milliseconds (flow v2); strictly > 0.
                    }                                                       reorder;
                    struct {
                        bool                                                enabled;            ///< Master switch for per-flow NACK retransmit (M4); negotiated, requires flow-v2 on both ends.
                        int                                                 cache_bytes;        ///< Send-side retransmit cache byte cap per connection; clamped >= reorder.bytes x 2.
                        int                                                 max_frames;         ///< Send-side retransmit cache frame cap per connection.
                        int                                                 ack_every_frames;   ///< Receiver emits one aggregated ACK every N delivered frames.
                        int                                                 ack_delay_ms;       ///< Receiver emits an aggregated ACK at most every N ms.
                        int                                                 nack_max_retries;   ///< NACK resend retries before falling back to a clean mux rebuild.
                        int                                                 nack_backoff_ms;    ///< Base NACK resend backoff; doubled per retry up to 1600 ms.
                    }                                                       retransmit;
                }                                                           flow;           ///< Per-flow (flow v2) receiver ordering parameters.
                struct {
                    struct {
                        int                                                 max;            ///< Data tx-queue high-water depth; acceleration read-pump throttles at/above it (D11). > 0.
                        int                                                 stall;          ///< Milliseconds the data tx-queue may stay backlogged before the session is rebuilt (D11 watchdog). > 0.
                    }                                                       queue;
                }                                                           tx;             ///< Transmit-side flow-control / backpressure parameters.
                struct {
                    ppp::string                                             key;            ///< Shared debug secret (`--debug-key`); empty disables remote mux-mode control.
                    ppp::string                                             set_mode;       ///< Transient `--mux-mode-set` request; pushes a mode change to the peer once at startup.
                }                                                           debug;          ///< Debug-only remote control of the peer's scheduler mode.
            }                                                               mux;            ///< Multiplexed connection channel parameters.
            struct {
                struct {
                    int                                                     ws;             ///< Plain WebSocket listen port (ws://); 0 disables plain WS.
                    int                                                     wss;            ///< Secure WebSocket listen port (wss://); 0 disables WSS.
                }                                                           listen;
                struct {
                    std::string                                             certificate_file;          ///< Path to PEM-encoded TLS certificate file.
                    std::string                                             certificate_key_file;      ///< Path to PEM-encoded private key file matching the certificate.
                    std::string                                             certificate_chain_file;    ///< Path to PEM-encoded intermediate CA chain file.
                    std::string                                             certificate_key_password;  ///< Passphrase protecting the private key; empty if key is unencrypted.
                    std::string                                             ciphersuites;              ///< Colon-separated TLS 1.3 cipher suite list; empty = OpenSSL default.
                    bool                                                    verify_peer;               ///< Require and verify client certificate when true.
                }                                                           ssl;
                ppp::string                                                 host;           ///< Expected HTTP `Host` header value for WebSocket upgrade validation.
                ppp::string                                                 path;           ///< WebSocket upgrade path (e.g. "/ws"); must begin with '/'.
                struct {
                    std::string                                             error;          ///< Body returned for non-WebSocket HTTP requests (plain text or HTML).
                    ppp::map<ppp::string, ppp::string>                      request;        ///< Extra HTTP headers injected into the client upgrade request.
                    ppp::map<ppp::string, ppp::string>                      response;       ///< Extra HTTP headers injected into the server upgrade response.
                }                                                           http;
            }                                                               websocket;      ///< WebSocket transport (WS/WSS) parameters.
            struct {
                int                                                         kf;             ///< Key-frame interval (packets between re-key events).
                int                                                         kh;             ///< Key hash iterations for KDF strengthening.
                int                                                         kl;             ///< Key length in bits (e.g. 128, 256).
                int                                                         kx;             ///< Key exchange algorithm selector.
                int                                                         sb;             ///< Shuffle block size used by `shuffle_data` mode.
                ppp::string                                                 protocol;       ///< Protocol-layer cipher algorithm name (e.g. "aes-256-cfb").
                ppp::string                                                 protocol_key;   ///< Passphrase used to derive the protocol-layer cipher key.
                ppp::string                                                 transport;      ///< Transport-layer cipher algorithm name (e.g. "aes-128-cfb").
                ppp::string                                                 transport_key;  ///< Passphrase used to derive the transport-layer cipher key.
                bool                                                        masked;         ///< Apply a masking XOR pass over packet headers when true.
                bool                                                        plaintext;      ///< Transmit data in plaintext (no encryption) when true; for debugging only.
                bool                                                        delta_encode;   ///< Apply delta encoding to payload bytes before encryption.
                bool                                                        shuffle_data;   ///< Randomly reorder payload blocks within each packet when true.
            }                                                               key;            ///< Cryptographic key and cipher configuration.
            struct {
                int64_t                                                     size;           ///< Virtual memory file size in bytes; 0 disables vmem backing.
                ppp::string                                                 path;           ///< File path used for memory-mapped virtual memory backing store.
            }                                                               vmem;           ///< Virtual memory (disk-backed buffer) configuration.
            struct {
                int                                                         node;           ///< Server node identifier used for clustering and session routing.
                ppp::string                                                 log;            ///< Path to the structured event log file; empty disables logging.
                bool                                                        subnet;         ///< Advertise client subnets between sessions when true.
                bool                                                        mapping;        ///< Enable server-side static port mapping support when true.
                ppp::string                                                 backend;        ///< URL or address of the authentication/management backend.
                ppp::string                                                 backend_key;    ///< HMAC key used to sign backend API requests.
                struct {
                    bool                                                    enabled;        ///< Enables direct policy distribution from OpenPPP2 Management.
                    ppp::string                                             endpoint;       ///< Management panel base HTTP/HTTPS URL.
                    ppp::string                                             node_id;        ///< Node identifier configured in the management panel.
                    ppp::string                                             communication_key; ///< Fixed key shared by the panel and all nodes.
                    ppp::string                                             token;          ///< Node bearer token (prefer token_file in production).
                    ppp::string                                             token_file;     ///< File containing the node bearer token.
                    ppp::string                                             cache_file;     ///< Last-known-good policy cache.
                    ppp::string                                             local_blacklist_file; ///< Local emergency GUID blacklist.
                    ppp::string                                             cacert_file;    ///< CA bundle used to verify the management endpoint.
                    int                                                     pull_interval;  ///< Policy refresh interval in seconds.
                    bool                                                    report_online;  ///< Reports GUID presence and traffic to the panel.
                }                                                           management;
                struct {
                    IPv6Mode                                                mode;           ///< IPv6 provisioning strategy for connected clients.
                    ppp::string                                             cidr;           ///< IPv6 CIDR prefix pool from which client addresses are allocated.
                    int                                                     prefix_length;  ///< Prefix length delegated to each client (e.g. 64 for /64).
                    ppp::string                                             gateway;        ///< IPv6 gateway address announced to clients.
                    ppp::string                                             dns1;           ///< Primary IPv6 DNS server address announced to clients.
                    ppp::string                                             dns2;           ///< Secondary IPv6 DNS server address announced to clients.
                    int                                                     lease_time;     ///< IPv6 address lease duration in seconds.
                    ppp::map<ppp::string, ppp::string>                      static_addresses; ///< Map of client GUID to statically assigned IPv6 address string.
                }                                                           ipv6;
                /**
                 * @brief Server-side IPv4 address pool configuration.
                 *
                 * When present in the JSON configuration file, the server
                 * enables automatic IPv4 address assignment for connected
                 * clients.  The @c configured flag is set to true whenever
                 * the @c server.ipv4-pool JSON object exists, even if
                 * individual fields are missing.
                 */
                struct {
                    bool                                            configured;  ///< True when @c server.ipv4-pool was present in the JSON config.
                    ppp::string                                     network;     ///< IPv4 network address (e.g. "10.0.0.0").
                    ppp::string                                     mask;        ///< IPv4 subnet mask (e.g. "255.255.255.0").
                }                                                           ipv4_pool;
            }                                                               server;         ///< Server-mode specific parameters.
            struct {
                ppp::string                                                 guid;           ///< Client GUID string used for authentication and session tracking.
                ppp::string                                                 server;         ///< Primary VPN server address in "host:port" format.
                ppp::vector<ppp::string>                                    servers;        ///< Backup server entries ("IP:port" / "[IPv6]:port"); scheme, path and websocket host/sni are inherited from server.
                ppp::string                                                 server_proxy;   ///< HTTP/SOCKS proxy address used to reach the VPN server; empty = direct.
                int64_t                                                     bandwidth;      ///< Client-side bandwidth cap in bits per second; 0 = unlimited.
                struct {
                    int                                                     timeout;        ///< Seconds to wait before attempting a reconnection after disconnect.
                }                                                           reconnections;
                struct {
                    ppp::string                                             host;           ///< HTTP Host header value sent in the WebSocket upgrade request (CDN host override).
                    ppp::string                                             sni;            ///< TLS SNI (Server Name Indication) value for the WebSocket connection.
                }                                                           websocket;      ///< WebSocket Host/SNI override for CDN-optimized routing.
                ppp::string                                                 log;            ///< Client-side event log file path; empty disables client logging.
                struct {
                    bool                                                    enabled         = true;         ///< Master switch; false = no probing, legacy single-entry behavior.
                    int                                                     timeout_ms      = 800;          ///< Per-entry probe timeout in milliseconds.
                    int                                                     ttl_seconds     = 30;           ///< Probe result validity in seconds before a background refresh.
                }                                                           probe;          ///< Connectivity probe configuration.
                struct {
                    bool                                                    enabled                 = true;     ///< Master switch; auto-enabled when client.servers is non-empty, explicit false keeps legacy failover.
                    int                                                     jitter_threshold_ms     = 50;       ///< Fluctuation gate (ms): switch only when the window fluctuation reaches this.
                    int                                                     jitter_window           = 3;        ///< Fluctuation sample window AND the consecutive periods required to switch.
                    double                                                  weight_rtt              = 0.6;      ///< Score weight of RTT; jitter weight = 1 - weight_rtt.
                    double                                                  switch_margin           = 0.15;     ///< Minimum score gap (current - best) required to switch.
                    int                                                     channels_per_entry      = 0;        ///< Resident channel distribution per entry (0 = all channels on the best/sticky entry).
                    // Built-in fixed parameters (not configurable through JSON).
                    int                                                     lock_seconds            = 60;       ///< Lock period after activation; no switch-back during it.
                    int                                                     penalty_seconds         = 60;       ///< Blacklist penalty for a failed preheat entry.
                    bool                                                    preheat                 = true;     ///< Preheat (progressive) switch; false = direct switch (passive mode).
                    int                                                     observe_ms              = 2000;     ///< Ready observation window before activation; rollback if the old entry recovers.
                    int                                                     drain_timeout_seconds   = 120;      ///< Force-dispose retiring channels that fail to drain.
                }                                                           hot_switch;             ///< Preheated progressive hot-switch configuration (multi-entry).
#if defined(_WIN32)
                struct {
                    bool                                                    tcp;            ///< Enable Paper Airplane TCP acceleration driver on Windows when true.
                }                                                           paper_airplane;
#endif
                ppp::vector<MappingConfiguration>                           mappings;       ///< List of static port mapping rules activated on connect.
                ppp::vector<RouteConfiguration>                             routes;         ///< List of route sources imported after tunnel establishment.
                ClientRoutingConfiguration                                  routing;        ///< Canonical routing policy; legacy route fields remain mirrored during migration.
                struct {
                    int                                                     port;           ///< HTTP proxy listen port; 0 disables the local HTTP proxy.
                    ppp::string                                             bind;           ///< HTTP proxy bind address; empty = loopback only.
                }                                                           http_proxy;
                struct {
                    int                                                     port;           ///< SOCKS5 proxy listen port; 0 disables the local SOCKS5 proxy.
                    ppp::string                                             bind;           ///< SOCKS5 proxy bind address; empty = loopback only.
                    ppp::string                                             username;       ///< SOCKS5 authentication username; empty = no authentication.
                    ppp::string                                             password;       ///< SOCKS5 authentication password; empty = no authentication.
                }                                                           socks_proxy;
                bool                                                        proxy_only;     ///< Local HTTP/SOCKS runtime; suppresses host TUN routes/DNS takeover while native policy remains active (also implied by --mode=proxy).
            }                                                               client;         ///< Client-mode specific parameters.
            struct {
                bool                                                        enabled;        ///< Enable telemetry output when true; default false for zero-cost on low-end hardware.
                int                                                         level;          ///< Minimum verbosity level to output: 0=INFO, 1=VERB, 2=DEBUG, 3=TRACE.
                bool                                                        count;          ///< Enable counter metrics when true.
                bool                                                        span;           ///< Enable trace spans when true.
                ppp::string                                                 endpoint;       ///< Optional OTLP/gRPC endpoint; empty uses built-in stderr backend.
                ppp::string                                                 log_file;       ///< Optional local log file path; empty disables file output.
                bool                                                        console_log;    ///< Show log events on local console/file sink.
                bool                                                        console_metric; ///< Show counter/gauge/histogram events on local console/file sink.
                bool                                                        console_span;   ///< Show span events on local console/file sink.
            }                                                               telemetry;      ///< Optional telemetry/observability configuration.
            struct {
                bool                                                        enabled;               ///< Enable geo-rules generation; default false (no-op).
                ppp::string                                                 country;               ///< Target country/region code; default "cn".
                ppp::string                                                 geoip_dat;             ///< Local GeoIP dat cache path for downloads; default "GeoIP.dat".
                ppp::string                                                 geosite_dat;           ///< Local GeoSite dat cache path for downloads; default "GeoSite.dat".
                ppp::string                                                 geoip_download_url;    ///< Optional URL used to download/update geoip_dat before generation.
                ppp::string                                                 geosite_download_url;  ///< Optional URL used to download/update geosite_dat before generation.
                ppp::vector<ppp::string>                                    geoip;                 ///< GeoIP/CIDR source file paths (text CIDR format).
                ppp::vector<ppp::string>                                    geosite;               ///< GeoSite/domain source file paths (text domain format).
                ppp::string                                                 dns_provider_domestic; ///< DNS provider for domestic rules; falls back to dns.servers.domestic or "doh.pub".
                ppp::string                                                 dns_provider_foreign;  ///< DNS provider for foreign rules (reserved); falls back to dns.servers.foreign or "cloudflare".
                ppp::string                                                 output_bypass;         ///< Generated bypass output file path; default "./generated/bypass-cn.txt".
                ppp::string                                                 output_dns_rules;      ///< Generated DNS rules output file path; default "./generated/dns-rules-cn.txt".
                ppp::vector<ppp::string>                                    append_bypass;         ///< Extra CIDR lines or file paths appended after GeoIP results.
                ppp::vector<ppp::string>                                    append_dns_rules;      ///< Extra DNS rule lines, file paths, or rules:// URLs appended after GeoSite results.
            }                                                               geo_rules;      ///< GeoIP/GeoSite rule generation configuration (Phase G).
        public:
            /**
             * @brief Initializes configuration fields to default values.
             */
            AppConfiguration() noexcept;

        public:
            /**
             * @brief Resets all fields to built-in defaults.
             */
            void                                                            Clear() noexcept;
            /**
             * @brief Loads configuration data from a JSON object.
             * @param json Source JSON object.
             * @return True when loading and normalization succeed.
             */
            bool                                                            Load(Json::Value& json) noexcept;
            /**
             * @brief Loads configuration data from a JSON file path.
             * @param path Path to the configuration file.
             * @return True when loading and normalization succeed.
             */
            bool                                                            Load(const ppp::string& path) noexcept;

        public:
            /**
             * @brief Internal LCG modifier slot identifiers.
             *
             * Each slot holds a precomputed linear-congruential modifier value
             * derived from the configured key parameters.  The value is used to
             * vary packet framing offsets at runtime, making traffic patterns
             * less predictable.
             */
            enum LcgmodType {
                LCGMOD_TYPE_TRANSMISSION,   ///< Modifier applied to the protocol transmission layer framing.
                LCGMOD_TYPE_STATIC,         ///< Modifier applied to static UDP channel framing.
                LCGMOD_TYPE_MAX             ///< Sentinel value equal to the number of defined modifier slots.
            };
            /**
             * @brief Gets the computed LCG modifier for a type.
             * @param tp Modifier type.
             * @return Computed modifier value.
             */
            int                                                             Lcgmod(LcgmodType tp) noexcept { return _lcgmods[(int)tp]; }

        public:
            /**
             * @brief Gets the current buffer allocator.
             * @return Shared pointer to the allocator.
             */
            std::shared_ptr<ppp::threading::BufferswapAllocator>            GetBufferAllocator() noexcept { return this->_BufferAllocator; }
            /**
             * @brief Replaces the current buffer allocator.
             * @param allocator New allocator instance.
             * @return Previous allocator instance.
             */
            std::shared_ptr<ppp::threading::BufferswapAllocator>            SetBufferAllocator(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) noexcept {
                std::shared_ptr<ppp::threading::BufferswapAllocator> result = std::move(this->_BufferAllocator);
                this->_BufferAllocator = allocator;
                return result;
            }

        public:
            /**
             * @brief Serializes configuration to a JSON value.
             * @return JSON object representation.
             */
            Json::Value                                                     ToJson() noexcept;
            /**
             * @brief Serializes configuration to a JSON string.
             * @return JSON string representation.
             */
            ppp::string                                                     ToString() noexcept;
            /**
             * @brief Returns the absolute path of the JSON file used by Load(path).
             */
            const ppp::string&                                              GetSourcePath() const noexcept { return _source_path; }
            /**
             * @brief Emits a startup security diagnostics report.
             *
             * Scans the loaded configuration for weak/default/short keys and
             * plaintext mode.  Each finding is logged via the telemetry subsystem
             * and written to the console.  All findings are non-fatal warnings;
             * startup never fails as a result of this call.
             *
             * @note Called once during application startup after telemetry is
             *       configured.  Safe to call multiple times (idempotent).
             */
            void                                                            EmitSecurityDiagnostics() noexcept;

            /**
             * @brief Emits a startup report describing the active MUX scheduler.
             *
             * Logs the active `mux.mode` and, when an unknown configured value
             * was normalized to compat, emits a non-fatal warning describing the
             * fallback. Startup never fails as a result of this call.
             *
             * @note Called once during application startup after telemetry is
             *       configured.  Safe to call multiple times (idempotent).
             */
            void                                                            EmitMuxDiagnostics() noexcept;
            /**
             * @brief Re-runs configuration normalization after runtime CLI overrides.
             * @return True when normalization completes.
             */
            bool                                                            Normalize() noexcept { return Loaded(); }
            /**
             * @brief Applies the client proxy-only runtime defaults.
             *
             * Forces the local HTTP/SOCKS listeners onto loopback and enables
             * the proxy-only runtime.  Used when the CLI requests proxy mode or
             * the loaded configuration has client.proxy-only set.
             */
            void                                                            ApplyProxyModeDefaults() noexcept;

        private:
            /**
             * @brief Validates and normalizes loaded values.
             * @return True after normalization completes.
             * @note Called internally by both `Load` overloads after JSON parsing.
             *       Clamps out-of-range integers, derives LCG modifiers, and
             *       initialises the buffer allocator when not already set.
             */
            bool                                                            Loaded() noexcept;

        private:
            int                                                             _lcgmods[LCGMOD_TYPE_MAX]; ///< Precomputed LCG modifier values indexed by `LcgmodType`.
            std::shared_ptr<ppp::threading::BufferswapAllocator>            _BufferAllocator;           ///< Shared buffer pool for packet allocation; may be null until explicitly set.
            ppp::string                                                     _mux_mode_diagnostic;       ///< Transient note set by Loaded() when mux.mode was normalized; emitted at startup.
            ppp::string                                                     _source_path;                ///< Original JSON file path, retained so management can upload the exact source text.
            std::atomic<int>                                                _mux_mode_runtime_override{ -1 }; ///< Debug-only runtime scheduler override (-1=none, else mux::vmux_net::mux_mode); set by remote mux-mode-set, survives session rebuilds.

        public:
            /**
             * @brief Returns the effective scheduler mode token for new mux sessions.
             *
             * Returns the debug runtime override (set by an accepted remote
             * `mux-mode-set`) when present; otherwise the configured `mux.mode`.
             * Safe to call from any thread.
             */
            ppp::string                                                     GetEffectiveMuxMode() const noexcept;
            /**
             * @brief Records a debug-only runtime scheduler override.
             * @param mode_value Scheduler mode value (mux::vmux_net::mux_mode), or -1 to clear.
             * @note Lock-free; readable by the session-rebuild path on another thread.
             */
            void                                                            SetMuxModeRuntimeOverride(int mode_value) noexcept;
        };

        namespace extensions {
            /**
             * @brief Checks whether protocol and transport cipher settings are present.
             * @param configuration Configuration instance to check.
             * @return True when required cipher fields are all non-empty.
             */
            bool                                                            IsHaveCiphertext(const AppConfiguration& configuration) noexcept;
            /**
             * @brief Pointer overload for @ref IsHaveCiphertext.
             * @param configuration Optional configuration pointer.
             * @return True when pointer is valid and ciphertext fields are present.
             */
            inline bool                                                     IsHaveCiphertext(const AppConfiguration* configuration) noexcept { return NULLPTR != configuration ? IsHaveCiphertext(*configuration) : false; }
        }
    }
}


