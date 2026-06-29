#include <ppp/stdafx.h>
#include <ppp/dns/DnsResolver.h>
#include <ppp/net/Socket.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/ssl/SSL.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

namespace ppp {
    namespace dns {

        static constexpr int PPP_DNS_RESOLVER_UDP_BUFFER_SIZE   = 4096;
        static constexpr int PPP_DNS_RESOLVER_TCP_MAX_SIZE      = 65535;
        /**
         * @brief Per-protocol upstream timeouts (milliseconds).
         *
         * @details The original implementation used a single 5 s timeout for every
         *          protocol. That meant a single failing DoH endpoint stalled the
         *          fallback chain for 5 s before the next protocol was tried, and
         *          UDP queries — which should normally complete in under 100 ms —
         *          would also block their callers for 5 s on packet loss. The
         *          values below give faster fallback while keeping enough headroom
         *          for the slowest legitimate path (TLS handshake on lossy links).
         */
        static constexpr int PPP_DNS_RESOLVER_UDP_TIMEOUT_MS    = 2000;  ///< Plain UDP: short, retried via fallback.
        static constexpr int PPP_DNS_RESOLVER_TCP_TIMEOUT_MS    = 3000;  ///< Plain TCP / DoT length-prefixed.
        static constexpr int PPP_DNS_RESOLVER_TLS_TIMEOUT_MS    = 4000;  ///< DoH / DoT (TLS handshake + request).

        typedef boost::asio::ip::udp udp;
        typedef boost::asio::ip::tcp tcp;

        static constexpr std::size_t PPP_DNS_TLS_SESSION_CACHE_LIMIT = 32;

        enum class DnsTransportStage {
            Attempt,
            Success,
            Socket,
            Connect,
            Send,
            Recv,
            Parse,
            Tls,
            Http,
            Bootstrap,
            Timeout,
        };

        enum class DnsTransportReason {
            None,
            Failed,
            Invalid,
            Empty,
            AllocFailed,
            OpenFailed,
            ProtectFailed,
            BuildFailed,
            VerifyFailed,
            CacheHit,
            CacheMiss,
            ReuseAttempt,
            Reused,
            NotReused,
            BadStatus,
            Timeout,
        };

        static const char* DnsTransportToString(Protocol protocol) noexcept {
            switch (protocol) {
            case Protocol::UDP: return "udp";
            case Protocol::TCP: return "tcp";
            case Protocol::DoH: return "doh";
            case Protocol::DoT: return "dot";
            default:            return "unknown";
            }
        }

        static const char* DnsTransportStageToString(DnsTransportStage stage) noexcept {
            switch (stage) {
            case DnsTransportStage::Attempt:   return "attempt";
            case DnsTransportStage::Success:   return "success";
            case DnsTransportStage::Socket:    return "socket";
            case DnsTransportStage::Connect:   return "connect";
            case DnsTransportStage::Send:      return "send";
            case DnsTransportStage::Recv:      return "recv";
            case DnsTransportStage::Parse:     return "parse";
            case DnsTransportStage::Tls:       return "tls";
            case DnsTransportStage::Http:      return "http";
            case DnsTransportStage::Bootstrap: return "bootstrap";
            case DnsTransportStage::Timeout:   return "timeout";
            default:                           return "unknown";
            }
        }

        static const char* DnsTransportReasonToString(DnsTransportReason reason) noexcept {
            switch (reason) {
            case DnsTransportReason::None:         return "";
            case DnsTransportReason::Failed:       return "failed";
            case DnsTransportReason::Invalid:      return "invalid";
            case DnsTransportReason::Empty:        return "empty";
            case DnsTransportReason::AllocFailed:  return "alloc_failed";
            case DnsTransportReason::OpenFailed:   return "open_failed";
            case DnsTransportReason::ProtectFailed: return "protect_failed";
            case DnsTransportReason::BuildFailed:  return "build_failed";
            case DnsTransportReason::VerifyFailed: return "verify_failed";
            case DnsTransportReason::CacheHit:     return "cache_hit";
            case DnsTransportReason::CacheMiss:    return "cache_miss";
            case DnsTransportReason::ReuseAttempt: return "reuse_attempt";
            case DnsTransportReason::Reused:       return "reused";
            case DnsTransportReason::NotReused:    return "not_reused";
            case DnsTransportReason::BadStatus:    return "bad_status";
            case DnsTransportReason::Timeout:      return "timeout";
            default:                               return "unknown";
            }
        }

        static void CountDnsTransport(Protocol protocol, DnsTransportStage stage, DnsTransportReason reason = DnsTransportReason::None) noexcept {
            /* telemetry removed */
        }

        static constexpr std::size_t kDnsHeaderSize   = 12;

        /* ========================================================================
         * DNS wire-format parsing helpers
         * ======================================================================== */

        /**
         * @brief Skips a DNS name (sequence of labels or a compression pointer)
         *        starting at position @p pos in @p data.
         *
         * @return Number of bytes consumed from position @p pos, or 0 on error.
         */
        static std::size_t SkipDnsName(const Byte* data, std::size_t size, std::size_t pos) noexcept {
            if (pos >= size) {
                return 0;
            }

            std::size_t consumed = 0;
            for (;;) {
                if (pos + consumed >= size) {
                    return 0;
                }

                Byte label = data[pos + consumed];
                if (label == 0x00) {
                    /* Root label — single null byte. */
                    return consumed + 1;
                }
                if ((label & 0xC0) == 0xC0) {
                    /* Compression pointer — two bytes. */
                    if (pos + consumed + 2 > size) {
                        return 0;
                    }
                    return consumed + 2;
                }

                /* Regular label: length byte + characters. */
                std::size_t label_len = static_cast<std::size_t>(label);
                if (label_len > 63) {
                    return 0; /* Invalid label length. */
                }

                consumed += 1 + label_len;
            }
        }

        /**
         * @brief Skips @p count DNS query (question) entries starting at @p pos.
         *
         * Each question entry consists of: name + QTYPE(2) + QCLASS(2).
         *
         * @return New position after skipping, or 0 on parse error.
         */
        static std::size_t SkipDnsQuestionSection(const Byte* data, std::size_t size, std::size_t pos, uint16_t count) noexcept {
            for (uint16_t i = 0; i < count; ++i) {
                std::size_t name_len = SkipDnsName(data, size, pos);
                if (name_len == 0) {
                    return 0;
                }
                pos += name_len;

                /* QTYPE(2) + QCLASS(2) = 4 bytes */
                if (pos + 4 > size) {
                    return 0;
                }
                pos += 4;
            }
            return pos;
        }

        /**
         * @brief Skips @p count DNS resource records (answers, authority, or additional).
         *
         * Each RR consists of: name + type(2) + class(2) + ttl(4) + rdlength(2) + RDATA(rdlength).
         *
         * @return New position after skipping, or 0 on parse error.
         */
        static std::size_t SkipDnsRrSection(const Byte* data, std::size_t size, std::size_t pos, uint16_t count) noexcept {
            for (uint16_t i = 0; i < count; ++i) {
                std::size_t name_len = SkipDnsName(data, size, pos);
                if (name_len == 0) {
                    return 0;
                }
                pos += name_len;

                /* type(2) + class(2) + ttl(4) + rdlength(2) = 10 bytes */
                if (pos + 10 > size) {
                    return 0;
                }

                uint16_t rdlength = (static_cast<uint16_t>(data[pos + 8]) << 8) |
                                     static_cast<uint16_t>(data[pos + 9]);
                pos += 10 + rdlength;
            }
            return pos;
        }

        /* ========================================================================
         * Original helper functions (unchanged)
         * ======================================================================== */

        static ppp::string NormalizeProviderName(const ppp::string& name) noexcept {
            return ToLower(ATrim(name));
        }

        static boost::asio::ip::address ParseAddressOnly(const ppp::string& address_text, boost::system::error_code& ec) noexcept {
            ppp::string text = ATrim(address_text);
            std::size_t colon = text.find(':');
            if (colon != ppp::string::npos && text.find(':', colon + 1) == ppp::string::npos) {
                text = text.substr(0, colon);
            }

            return StringToAddress(text, ec);
        }

        /**
         * @brief Returns true when @p address_text is an IP literal with optional port.
         */
        static bool IsAddressLiteral(const ppp::string& address_text) noexcept {
            boost::system::error_code ec;
            boost::asio::ip::address ip = ParseAddressOnly(address_text, ec);
            return !ec && !ip.is_unspecified();
        }

        /**
         * @brief Appends built-in provider entries when text is a provider short-name.
         */
        static bool AppendProviderEntries(ppp::vector<ServerEntry>& entries, const ppp::string& provider_name) noexcept {
            const ppp::vector<ServerEntry>* provider = DnsResolver::GetProvider(provider_name);
            if (NULLPTR == provider) {
                return false;
            }

            entries.insert(entries.end(), provider->begin(), provider->end());
            return true;
        }

        static int ParsePort(const ppp::string& address_text, int default_port) noexcept {
            ppp::string text = ATrim(address_text);
            std::size_t colon = text.find(':');
            if (colon == ppp::string::npos || text.find(':', colon + 1) != ppp::string::npos) {
                return default_port;
            }

            ppp::string port_text = ATrim(text.substr(colon + 1));
            if (port_text.empty()) {
                return default_port;
            }

            int port = atoi(port_text.data());
            return port > 0 && port <= UINT16_MAX ? port : default_port;
        }

        static ServerEntry MakeEntry(Protocol protocol, const char* address, const char* hostname = NULLPTR, const char* url = NULLPTR) noexcept {
            ServerEntry entry;
            entry.protocol = protocol;
            if (NULLPTR != address) {
                entry.address = address;
            }
            if (NULLPTR != hostname) {
                entry.hostname = hostname;
            }
            if (NULLPTR != url) {
                entry.url = url;
            }
            return entry;
        }

        /* ========================================================================
         * Provider table — all 12 documented providers.
         * ======================================================================== */

        static const ppp::unordered_map<ppp::string, ppp::vector<ServerEntry> >& Providers() noexcept {
            static const ppp::unordered_map<ppp::string, ppp::vector<ServerEntry> > providers = {
                { "doh.pub", {
                    MakeEntry(Protocol::DoH, "119.29.29.29:443", "doh.pub", "https://doh.pub/dns-query"),
                    MakeEntry(Protocol::DoT, "119.29.29.29:853", "dot.pub"),
                    MakeEntry(Protocol::TCP, "119.29.29.29:53"),
                    MakeEntry(Protocol::UDP, "119.29.29.29:53") } },
                { "alidns", {
                    MakeEntry(Protocol::DoH, "223.5.5.5:443", "dns.alidns.com", "https://dns.alidns.com/dns-query"),
                    MakeEntry(Protocol::DoT, "223.5.5.5:853", "dns.alidns.com"),
                    MakeEntry(Protocol::TCP, "223.5.5.5:53"),
                    MakeEntry(Protocol::UDP, "223.5.5.5:53") } },
                { "baidu", {
                    MakeEntry(Protocol::DoH, "180.76.76.76:443", "doh.baidu.com", "https://doh.baidu.com/dns-query"),
                    MakeEntry(Protocol::TCP, "180.76.76.76:53"),
                    MakeEntry(Protocol::UDP, "180.76.76.76:53") } },
                { "360", {
                    MakeEntry(Protocol::DoH, "101.226.4.6:443", "doh.360.cn", "https://doh.360.cn/dns-query"),
                    MakeEntry(Protocol::DoT, "101.226.4.6:853", "dns.360.cn"),
                    MakeEntry(Protocol::TCP, "101.226.4.6:53"),
                    MakeEntry(Protocol::UDP, "101.226.4.6:53") } },
                { "114", {
                    MakeEntry(Protocol::DoH, "114.114.114.114:443", "dns.114.com", "https://dns.114.com/dns-query"),
                    MakeEntry(Protocol::TCP, "114.114.114.114:53"),
                    MakeEntry(Protocol::UDP, "114.114.114.114:53") } },
                { "tuna", {
                    MakeEntry(Protocol::DoH, "101.6.6.6:443", "doh.tuna.tsinghua.edu.cn", "https://doh.tuna.tsinghua.edu.cn/dns-query"),
                    MakeEntry(Protocol::DoT, "101.6.6.6:853", "dns.tuna.tsinghua.edu.cn"),
                    MakeEntry(Protocol::TCP, "101.6.6.6:53"),
                    MakeEntry(Protocol::UDP, "101.6.6.6:53") } },
                { "cloudflare", {
                    MakeEntry(Protocol::DoH, "1.1.1.1:443", "cloudflare-dns.com", "https://cloudflare-dns.com/dns-query"),
                    MakeEntry(Protocol::DoT, "1.1.1.1:853", "cloudflare-dns.com"),
                    MakeEntry(Protocol::TCP, "1.1.1.1:53"),
                    MakeEntry(Protocol::UDP, "1.1.1.1:53") } },
                { "google", {
                    MakeEntry(Protocol::DoH, "8.8.8.8:443", "dns.google", "https://dns.google/dns-query"),
                    MakeEntry(Protocol::DoT, "8.8.8.8:853", "dns.google"),
                    MakeEntry(Protocol::TCP, "8.8.8.8:53"),
                    MakeEntry(Protocol::UDP, "8.8.8.8:53") } },
                { "quad9", {
                    MakeEntry(Protocol::DoH, "9.9.9.9:443", "dns.quad9.net", "https://dns.quad9.net/dns-query"),
                    MakeEntry(Protocol::DoT, "9.9.9.9:853", "dns.quad9.net"),
                    MakeEntry(Protocol::TCP, "9.9.9.9:53"),
                    MakeEntry(Protocol::UDP, "9.9.9.9:53") } },
                { "adguard", {
                    MakeEntry(Protocol::DoH, "94.140.14.14:443", "dns.adguard.com", "https://dns.adguard.com/dns-query"),
                    MakeEntry(Protocol::DoT, "94.140.14.14:853", "dns.adguard.com"),
                    MakeEntry(Protocol::TCP, "94.140.14.14:53"),
                    MakeEntry(Protocol::UDP, "94.140.14.14:53") } },
                { "nextdns", {
                    MakeEntry(Protocol::DoH, "45.90.28.0:443", "dns.nextdns.io", "https://dns.nextdns.io/dns-query"),
                    MakeEntry(Protocol::DoT, "45.90.28.0:853", "dns.nextdns.io"),
                    MakeEntry(Protocol::TCP, "45.90.28.0:53"),
                    MakeEntry(Protocol::UDP, "45.90.28.0:53") } },
                { "mullvad", {
                    MakeEntry(Protocol::DoH, "194.242.2.2:443", "dns.mullvad.net", "https://dns.mullvad.net/dns-query"),
                    MakeEntry(Protocol::DoT, "194.242.2.2:853", "dns.mullvad.net"),
                    MakeEntry(Protocol::TCP, "194.242.2.2:53"),
                    MakeEntry(Protocol::UDP, "194.242.2.2:53") } },
            };
            return providers;
        }

        /* ========================================================================
         * CompletionState — atomic once-invocation guard
         * ======================================================================== */

        /**
         * @brief Per-query transient resource owner with single-shot completion.
         *
         * @details
         *  This struct centralises ownership of every async resource belonging
         *  to a single SendDoh/SendDot/SendUdp/SendTcp invocation: the deadline
         *  timer, the SSL stream (or raw socket), the SSL context, beast HTTP
         *  buffers, request/response payloads, and the user-supplied
         *  ResolveCallback.
         *
         *  All async lambdas in the chain capture **only** a single
         *  `std::shared_ptr<CompletionState>`; they never capture the timer,
         *  stream, socket, or buffers separately. Resource teardown happens
         *  exclusively inside `Complete()`, under a single CAS guard. This
         *  guarantees:
         *
         *    1. Sockets/timers are closed and cancelled exactly once, on the
         *       thread that wins the CAS.
         *    2. The internal shared_ptr<steady_timer>/shared_ptr<ssl::stream>
         *       objects are released exactly once, immediately after the user
         *       callback runs, on the same thread. No multi-level lambda
         *       destruction chain races to be the "last owner".
         *    3. Late-arriving completion lambdas (e.g. the timer's wait
         *       handler firing after a real response was already received)
         *       observe `completed=true`, take the early-return path, and
         *       merely release their reference to the (already drained)
         *       CompletionState. ~CompletionState then runs harmlessly with
         *       all transient slots already null.
         *
         *  This was added to fix a SIGSEGV observed on Android arm64 inside
         *  ~shared_ptr<steady_timer> at the tail of a DoH read completion.
         *  The crash signature was a virtual call through a freed/poisoned
         *  shared_ptr control block, caused by the steady_timer being
         *  destroyed across multiple racing lambda destruction frames in the
         *  multi-level DoH async chain.
         */
        struct CompletionState final {
            std::atomic<bool>                                                       completed{ false };
            DnsResolver::ResolveCallback                                            callback;

            // Transient resources owned by this query. Populated by SendDoh /
            // SendDot / SendUdp / SendTcp before any async op is started.
            std::shared_ptr<boost::asio::steady_timer>                              timer;
            std::shared_ptr<boost::asio::ssl::context>                              ssl_ctx;
            std::shared_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> > tls_stream;
            std::shared_ptr<boost::asio::ip::tcp::socket>                           tcp_socket;
            std::shared_ptr<boost::asio::ip::udp::socket>                           udp_socket;

            // Generic byte/blob slots. Concrete identity (request/response/
            // length-prefix/HTTP request/HTTP response/flat_buffer/source EP)
            // is local to each Send* implementation. Storing them as
            // type-erased shared_ptr<void> keeps CompletionState protocol-
            // agnostic without splitting it into per-protocol subclasses.
            std::shared_ptr<void>                                                   slot0;
            std::shared_ptr<void>                                                   slot1;
            std::shared_ptr<void>                                                   slot2;
            std::shared_ptr<void>                                                   slot3;

            explicit CompletionState(const DnsResolver::ResolveCallback& cb) noexcept : callback(cb) {}

            /** @brief Returns true if Complete() has already fired. */
            bool                                                                    IsCompleted() const noexcept {
                return completed.load(std::memory_order_acquire);
            }

            /**
             * @brief Atomically signals "this query is done", closes the
             *        underlying I/O endpoints (so in-flight async ops
             *        abort), cancels the timer, then fires the user
             *        callback.
             *
             * @details Idempotent (CAS-guarded). Subsequent calls are
             *          no-ops.
             *
             *          IMPORTANT — what this method DOES NOT do:
             *          it does NOT reset() any of the internal shared_ptr
             *          slots. boost::asio's ssl/socket async ops do NOT
             *          hold a shared_ptr to the underlying stream/socket;
             *          they assume the user keeps it alive for the
             *          duration of the op. Resetting the stream here
             *          (while a handshake_op was in flight) caused the
             *          K70 fault-addr=0x68 null-deref crash inside
             *          ssl::detail::io_op completion: the SSL stream was
             *          destroyed before its in-flight op was delivered.
             *
             *          Lifetime model:
             *          - Every async lambda captures only [state].
             *          - state owns timer/streams/sockets/buffers.
             *          - close()+cancel() here causes every in-flight op
             *            to complete with operation_aborted on its own.
             *          - As each completion lambda runs and is destroyed,
             *            its [state] capture is released. When the LAST
             *            outstanding lambda is destroyed, ref-count on
             *            CompletionState drops to zero and
             *            ~CompletionState destroys timer/stream/socket
             *            members in declaration-reverse order on a stack
             *            frame that has no in-flight op against any of
             *            them. This is the standard asio idiom and is
             *            UAF-free by construction.
             *
             *          The user callback is moved out and invoked LAST,
             *          after close+cancel, so that re-entrant resolve
             *          attempts launched from inside the callback do not
             *          observe stale I/O state.
             */
            void                                                                    Complete(ppp::vector<Byte> response) noexcept {
                bool expected = false;
                if (!completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    return;
                }

                boost::system::error_code ignored;
                if (NULLPTR != tls_stream) {
                    tls_stream->lowest_layer().close(ignored);
                }
                if (NULLPTR != tcp_socket) {
                    tcp_socket->close(ignored);
                }
                if (NULLPTR != udp_socket) {
                    udp_socket->close(ignored);
                }
                if (NULLPTR != timer) {
                    ignored = boost::system::error_code();
                    try {
                        timer->cancel();
                    }
                    catch (const boost::system::system_error& e) {
                        ignored = e.code();
                    }
                }

                DnsResolver::ResolveCallback cb = std::move(callback);
                callback = NULLPTR;

                if (NULLPTR != cb) {
                    cb(std::move(response));
                }
            }
        };

        /* ========================================================================
         * DnsResolver — constructor and property setters
         * ======================================================================== */

        DnsResolver::DnsResolver(boost::asio::io_context& context) noexcept
            : context_(context) {
        }

        DnsResolver::~DnsResolver() noexcept {
            /* Release any cached SSL_SESSION references that survived to shutdown. */
            std::lock_guard<std::mutex> lk(tls_session_mutex_);
            for (auto& kv : tls_session_cache_) {
                if (kv.second.session != NULLPTR) {
                    SSL_SESSION_free(reinterpret_cast<SSL_SESSION*>(kv.second.session));
                }
            }
            tls_session_cache_.clear();
            tls_session_lru_.clear();
        }

        /* ========================================================================
         * TLS session cache — used by SendDoh / SendDot to enable session
         * resumption (TLS 1.2 session ID or TLS 1.3 ticket) on subsequent queries
         * to the same upstream. Cuts the TLS handshake from a full 2-RTT to
         * roughly 1-RTT (resume) or 0-RTT depending on the peer.
         * ======================================================================== */

        ssl_session_st* DnsResolver::AcquireTlsSession(const ppp::string& host_key) noexcept {
            if (host_key.empty()) {
                return NULLPTR;
            }

            std::lock_guard<std::mutex> lk(tls_session_mutex_);
            auto it = tls_session_cache_.find(host_key);
            if (it == tls_session_cache_.end() || it->second.session == NULLPTR) {
                return NULLPTR;
            }

            SSL_SESSION* session = reinterpret_cast<SSL_SESSION*>(it->second.session);
            /* Up-ref so the cache keeps its reference; the caller now owns one
             * additional reference and is responsible for SSL_SESSION_free. */
            if (SSL_SESSION_up_ref(session) != 1) {
                /* Failed to up-ref — drop the cache entry to avoid double-free. */
                SSL_SESSION_free(session);
                tls_session_lru_.erase(it->second.lru);
                tls_session_cache_.erase(it);
                return NULLPTR;
            }
            tls_session_lru_.splice(tls_session_lru_.begin(), tls_session_lru_, it->second.lru);
            return reinterpret_cast<ssl_session_st*>(session);
        }

        void DnsResolver::StoreTlsSession(const ppp::string& host_key, ssl_session_st* session) noexcept {
            if (host_key.empty()) {
                if (session != NULLPTR) {
                    SSL_SESSION_free(reinterpret_cast<SSL_SESSION*>(session));
                }
                return;
            }
            if (session == NULLPTR) {
                return;
            }

            std::lock_guard<std::mutex> lk(tls_session_mutex_);
            auto it = tls_session_cache_.find(host_key);
            if (it != tls_session_cache_.end()) {
                if (it->second.session != NULLPTR) {
                    SSL_SESSION_free(reinterpret_cast<SSL_SESSION*>(it->second.session));
                }
                it->second.session = session;
                tls_session_lru_.splice(tls_session_lru_.begin(), tls_session_lru_, it->second.lru);
                return;
            }
            tls_session_lru_.push_front(host_key);
            TlsSessionCacheEntry entry;
            entry.session = session;
            entry.lru = tls_session_lru_.begin();
            tls_session_cache_.emplace(host_key, entry);
            while (tls_session_cache_.size() > PPP_DNS_TLS_SESSION_CACHE_LIMIT && !tls_session_lru_.empty()) {
                const ppp::string evict_key = tls_session_lru_.back();
                auto evict = tls_session_cache_.find(evict_key);
                if (evict != tls_session_cache_.end()) {
                    if (evict->second.session != NULLPTR) {
                        SSL_SESSION_free(reinterpret_cast<SSL_SESSION*>(evict->second.session));
                    }
                    tls_session_cache_.erase(evict);
                }
                tls_session_lru_.pop_back();
            }
        }

        void DnsResolver::SetProtectSocketCallback(const ProtectSocketCallback& cb) noexcept {
            protect_socket_ = cb;
        }

        /* ========================================================================
         * Provider lookup
         * ======================================================================== */

        bool DnsResolver::HasProvider(const ppp::string& name) noexcept {
            return NULLPTR != GetProvider(name);
        }

        const ppp::vector<ServerEntry>* DnsResolver::GetProvider(const ppp::string& name) noexcept {
            ppp::string key = NormalizeProviderName(name);
            const auto& providers = Providers();
            auto tail = providers.find(key);
            return tail == providers.end() ? NULLPTR : &tail->second;
        }

        /* ========================================================================
         * AAAA short-circuit helpers
         *
         * Walk the DNS query header + question section to identify AAAA queries
         * (QTYPE == 28) and synthesize an empty NOERROR response that mirrors
         * the original question. This lets callers avoid an upstream round-trip
         * when the local data plane has no IPv6 connectivity.
         * ======================================================================== */

        /**
         * @brief Parses the question section to locate QTYPE/QCLASS offsets.
         * @return Offset just past QTYPE+QCLASS (i.e. end of question section), or 0 on failure.
         */
        static int LocateQuestionEnd(const Byte* packet, int length) noexcept {
            if (NULLPTR == packet || length < 12) {
                return 0;
            }
            int idx = 12;
            while (idx < length) {
                Byte label_len = packet[idx];
                if (label_len == 0) {
                    idx += 1;
                    if (idx + 4 > length) {
                        return 0;
                    }
                    return idx + 4; // QTYPE(2) + QCLASS(2)
                }
                if ((label_len & 0xC0) != 0) {
                    // Compression pointer in question section is unusual; not supported here.
                    return 0;
                }
                idx += 1 + label_len;
                if (idx > length) {
                    return 0;
                }
            }
            return 0;
        }

        bool DnsResolver::IsAaaaQuery(const Byte* packet, int length) noexcept {
            int qend = LocateQuestionEnd(packet, length);
            if (qend == 0) {
                return false;
            }
            // QDCOUNT must equal 1 to safely interpret a single question.
            int qdcount = (static_cast<int>(packet[4]) << 8) | static_cast<int>(packet[5]);
            if (qdcount != 1) {
                return false;
            }
            // QR bit must be 0 (it is a query, not a response).
            if ((packet[2] & 0x80) != 0) {
                return false;
            }
            int qtype_off = qend - 4;
            int qtype = (static_cast<int>(packet[qtype_off]) << 8) | static_cast<int>(packet[qtype_off + 1]);
            return qtype == 28; // AAAA
        }

        ppp::vector<Byte> DnsResolver::BuildAaaaBlockedResponse(const Byte* packet, int length) noexcept {
            int qend = LocateQuestionEnd(packet, length);
            if (qend == 0) {
                return ppp::vector<Byte>();
            }

            ppp::vector<Byte> response(static_cast<std::size_t>(qend));
            std::memcpy(response.data(), packet, static_cast<std::size_t>(qend));

            // Header rewrite: QR=1, AA=0, TC=0, RA=1, Z=0, RCODE=0 (NOERROR).
            // Preserve the original ID and the RD bit (bit 0 of byte 2).
            Byte rd = static_cast<Byte>(response[2] & 0x01);
            response[2] = static_cast<Byte>(0x80 | rd); // QR=1 + RD copied
            response[3] = 0x80;                          // RA=1, Z=0, RCODE=NOERROR

            // Counts: QDCOUNT=1 (echoed), ANCOUNT=NSCOUNT=ARCOUNT=0.
            response[4] = 0; response[5] = 1;
            response[6] = 0; response[7] = 0;
            response[8] = 0; response[9] = 0;
            response[10] = 0; response[11] = 0;

            return response;
        }

        /* ========================================================================
         * ResolveAsync — single-provider, protocol-cascading resolution
         * ======================================================================== */

        void DnsResolver::ResolveAsync(const ppp::string& provider_name, bool domestic, const Byte* packet, int length, const ResolveCallback& callback) noexcept {
            if (NULLPTR == callback) {
                return;
            }

            if (NULLPTR == packet || length <= 0 || length > PPP_DNS_RESOLVER_TCP_MAX_SIZE) {
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            const ppp::vector<ServerEntry>* provider = GetProvider(provider_name);
            if (NULLPTR == provider || provider->empty()) {
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            std::shared_ptr<ppp::vector<Byte> > request = make_shared_object<ppp::vector<Byte> >();
            std::shared_ptr<ppp::vector<ServerEntry> > entries = make_shared_object<ppp::vector<ServerEntry> >();
            if (NULLPTR == request || NULLPTR == entries) {
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            try {
                request->assign(packet, packet + length);
                *entries = *provider;
            }
            catch (const std::exception&) {
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            TryProtocols(entries, 0, request, callback, domestic);
        }

        /* ========================================================================
         * Socket protection helper
         * ======================================================================== */

        bool DnsResolver::ProtectSocket(int native_handle) noexcept {
            if (NULLPTR == protect_socket_) {
                return true;
            }

            try {
                return protect_socket_(native_handle);
            }
            catch (const std::exception&) {
                return false;
            }
        }

        /* ========================================================================
         * TryProtocols — cascading protocol fallback for a single provider
         * ======================================================================== */

        void DnsResolver::TryProtocols(std::shared_ptr<ppp::vector<ServerEntry> > entries, std::size_t index, std::shared_ptr<ppp::vector<Byte> > packet, const ResolveCallback& callback, bool domestic) noexcept {
            if (NULLPTR == entries || NULLPTR == packet || index >= entries->size()) {
                callback(ppp::vector<Byte>());
                return;
            }

            const ServerEntry& entry = (*entries)[index];

            std::weak_ptr<DnsResolver> weak_self = weak_from_this();
            ResolveCallback next = [weak_self, entries, index, packet, callback, domestic](ppp::vector<Byte> response) noexcept {
                if (!response.empty()) {
                    callback(std::move(response));
                    return;
                }

                std::shared_ptr<DnsResolver> self = weak_self.lock();
                if (NULLPTR == self) {
                    callback(ppp::vector<Byte>());
                    return;
                }

                self->TryProtocols(entries, index + 1, packet, callback, domestic);
            };

            switch (entry.protocol) {
            case Protocol::UDP:
                SendUdp(entry, packet, next);
                break;
            case Protocol::TCP:
                SendTcp(entry, packet, next);
                break;
            case Protocol::DoH:
                SendDoh(entry, packet, next);
                break;
            case Protocol::DoT:
                SendDot(entry, packet, next);
                break;
            default:
                next(ppp::vector<Byte>());
                break;
            }
        }

        /* ========================================================================
         * SendDoh — DNS-over-HTTPS via Boost.Beast + TLS
         * ======================================================================== */

        void DnsResolver::SendDoh(const ServerEntry& entry, std::shared_ptr<ppp::vector<Byte> > packet, const ResolveCallback& callback) noexcept {
            CountDnsTransport(Protocol::DoH, DnsTransportStage::Attempt);
            /* Parse the DoH URL to extract host and path. */
            ppp::string url = ATrim(entry.url);
            if (url.empty()) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Bootstrap, DnsTransportReason::Empty);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            ppp::string host;
            ppp::string path;
            {
                /* Strip https:// prefix. */
                const char* https_prefix = "https://";
                std::size_t scheme_pos = url.find(https_prefix);
                if (scheme_pos == ppp::string::npos) {
                    CountDnsTransport(Protocol::DoH, DnsTransportStage::Bootstrap, DnsTransportReason::Invalid);
                    boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                    return;
                }
                ppp::string remainder = url.substr(scheme_pos + strlen(https_prefix));

                /* Split host and path. */
                std::size_t slash_pos = remainder.find('/');
                if (slash_pos == ppp::string::npos) {
                    host = remainder;
                    path = "/";
                }
                else {
                    host = remainder.substr(0, slash_pos);
                    path = remainder.substr(slash_pos);
                }

                /* Strip port from host if present (e.g. "dns.example.com:443"). */
                std::size_t colon_pos = host.find(':');
                if (colon_pos != ppp::string::npos) {
                    host = host.substr(0, colon_pos);
                }

                if (host.empty()) {
                    CountDnsTransport(Protocol::DoH, DnsTransportStage::Bootstrap, DnsTransportReason::Empty);
                    boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                    return;
                }
            }

            boost::system::error_code ec;
            boost::asio::ip::address ip = ParseAddressOnly(entry.address, ec);
            if (ec || ip.is_unspecified()) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Bootstrap, DnsTransportReason::Invalid);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            tcp::endpoint remote(ip, static_cast<unsigned short>(ParsePort(entry.address, 443)));

            // Allocate the per-query state up-front. Every async resource is
            // owned by `state` so that all lambdas in the chain only need to
            // capture `[state]`. Resource teardown happens exclusively in
            // CompletionState::Complete() under a single CAS guard.
            std::shared_ptr<CompletionState> state = make_shared_object<CompletionState>(callback);
            std::shared_ptr<tcp::socket> socket = make_shared_object<tcp::socket>(context_);
            std::shared_ptr<boost::asio::steady_timer> timer = make_shared_object<boost::asio::steady_timer>(context_);
            if (NULLPTR == state || NULLPTR == socket || NULLPTR == timer) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Socket, DnsTransportReason::AllocFailed);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }
            state->timer = timer;

            socket->open(remote.protocol(), ec);
            if (ec) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Socket, DnsTransportReason::OpenFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            ppp::net::Socket::AdjustDefaultSocketOptional(socket->native_handle(), ip.is_v4());
            ppp::net::Socket::SetTypeOfService(socket->native_handle());
            ppp::net::Socket::SetSignalPipeline(socket->native_handle(), false);
            ppp::net::Socket::ReuseSocketAddress(socket->native_handle(), true);
            if (!ProtectSocket(socket->native_handle())) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Socket, DnsTransportReason::ProtectFailed);
                ppp::net::Socket::Closesocket(socket);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            /* Create TLS 1.2+ client context with system/bundled CA verification enabled. */
            std::shared_ptr<boost::asio::ssl::context> ssl_ctx = ppp::ssl::SSL::CreateClientSslContext(
                ppp::ssl::SSL::SSL_METHOD::tlsv12, true, std::string());
            if (NULLPTR == ssl_ctx) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::AllocFailed);
                ppp::net::Socket::Closesocket(socket);
                state->Complete(ppp::vector<Byte>());
                return;
            }
            state->ssl_ctx = ssl_ctx;

            std::shared_ptr<boost::asio::ssl::stream<tcp::socket> > stream =
                make_shared_object<boost::asio::ssl::stream<tcp::socket> >(std::move(*socket), *ssl_ctx);
            if (NULLPTR == stream) {
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::AllocFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }
            state->tls_stream = stream;

            /* SNI: use the URL host (or entry.hostname fallback) for TLS Server Name Indication. */
            ppp::string sni_name = !entry.hostname.empty() ? entry.hostname : host;
            if (!sni_name.empty()) {
                SSL_set_tlsext_host_name(stream->native_handle(), sni_name.data());
                stream->set_verify_callback(boost::asio::ssl::host_name_verification(stl::transform<std::string>(sni_name)));
            }

            /* Cache key composed from the SNI name and remote port. Used to look
             * up a previously stored TLS session for resumption (1-RTT instead
             * of a full 2-RTT handshake) and to remember the new session after
             * the handshake completes. */
            ppp::string host_key;
            if (!sni_name.empty()) {
                host_key.append(sni_name).append(":").append(stl::to_string<ppp::string>(static_cast<int>(remote.port())));
            }

            /* Apply a previously cached SSL_SESSION before the handshake. The
             * cache returns an up-ref'd pointer; SSL_set_session up-refs again
             * internally, so we still need to free our own reference. */
            if (!host_key.empty()) {
                if (SSL_SESSION* cached = reinterpret_cast<SSL_SESSION*>(AcquireTlsSession(host_key))) {
                    SSL_set_session(stream->native_handle(), cached);
                    SSL_SESSION_free(cached);
                    CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::CacheHit);
                    CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::ReuseAttempt);
                }
                else {
                    CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::CacheMiss);
                }
            }

            // ---------------------------------------------------------------
            // Async chain. Every lambda captures ONLY [state]. Late-arriving
            // completions check state->IsCompleted() and bail without
            // touching any transient resource (which Complete() has already
            // released).
            // ---------------------------------------------------------------

            timer->expires_after(std::chrono::milliseconds(PPP_DNS_RESOLVER_TLS_TIMEOUT_MS));
            timer->async_wait([state](const boost::system::error_code& ec_) noexcept {
                if (ec_ || state->IsCompleted()) {
                    return;
                }
                CountDnsTransport(Protocol::DoH, DnsTransportStage::Timeout);
                state->Complete(ppp::vector<Byte>());
            });

            std::weak_ptr<DnsResolver> weak_self = weak_from_this();
            stream->lowest_layer().async_connect(remote,
                [weak_self, state, packet, host, path, sni_name, host_key](const boost::system::error_code& connect_ec) noexcept {
                    if (state->IsCompleted()) {
                        return;
                    }
                    if (connect_ec) {
                        CountDnsTransport(Protocol::DoH, DnsTransportStage::Connect, DnsTransportReason::Failed);
                        state->Complete(ppp::vector<Byte>());
                        return;
                    }

                    auto stream_local = state->tls_stream;
                    if (NULLPTR == stream_local) {
                        return;
                    }
                    stream_local->async_handshake(boost::asio::ssl::stream_base::client,
                        [weak_self, state, packet, host, path, sni_name, host_key](const boost::system::error_code& handshake_ec) noexcept {
                            if (state->IsCompleted()) {
                                return;
                            }
                            if (handshake_ec) {
                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls,
                                    handshake_ec == boost::asio::error::operation_aborted ? DnsTransportReason::Failed : DnsTransportReason::VerifyFailed);
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }

                            auto stream_inner = state->tls_stream;
                            if (NULLPTR == stream_inner) {
                                return;
                            }

                            /* Persist the negotiated session for the next query.
                             * SSL_get1_session up-refs the session; the cache
                             * takes ownership of that reference. */
                            if (std::shared_ptr<DnsResolver> self = weak_self.lock(); self != NULLPTR && !host_key.empty()) {
                                if (SSL_SESSION* fresh = SSL_get1_session(stream_inner->native_handle())) {
                                    self->StoreTlsSession(host_key, reinterpret_cast<ssl_session_st*>(fresh));
                                    if (SSL_session_reused(stream_inner->native_handle())) {
                                        CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::Reused);
                                    }
                                    else {
                                        CountDnsTransport(Protocol::DoH, DnsTransportStage::Tls, DnsTransportReason::NotReused);
                                    }
                                }
                            }

                            /* Build HTTP/1.1 POST request with DNS wire-format body. */
                            typedef boost::beast::http::request<boost::beast::http::string_body> http_request_t;
                            typedef boost::beast::http::response<boost::beast::http::string_body> http_response_t;
                            std::shared_ptr<http_request_t> http_req = make_shared_object<http_request_t>();
                            std::shared_ptr<boost::beast::flat_buffer> read_buf = make_shared_object<boost::beast::flat_buffer>();
                            std::shared_ptr<http_response_t> http_res = make_shared_object<http_response_t>();
                            if (NULLPTR == http_req || NULLPTR == read_buf || NULLPTR == http_res) {
                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Http, DnsTransportReason::AllocFailed);
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }
                            // Park the HTTP request/response/parse buffer on
                            // the state so they outlive each individual
                            // lambda in the chain without being captured
                            // separately.
                            state->slot0 = http_req;
                            state->slot1 = read_buf;
                            state->slot2 = http_res;

                            try {
                                http_req->method(boost::beast::http::verb::post);
                                http_req->target(path);
                                http_req->version(11);
                                http_req->set(boost::beast::http::field::host, host);
                                http_req->set(boost::beast::http::field::content_type, "application/dns-message");
                                http_req->set(boost::beast::http::field::accept, "application/dns-message");
                                http_req->body().assign(packet->begin(), packet->end());
                                http_req->prepare_payload();
                            }
                            catch (const std::exception&) {
                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Http, DnsTransportReason::BuildFailed);
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }

                            /* Send HTTP request. */
                            boost::beast::http::async_write(*stream_inner, *http_req,
                                [state](const boost::system::error_code& write_ec, std::size_t) noexcept {
                                    if (state->IsCompleted()) {
                                        return;
                                    }
                                    if (write_ec) {
                                        CountDnsTransport(Protocol::DoH, DnsTransportStage::Send, DnsTransportReason::Failed);
                                        state->Complete(ppp::vector<Byte>());
                                        return;
                                    }

                                    auto stream_w = state->tls_stream;
                                    auto read_buf_w = std::static_pointer_cast<boost::beast::flat_buffer>(state->slot1);
                                    auto http_res_w = std::static_pointer_cast<boost::beast::http::response<boost::beast::http::string_body> >(state->slot2);
                                    if (NULLPTR == stream_w || NULLPTR == read_buf_w || NULLPTR == http_res_w) {
                                        return;
                                    }

                                    /* Read HTTP response. read_buf and http_res are kept
                                     * alive via state->slot1 / state->slot2 for the entire
                                     * duration of async_read; beast holds them only by
                                     * reference. */
                                    boost::beast::http::async_read(*stream_w, *read_buf_w, *http_res_w,
                                        [state](const boost::system::error_code& read_ec, std::size_t) noexcept {
                                            if (state->IsCompleted()) {
                                                return;
                                            }
                                            if (read_ec) {
                                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Recv, DnsTransportReason::Failed);
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }

                                            auto http_res_r = std::static_pointer_cast<boost::beast::http::response<boost::beast::http::string_body> >(state->slot2);
                                            if (NULLPTR == http_res_r) {
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }

                                            if (http_res_r->result_int() != 200 || http_res_r->body().empty()) {
                                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Http,
                                                    http_res_r->result_int() != 200 ? DnsTransportReason::BadStatus : DnsTransportReason::Empty);
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }

                                            const std::string& body = http_res_r->body();
                                            try {
                                                ppp::vector<Byte> response(body.begin(), body.end());
                                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Success);
                                                state->Complete(std::move(response));
                                            }
                                            catch (const std::exception&) {
                                                CountDnsTransport(Protocol::DoH, DnsTransportStage::Parse, DnsTransportReason::Failed);
                                                state->Complete(ppp::vector<Byte>());
                                            }
                                        });
                                });
                        });
                });
        }

        /* ========================================================================
         * SendDot — DNS-over-TLS via Boost.Asio SSL stream
         * ======================================================================== */

        void DnsResolver::SendDot(const ServerEntry& entry, std::shared_ptr<ppp::vector<Byte> > packet, const ResolveCallback& callback) noexcept {
            CountDnsTransport(Protocol::DoT, DnsTransportStage::Attempt);
            boost::system::error_code ec;
            boost::asio::ip::address ip = ParseAddressOnly(entry.address, ec);
            if (ec || ip.is_unspecified()) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Bootstrap, DnsTransportReason::Invalid);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            tcp::endpoint remote(ip, static_cast<unsigned short>(ParsePort(entry.address, 853)));

            // Allocate the per-query state up-front. Same lifecycle policy as
            // SendDoh: every transient resource is owned by `state`, every
            // lambda captures only `[state]`, and Complete() is the sole
            // teardown point.
            std::shared_ptr<CompletionState> state = make_shared_object<CompletionState>(callback);
            std::shared_ptr<tcp::socket> socket = make_shared_object<tcp::socket>(context_);
            std::shared_ptr<boost::asio::steady_timer> timer = make_shared_object<boost::asio::steady_timer>(context_);
            std::shared_ptr<ppp::vector<Byte> > request = make_shared_object<ppp::vector<Byte> >();
            std::shared_ptr<std::array<Byte, 2> > length_buffer = make_shared_object<std::array<Byte, 2> >();
            if (NULLPTR == state || NULLPTR == socket || NULLPTR == timer || NULLPTR == request || NULLPTR == length_buffer) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Socket, DnsTransportReason::AllocFailed);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }
            state->timer = timer;
            state->slot0 = request;
            state->slot1 = length_buffer;

            /* Build DNS-over-TCP request: 2-byte big-endian length prefix + raw DNS query. */
            try {
                request->resize(packet->size() + 2);
                (*request)[0] = static_cast<Byte>((packet->size() >> 8) & 0xff);
                (*request)[1] = static_cast<Byte>(packet->size() & 0xff);
                memcpy(request->data() + 2, packet->data(), packet->size());
            }
            catch (const std::exception&) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Parse, DnsTransportReason::BuildFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            socket->open(remote.protocol(), ec);
            if (ec) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Socket, DnsTransportReason::OpenFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            ppp::net::Socket::AdjustDefaultSocketOptional(socket->native_handle(), ip.is_v4());
            ppp::net::Socket::SetTypeOfService(socket->native_handle());
            ppp::net::Socket::SetSignalPipeline(socket->native_handle(), false);
            ppp::net::Socket::ReuseSocketAddress(socket->native_handle(), true);
            if (!ProtectSocket(socket->native_handle())) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Socket, DnsTransportReason::ProtectFailed);
                ppp::net::Socket::Closesocket(socket);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            /* Create a TLS 1.2+ client context with system/bundled CA verification enabled. */
            std::shared_ptr<boost::asio::ssl::context> ssl_ctx = ppp::ssl::SSL::CreateClientSslContext(
                ppp::ssl::SSL::SSL_METHOD::tlsv12, true, std::string());
            if (NULLPTR == ssl_ctx) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::AllocFailed);
                ppp::net::Socket::Closesocket(socket);
                state->Complete(ppp::vector<Byte>());
                return;
            }
            state->ssl_ctx = ssl_ctx;

            std::shared_ptr<boost::asio::ssl::stream<tcp::socket> > stream =
                make_shared_object<boost::asio::ssl::stream<tcp::socket> >(std::move(*socket), *ssl_ctx);
            if (NULLPTR == stream) {
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::AllocFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }
            state->tls_stream = stream;

            /* SNI: use entry.hostname for TLS Server Name Indication. */
            if (!entry.hostname.empty()) {
                SSL_set_tlsext_host_name(stream->native_handle(), entry.hostname.data());
                stream->set_verify_callback(boost::asio::ssl::host_name_verification(stl::transform<std::string>(entry.hostname)));
            }

            /* Cache key for the TLS session cache. See SendDoh for the policy. */
            ppp::string host_key;
            if (!entry.hostname.empty()) {
                host_key.append(entry.hostname).append(":").append(stl::to_string<ppp::string>(static_cast<int>(remote.port())));
            }
            if (!host_key.empty()) {
                if (SSL_SESSION* cached = reinterpret_cast<SSL_SESSION*>(AcquireTlsSession(host_key))) {
                    SSL_set_session(stream->native_handle(), cached);
                    SSL_SESSION_free(cached);
                    CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::CacheHit);
                    CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::ReuseAttempt);
                }
                else {
                    CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::CacheMiss);
                }
            }

            timer->expires_after(std::chrono::milliseconds(PPP_DNS_RESOLVER_TLS_TIMEOUT_MS));
            timer->async_wait([state](const boost::system::error_code& ec_) noexcept {
                if (ec_ || state->IsCompleted()) {
                    return;
                }
                CountDnsTransport(Protocol::DoT, DnsTransportStage::Timeout);
                state->Complete(ppp::vector<Byte>());
            });

            std::weak_ptr<DnsResolver> weak_self = weak_from_this();
            stream->lowest_layer().async_connect(remote,
                [weak_self, state, hostname = entry.hostname, host_key](const boost::system::error_code& connect_ec) noexcept {
                    if (state->IsCompleted()) {
                        return;
                    }
                    if (connect_ec) {
                        CountDnsTransport(Protocol::DoT, DnsTransportStage::Connect, DnsTransportReason::Failed);
                        state->Complete(ppp::vector<Byte>());
                        return;
                    }

                    auto stream_local = state->tls_stream;
                    if (NULLPTR == stream_local) {
                        return;
                    }
                    /* Protect the connected socket before TLS handshake. */
                    stream_local->async_handshake(boost::asio::ssl::stream_base::client,
                        [weak_self, state, host_key](const boost::system::error_code& handshake_ec) noexcept {
                            if (state->IsCompleted()) {
                                return;
                            }
                            if (handshake_ec) {
                                CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls,
                                    handshake_ec == boost::asio::error::operation_aborted ? DnsTransportReason::Failed : DnsTransportReason::VerifyFailed);
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }

                            auto stream_inner = state->tls_stream;
                            if (NULLPTR == stream_inner) {
                                return;
                            }

                            /* Persist negotiated session for the next query. */
                            if (std::shared_ptr<DnsResolver> self = weak_self.lock(); self != NULLPTR && !host_key.empty()) {
                                if (SSL_SESSION* fresh = SSL_get1_session(stream_inner->native_handle())) {
                                    self->StoreTlsSession(host_key, reinterpret_cast<ssl_session_st*>(fresh));
                                    if (SSL_session_reused(stream_inner->native_handle())) {
                                        CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::Reused);
                                    }
                                    else {
                                        CountDnsTransport(Protocol::DoT, DnsTransportStage::Tls, DnsTransportReason::NotReused);
                                    }
                                }
                            }

                            auto request_inner = std::static_pointer_cast<ppp::vector<Byte> >(state->slot0);
                            if (NULLPTR == request_inner) {
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }

                            /* Send: 2-byte length prefix + DNS query. */
                            boost::asio::async_write(*stream_inner, boost::asio::buffer(request_inner->data(), request_inner->size()),
                                [state](const boost::system::error_code& write_ec, std::size_t) noexcept {
                                    if (state->IsCompleted()) {
                                        return;
                                    }
                                    if (write_ec) {
                                        CountDnsTransport(Protocol::DoT, DnsTransportStage::Send, DnsTransportReason::Failed);
                                        state->Complete(ppp::vector<Byte>());
                                        return;
                                    }

                                    auto stream_w = state->tls_stream;
                                    auto length_buffer_w = std::static_pointer_cast<std::array<Byte, 2> >(state->slot1);
                                    if (NULLPTR == stream_w || NULLPTR == length_buffer_w) {
                                        return;
                                    }

                                    /* Read: 2-byte response length prefix. */
                                    boost::asio::async_read(*stream_w, boost::asio::buffer(length_buffer_w->data(), length_buffer_w->size()),
                                        [state](const boost::system::error_code& read_len_ec, std::size_t) noexcept {
                                            if (state->IsCompleted()) {
                                                return;
                                            }
                                            if (read_len_ec) {
                                                CountDnsTransport(Protocol::DoT, DnsTransportStage::Recv, DnsTransportReason::Failed);
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }

                                            auto length_buffer_r = std::static_pointer_cast<std::array<Byte, 2> >(state->slot1);
                                            if (NULLPTR == length_buffer_r) {
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }

                                            int response_size = (static_cast<int>((*length_buffer_r)[0]) << 8) | static_cast<int>((*length_buffer_r)[1]);
                                            if (response_size <= 0 || response_size > PPP_DNS_RESOLVER_TCP_MAX_SIZE) {
                                                CountDnsTransport(Protocol::DoT, DnsTransportStage::Parse, DnsTransportReason::Invalid);
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }

                                            std::shared_ptr<ppp::vector<Byte> > response = make_shared_object<ppp::vector<Byte> >(response_size);
                                            if (NULLPTR == response) {
                                                CountDnsTransport(Protocol::DoT, DnsTransportStage::Parse, DnsTransportReason::AllocFailed);
                                                state->Complete(ppp::vector<Byte>());
                                                return;
                                            }
                                            // Replace the request buffer slot with the response
                                            // buffer; we no longer need the request after the
                                            // length prefix has been received.
                                            state->slot0 = response;

                                            auto stream_b = state->tls_stream;
                                            if (NULLPTR == stream_b) {
                                                return;
                                            }

                                            /* Read: response body. */
                                            boost::asio::async_read(*stream_b, boost::asio::buffer(response->data(), response->size()),
                                                [state](const boost::system::error_code& read_body_ec, std::size_t) noexcept {
                                                    if (state->IsCompleted()) {
                                                        return;
                                                    }
                                                    if (read_body_ec) {
                                                        CountDnsTransport(Protocol::DoT, DnsTransportStage::Recv, DnsTransportReason::Failed);
                                                        state->Complete(ppp::vector<Byte>());
                                                        return;
                                                    }

                                                    auto response_r = std::static_pointer_cast<ppp::vector<Byte> >(state->slot0);
                                                    if (NULLPTR == response_r) {
                                                        state->Complete(ppp::vector<Byte>());
                                                        return;
                                                    }

                                                    CountDnsTransport(Protocol::DoT, DnsTransportStage::Success);
                                                    state->Complete(std::move(*response_r));
                                                });
                                        });
                                });
                        });
                });
        }

        /* ========================================================================
         * SendUdp — plain DNS over UDP
         * ======================================================================== */

        void DnsResolver::SendUdp(const ServerEntry& entry, std::shared_ptr<ppp::vector<Byte> > packet, const ResolveCallback& callback) noexcept {
            CountDnsTransport(Protocol::UDP, DnsTransportStage::Attempt);
            boost::system::error_code ec;
            boost::asio::ip::address ip = ParseAddressOnly(entry.address, ec);
            if (ec || ip.is_unspecified()) {
                CountDnsTransport(Protocol::UDP, DnsTransportStage::Bootstrap, DnsTransportReason::Invalid);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            udp::endpoint remote(ip, static_cast<unsigned short>(ParsePort(entry.address, PPP_DNS_SYS_PORT)));

            // Centralised state, [state]-only lambda captures, single-shot
            // teardown via state->Complete(). Same lifecycle policy as
            // SendDoh/SendDot.
            std::shared_ptr<CompletionState> state = make_shared_object<CompletionState>(callback);
            std::shared_ptr<udp::socket> socket = make_shared_object<udp::socket>(context_);
            std::shared_ptr<boost::asio::steady_timer> timer = make_shared_object<boost::asio::steady_timer>(context_);
            std::shared_ptr<ppp::vector<Byte> > buffer = make_shared_object<ppp::vector<Byte> >(PPP_DNS_RESOLVER_UDP_BUFFER_SIZE);
            std::shared_ptr<udp::endpoint> source = make_shared_object<udp::endpoint>();
            if (NULLPTR == state || NULLPTR == socket || NULLPTR == timer || NULLPTR == buffer || NULLPTR == source) {
                CountDnsTransport(Protocol::UDP, DnsTransportStage::Socket, DnsTransportReason::AllocFailed);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }
            state->udp_socket = socket;
            state->timer = timer;
            state->slot0 = buffer;
            state->slot1 = source;

            socket->open(remote.protocol(), ec);
            if (ec) {
                CountDnsTransport(Protocol::UDP, DnsTransportStage::Socket, DnsTransportReason::OpenFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            ppp::net::Socket::AdjustDefaultSocketOptional(socket->native_handle(), ip.is_v4());
            ppp::net::Socket::SetTypeOfService(socket->native_handle());
            ppp::net::Socket::SetSignalPipeline(socket->native_handle(), false);
            ppp::net::Socket::ReuseSocketAddress(socket->native_handle(), true);
            if (!ProtectSocket(socket->native_handle())) {
                CountDnsTransport(Protocol::UDP, DnsTransportStage::Socket, DnsTransportReason::ProtectFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            timer->expires_after(std::chrono::milliseconds(PPP_DNS_RESOLVER_UDP_TIMEOUT_MS));
            timer->async_wait([state](const boost::system::error_code& ec_) noexcept {
                if (ec_ || state->IsCompleted()) {
                    return;
                }
                CountDnsTransport(Protocol::UDP, DnsTransportStage::Recv, DnsTransportReason::Timeout);
                CountDnsTransport(Protocol::UDP, DnsTransportStage::Timeout);
                state->Complete(ppp::vector<Byte>());
            });

            socket->async_send_to(boost::asio::buffer(packet->data(), packet->size()), remote,
                [state](const boost::system::error_code& send_ec, std::size_t) noexcept {
                    if (state->IsCompleted()) {
                        return;
                    }
                    if (send_ec) {
                        CountDnsTransport(Protocol::UDP, DnsTransportStage::Send, DnsTransportReason::Failed);
                        state->Complete(ppp::vector<Byte>());
                        return;
                    }

                    auto socket_local = state->udp_socket;
                    auto buffer_local = std::static_pointer_cast<ppp::vector<Byte> >(state->slot0);
                    auto source_local = std::static_pointer_cast<udp::endpoint>(state->slot1);
                    if (NULLPTR == socket_local || NULLPTR == buffer_local || NULLPTR == source_local) {
                        return;
                    }

                    socket_local->async_receive_from(boost::asio::buffer(buffer_local->data(), buffer_local->size()), *source_local,
                        [state](const boost::system::error_code& recv_ec, std::size_t size) noexcept {
                            if (state->IsCompleted()) {
                                return;
                            }
                            if (recv_ec || size < 1) {
                                CountDnsTransport(Protocol::UDP, DnsTransportStage::Recv,
                                    recv_ec ? DnsTransportReason::Failed : DnsTransportReason::Empty);
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }

                            auto buffer_r = std::static_pointer_cast<ppp::vector<Byte> >(state->slot0);
                            if (NULLPTR == buffer_r) {
                                state->Complete(ppp::vector<Byte>());
                                return;
                            }

                            try {
                                buffer_r->resize(size);
                                CountDnsTransport(Protocol::UDP, DnsTransportStage::Success);
                                state->Complete(std::move(*buffer_r));
                            }
                            catch (const std::exception&) {
                                CountDnsTransport(Protocol::UDP, DnsTransportStage::Parse, DnsTransportReason::Failed);
                                state->Complete(ppp::vector<Byte>());
                            }
                        });
                });
        }

        /* ========================================================================
         * SendTcp — plain DNS over TCP (with 2-byte length prefix)
         * ======================================================================== */

        void DnsResolver::SendTcp(const ServerEntry& entry, std::shared_ptr<ppp::vector<Byte> > packet, const ResolveCallback& callback) noexcept {
            CountDnsTransport(Protocol::TCP, DnsTransportStage::Attempt);
            boost::system::error_code ec;
            boost::asio::ip::address ip = ParseAddressOnly(entry.address, ec);
            if (ec || ip.is_unspecified()) {
                CountDnsTransport(Protocol::TCP, DnsTransportStage::Bootstrap, DnsTransportReason::Invalid);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }

            tcp::endpoint remote(ip, static_cast<unsigned short>(ParsePort(entry.address, PPP_DNS_SYS_PORT)));

            // Centralised state, [state]-only lambda captures, single-shot
            // teardown via state->Complete(). Same lifecycle policy as
            // SendDoh/SendDot.
            std::shared_ptr<CompletionState> state = make_shared_object<CompletionState>(callback);
            std::shared_ptr<tcp::socket> socket = make_shared_object<tcp::socket>(context_);
            std::shared_ptr<boost::asio::steady_timer> timer = make_shared_object<boost::asio::steady_timer>(context_);
            std::shared_ptr<ppp::vector<Byte> > request = make_shared_object<ppp::vector<Byte> >();
            std::shared_ptr<std::array<Byte, 2> > length_buffer = make_shared_object<std::array<Byte, 2> >();
            if (NULLPTR == state || NULLPTR == socket || NULLPTR == timer || NULLPTR == request || NULLPTR == length_buffer) {
                CountDnsTransport(Protocol::TCP, DnsTransportStage::Socket, DnsTransportReason::AllocFailed);
                boost::asio::post(context_, [callback]() noexcept { callback(ppp::vector<Byte>()); });
                return;
            }
            state->tcp_socket = socket;
            state->timer = timer;
            state->slot0 = request;
            state->slot1 = length_buffer;

            try {
                request->resize(packet->size() + 2);
                (*request)[0] = static_cast<Byte>((packet->size() >> 8) & 0xff);
                (*request)[1] = static_cast<Byte>(packet->size() & 0xff);
                memcpy(request->data() + 2, packet->data(), packet->size());
            }
            catch (const std::exception&) {
                CountDnsTransport(Protocol::TCP, DnsTransportStage::Parse, DnsTransportReason::BuildFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            socket->open(remote.protocol(), ec);
            if (ec) {
                CountDnsTransport(Protocol::TCP, DnsTransportStage::Socket, DnsTransportReason::OpenFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            ppp::net::Socket::AdjustDefaultSocketOptional(socket->native_handle(), ip.is_v4());
            ppp::net::Socket::SetTypeOfService(socket->native_handle());
            ppp::net::Socket::SetSignalPipeline(socket->native_handle(), false);
            ppp::net::Socket::ReuseSocketAddress(socket->native_handle(), true);
            if (!ProtectSocket(socket->native_handle())) {
                CountDnsTransport(Protocol::TCP, DnsTransportStage::Socket, DnsTransportReason::ProtectFailed);
                state->Complete(ppp::vector<Byte>());
                return;
            }

            timer->expires_after(std::chrono::milliseconds(PPP_DNS_RESOLVER_TCP_TIMEOUT_MS));
            timer->async_wait([state](const boost::system::error_code& ec_) noexcept {
                if (ec_ || state->IsCompleted()) {
                    return;
                }
                CountDnsTransport(Protocol::TCP, DnsTransportStage::Timeout);
                state->Complete(ppp::vector<Byte>());
            });

            socket->async_connect(remote, [state](const boost::system::error_code& connect_ec) noexcept {
                if (state->IsCompleted()) {
                    return;
                }
                if (connect_ec) {
                    CountDnsTransport(Protocol::TCP, DnsTransportStage::Connect, DnsTransportReason::Failed);
                    state->Complete(ppp::vector<Byte>());
                    return;
                }

                auto socket_local = state->tcp_socket;
                auto request_local = std::static_pointer_cast<ppp::vector<Byte> >(state->slot0);
                if (NULLPTR == socket_local || NULLPTR == request_local) {
                    return;
                }

                boost::asio::async_write(*socket_local, boost::asio::buffer(request_local->data(), request_local->size()),
                    [state](const boost::system::error_code& write_ec, std::size_t) noexcept {
                        if (state->IsCompleted()) {
                            return;
                        }
                        if (write_ec) {
                            CountDnsTransport(Protocol::TCP, DnsTransportStage::Send, DnsTransportReason::Failed);
                            state->Complete(ppp::vector<Byte>());
                            return;
                        }

                        auto socket_w = state->tcp_socket;
                        auto length_buffer_w = std::static_pointer_cast<std::array<Byte, 2> >(state->slot1);
                        if (NULLPTR == socket_w || NULLPTR == length_buffer_w) {
                            return;
                        }

                        boost::asio::async_read(*socket_w, boost::asio::buffer(length_buffer_w->data(), length_buffer_w->size()),
                            [state](const boost::system::error_code& read_len_ec, std::size_t) noexcept {
                                if (state->IsCompleted()) {
                                    return;
                                }
                                if (read_len_ec) {
                                    CountDnsTransport(Protocol::TCP, DnsTransportStage::Recv, DnsTransportReason::Failed);
                                    state->Complete(ppp::vector<Byte>());
                                    return;
                                }

                                auto length_buffer_r = std::static_pointer_cast<std::array<Byte, 2> >(state->slot1);
                                if (NULLPTR == length_buffer_r) {
                                    state->Complete(ppp::vector<Byte>());
                                    return;
                                }

                                int response_size = (static_cast<int>((*length_buffer_r)[0]) << 8) | static_cast<int>((*length_buffer_r)[1]);
                                if (response_size <= 0 || response_size > PPP_DNS_RESOLVER_TCP_MAX_SIZE) {
                                    CountDnsTransport(Protocol::TCP, DnsTransportStage::Parse, DnsTransportReason::Invalid);
                                    state->Complete(ppp::vector<Byte>());
                                    return;
                                }

                                std::shared_ptr<ppp::vector<Byte> > response = make_shared_object<ppp::vector<Byte> >(response_size);
                                if (NULLPTR == response) {
                                    CountDnsTransport(Protocol::TCP, DnsTransportStage::Parse, DnsTransportReason::AllocFailed);
                                    state->Complete(ppp::vector<Byte>());
                                    return;
                                }
                                state->slot0 = response;  // recycle slot0 (request no longer needed)

                                auto socket_b = state->tcp_socket;
                                if (NULLPTR == socket_b) {
                                    return;
                                }

                                boost::asio::async_read(*socket_b, boost::asio::buffer(response->data(), response->size()),
                                    [state](const boost::system::error_code& read_body_ec, std::size_t) noexcept {
                                        if (state->IsCompleted()) {
                                            return;
                                        }
                                        if (read_body_ec) {
                                            CountDnsTransport(Protocol::TCP, DnsTransportStage::Recv, DnsTransportReason::Failed);
                                            state->Complete(ppp::vector<Byte>());
                                            return;
                                        }

                                        auto response_r = std::static_pointer_cast<ppp::vector<Byte> >(state->slot0);
                                        if (NULLPTR == response_r) {
                                            state->Complete(ppp::vector<Byte>());
                                            return;
                                        }

                                        CountDnsTransport(Protocol::TCP, DnsTransportStage::Success);
                                        state->Complete(std::move(*response_r));
                                    });
                            });
                    });
            });
        }

        /* ========================================================================
         * ResolveHostnameAsync — bootstrap DNS helper (system UDP resolver)
         *
         * Sends a raw DNS A-record query to a well-known public resolver
         * (8.8.8.8) via UDP and returns the first A-record answer IP.
         * This is a reusable helper that does NOT depend on the provider
         * infrastructure and can be used during bootstrap when provider
         * hostnames need to be resolved before the full resolver is ready.
         *
         * Current usage: stub available for future bootstrap scenarios.
         * All providers already have hardcoded IP addresses, so this
         * method is not yet called in the normal resolution path.
         * ======================================================================== */

        /* Build a minimal DNS A-record query for a given hostname. */
        static bool BuildDnsAQuery(const ppp::string& hostname, ppp::vector<Byte>& out_packet) noexcept {
            /* DNS Header (12 bytes): ID=0, flags=standard query (0x0100),
             * QDCOUNT=1, ANCOUNT=0, NSCOUNT=0, ARCOUNT=0. */
            out_packet.clear();
            out_packet.resize(kDnsHeaderSize, 0);
            out_packet[0] = 0x00; out_packet[1] = 0x00; /* ID */
            out_packet[2] = 0x01; out_packet[3] = 0x00; /* Flags: standard query, RD=1 */
            out_packet[4] = 0x00; out_packet[5] = 0x01; /* QDCOUNT = 1 */
            /* ANCOUNT=0, NSCOUNT=0, ARCOUNT=0 (already zero). */

            /* Encode hostname as DNS labels. */
            ppp::string host = ATrim(hostname);
            if (host.empty() || host.size() > 253) {
                return false;
            }

            /* Split by '.' and write each label. */
            std::size_t pos = 0;
            while (pos < host.size()) {
                std::size_t dot = host.find('.', pos);
                ppp::string label;
                if (dot == ppp::string::npos) {
                    label = host.substr(pos);
                    pos = host.size();
                }
                else {
                    label = host.substr(pos, dot - pos);
                    pos = dot + 1;
                }

                if (label.empty() || label.size() > 63) {
                    return false;
                }

                out_packet.push_back(static_cast<Byte>(label.size()));
                for (std::size_t j = 0; j < label.size(); ++j) {
                    out_packet.push_back(static_cast<Byte>(label[j]));
                }
            }
            out_packet.push_back(0x00); /* Root label */

            /* QTYPE = A (1), QCLASS = IN (1). */
            out_packet.push_back(0x00); out_packet.push_back(0x01); /* QTYPE  */
            out_packet.push_back(0x00); out_packet.push_back(0x01); /* QCLASS */

            return true;
        }

        /* Parse a DNS response and extract the first A-record IP address. */
        static boost::asio::ip::address_v4 ParseFirstARecord(const Byte* data, std::size_t size) noexcept {
            if (size < kDnsHeaderSize) {
                return boost::asio::ip::address_v4();
            }

            uint16_t ancount = (static_cast<uint16_t>(data[6]) << 8) | static_cast<uint16_t>(data[7]);
            uint16_t qdcount = (static_cast<uint16_t>(data[4]) << 8) | static_cast<uint16_t>(data[5]);

            std::size_t pos = SkipDnsQuestionSection(data, size, kDnsHeaderSize, qdcount);
            if (pos == 0) {
                return boost::asio::ip::address_v4();
            }

            for (uint16_t i = 0; i < ancount; ++i) {
                std::size_t name_len = SkipDnsName(data, size, pos);
                if (name_len == 0) {
                    return boost::asio::ip::address_v4();
                }
                pos += name_len;

                if (pos + 10 > size) {
                    return boost::asio::ip::address_v4();
                }

                uint16_t rr_type   = (static_cast<uint16_t>(data[pos])     << 8) | static_cast<uint16_t>(data[pos + 1]);
                /* uint16_t rr_class  = (static_cast<uint16_t>(data[pos + 2]) << 8) | static_cast<uint16_t>(data[pos + 3]); */
                /* uint32_t rr_ttl    = ... ; */
                uint16_t rdlength  = (static_cast<uint16_t>(data[pos + 8]) << 8) | static_cast<uint16_t>(data[pos + 9]);

                pos += 10;

                if (pos + rdlength > size) {
                    return boost::asio::ip::address_v4();
                }

                if (rr_type == 1 && rdlength == 4) {
                    /* A record with 4-byte IPv4 address. */
                    boost::asio::ip::address_v4::bytes_type ab;
                    ab[0] = data[pos]; ab[1] = data[pos + 1];
                    ab[2] = data[pos + 2]; ab[3] = data[pos + 3];
                    return boost::asio::ip::address_v4(ab);
                }

                pos += rdlength;
            }

            return boost::asio::ip::address_v4();
        }

        void DnsResolver::ResolveHostnameAsync(
            boost::asio::io_context& context,
            const ppp::string& hostname,
            const ExitIpCallback& callback) noexcept {

            if (NULLPTR == callback || hostname.empty()) {
                if (NULLPTR != callback) {
                    boost::asio::post(context, [callback]() noexcept {
                        callback(boost::asio::ip::address());
                    });
                }
                return;
            }

            /* Build the DNS A-record query. */
            auto query = make_shared_object<ppp::vector<Byte> >();
            if (NULLPTR == query || !BuildDnsAQuery(hostname, *query)) {
                boost::asio::post(context, [callback]() noexcept {
                    callback(boost::asio::ip::address());
                });
                return;
            }

            /* Target: Google public DNS (8.8.8.8:53). */
            boost::system::error_code ec;
            boost::asio::ip::address dns_ip = boost::asio::ip::address_v4(0x08080808);
            udp::endpoint remote(dns_ip, PPP_DNS_SYS_PORT);

            std::shared_ptr<udp::socket> socket = make_shared_object<udp::socket>(context);
            std::shared_ptr<boost::asio::steady_timer> timer = make_shared_object<boost::asio::steady_timer>(context);
            std::shared_ptr<ppp::vector<Byte> > recv_buf = make_shared_object<ppp::vector<Byte> >(PPP_DNS_RESOLVER_UDP_BUFFER_SIZE);
            std::shared_ptr<udp::endpoint> recv_ep = make_shared_object<udp::endpoint>();
            std::shared_ptr<std::atomic<bool> > done = make_shared_object<std::atomic<bool> >(false);

            if (NULLPTR == socket || NULLPTR == timer || NULLPTR == recv_buf || NULLPTR == recv_ep || NULLPTR == done) {
                boost::asio::post(context, [callback]() noexcept {
                    callback(boost::asio::ip::address());
                });
                return;
            }

            socket->open(udp::v4(), ec);
            if (ec) {
                boost::asio::post(context, [callback]() noexcept {
                    callback(boost::asio::ip::address());
                });
                return;
            }

            ppp::net::Socket::AdjustDefaultSocketOptional(socket->native_handle(), true);
            ppp::net::Socket::SetTypeOfService(socket->native_handle());
            ppp::net::Socket::SetSignalPipeline(socket->native_handle(), false);
            ppp::net::Socket::ReuseSocketAddress(socket->native_handle(), true);

            /* Timeout. */
            timer->expires_after(std::chrono::milliseconds(PPP_DNS_RESOLVER_UDP_TIMEOUT_MS));
            timer->async_wait([socket, timer, done, callback](const boost::system::error_code& timer_ec) noexcept {
                (void)timer;
                if (!timer_ec) {
                    bool expected = false;
                    if (done->compare_exchange_strong(expected, true)) {
                        ppp::net::Socket::Closesocket(socket);
                        callback(boost::asio::ip::address());
                    }
                }
            });

            /* Send query. */
            socket->async_send_to(boost::asio::buffer(query->data(), query->size()), remote,
                [socket, timer, query, recv_buf, recv_ep, done, callback](const boost::system::error_code& send_ec, std::size_t) noexcept {
                    (void)query;
                    if (send_ec) {
                        bool expected = false;
                        if (done->compare_exchange_strong(expected, true)) {
                            timer->cancel();
                            ppp::net::Socket::Closesocket(socket);
                            callback(boost::asio::ip::address());
                        }
                        return;
                    }

                    /* Wait for response. */
                    socket->async_receive_from(
                        boost::asio::buffer(recv_buf->data(), recv_buf->size()), *recv_ep,
                        [socket, timer, recv_buf, done, callback](const boost::system::error_code& recv_ec, std::size_t recv_size) noexcept {
                            bool expected = false;
                            if (!done->compare_exchange_strong(expected, true)) {
                                return;
                            }

                            timer->cancel();
                            ppp::net::Socket::Closesocket(socket);

                            if (recv_ec || recv_size < kDnsHeaderSize) {
                                callback(boost::asio::ip::address());
                                return;
                            }

                            boost::asio::ip::address_v4 result = ParseFirstARecord(recv_buf->data(), recv_size);
                            if (result.is_unspecified()) {
                                callback(boost::asio::ip::address());
                            }
                            else {
                                callback(boost::asio::ip::address(result));
                            }
                        });
                });
        }

    } // namespace dns
} // namespace ppp
