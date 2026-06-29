#pragma once

/**
 * @file DnsResolver.h
 * @brief Multi-protocol upstream DNS resolver for provider-based dns-rules.
 */

#include <ppp/stdafx.h>
#include <atomic>
#include <mutex>

/* Forward-declare OpenSSL session type so the public header does not pull in
 * <openssl/ssl.h>. The cache stores opaque SSL_SESSION pointers, lifetime
 * managed via SSL_SESSION_up_ref / SSL_SESSION_free in the implementation. */
struct ssl_session_st;

namespace ppp {
    namespace dns {

        enum class Protocol {
            UDP,
            TCP,
            DoH,
            DoT,
        };

        struct ServerEntry {
            Protocol                                        protocol = Protocol::UDP;
            ppp::string                                     url;
            ppp::string                                     hostname;
            ppp::string                                     address;
            ppp::vector<boost::asio::ip::address>           bootstrap_ips;
        };

        class DnsResolver final : public std::enable_shared_from_this<DnsResolver> {
        public:
            typedef ppp::function<bool(int native_handle)>  ProtectSocketCallback;
            typedef ppp::function<void(ppp::vector<Byte>)>  ResolveCallback;
            typedef ppp::function<void(boost::asio::ip::address)> ExitIpCallback;

        public:
            explicit DnsResolver(boost::asio::io_context& context) noexcept;
            ~DnsResolver() noexcept;

            void                                            SetProtectSocketCallback(const ProtectSocketCallback& cb) noexcept;

            void                                            ResolveAsync(
                const ppp::string&                          provider_name,
                bool                                        domestic,
                const Byte*                                 packet,
                int                                         length,
                const ResolveCallback&                      callback) noexcept;

            static bool                                     HasProvider(const ppp::string& name) noexcept;
            static const ppp::vector<ServerEntry>*          GetProvider(const ppp::string& name) noexcept;

            /**
             * @brief Controls whether AAAA (IPv6) responses are propagated to clients.
             *
             * @details When the VPN session has not been assigned a managed IPv6
             *          address by the server, returning AAAA records to local
             *          applications causes 30+ second connect delays because the
             *          OS will attempt the IPv6 destination before falling back to
             *          IPv4. Setting allow=false makes the resolver synthesize an
             *          immediate empty NOERROR response for any AAAA query, which
             *          eliminates that latency without changing A-record behaviour.
             *          The default is false: AAAA queries are filtered until the
             *          server confirms an IPv6 assignment via OnInformation. This
             *          is the safer default because most VPN sessions are IPv4-only
             *          and an unsolicited AAAA pass-through would cause client
             *          applications to attempt unreachable IPv6 destinations.
             *
             * @param allow  Pass false to filter AAAA queries; true to permit them.
             */
            void                                            SetAllowIPv6Response(bool allow) noexcept { allow_ipv6_response_.store(allow, std::memory_order_relaxed); }
            bool                                            IsAllowIPv6Response() const noexcept { return allow_ipv6_response_.load(std::memory_order_relaxed); }

            /**
             * @brief Returns true when the supplied DNS query is asking for AAAA records.
             *
             * @param packet  Raw DNS wire-format query bytes.
             * @param length  Length of @p packet.
             */
            static bool                                     IsAaaaQuery(const Byte* packet, int length) noexcept;

            /**
             * @brief Builds an empty NOERROR response for the supplied AAAA query.
             *
             * @details Copies the question section verbatim, flips QR=1, sets RA=1,
             *          and zeros all answer counts. Used to short-circuit AAAA
             *          queries when IPv6 is not available end-to-end.
             *
             * @param packet  Original AAAA query bytes.
             * @param length  Length of @p packet.
             * @return Synthesised response bytes; empty vector on parse failure.
             */
            static ppp::vector<Byte>                        BuildAaaaBlockedResponse(const Byte* packet, int length) noexcept;

        private:
            void                                            TryProtocols(
                std::shared_ptr<ppp::vector<ServerEntry> >  entries,
                std::size_t                                 index,
                std::shared_ptr<ppp::vector<Byte> >         packet,
                const ResolveCallback&                      callback,
                bool                                        domestic = false) noexcept;

            void                                            SendUdp(
                const ServerEntry&                          entry,
                std::shared_ptr<ppp::vector<Byte> >         packet,
                const ResolveCallback&                      callback) noexcept;

            void                                            SendTcp(
                const ServerEntry&                          entry,
                std::shared_ptr<ppp::vector<Byte> >         packet,
                const ResolveCallback&                      callback) noexcept;

            void                                            SendDoh(
                const ServerEntry&                          entry,
                std::shared_ptr<ppp::vector<Byte> >         packet,
                const ResolveCallback&                      callback) noexcept;

            void                                            SendDot(
                const ServerEntry&                          entry,
                std::shared_ptr<ppp::vector<Byte> >         packet,
                const ResolveCallback&                      callback) noexcept;

            bool                                            ProtectSocket(int native_handle) noexcept;

            /**
             * @brief Looks up a previously cached TLS session for a given upstream.
             *
             * @details Returned pointer is up-ref'd; the caller takes ownership and
             *          must release it via SSL_SESSION_free (or pass it to
             *          SSL_set_session, which up-refs again — in that case the
             *          caller must still free its own reference).
             *
             * @param host_key  Cache key composed from "<host>:<port>".
             * @return SSL_SESSION* on cache hit; nullptr if no usable session is cached.
             */
            ssl_session_st*                                 AcquireTlsSession(const ppp::string& host_key) noexcept;

            /**
             * @brief Stores a TLS session for future resumption.
             *
             * @details Replaces any previously cached session for the same key.
             *          Takes ownership of @p session (the caller's reference is
             *          consumed; the cache will free it when evicted or on
             *          DnsResolver destruction).
             */
            void                                            StoreTlsSession(const ppp::string& host_key, ssl_session_st* session) noexcept;

            /**
             * @brief Resolves a hostname to an IPv4 address using the system DNS resolver.
             *
             * @details A reusable bootstrap DNS helper that does NOT depend on the
             *          provider infrastructure.  Sends a raw DNS A-record query
             *          to well-known public DNS servers (e.g. 8.8.8.8) via UDP.
             *          Intended for resolving DoH/DoT hostnames during bootstrap
             *          when provider IPs are not yet known.
             *
             * @param hostname  The hostname to resolve.
             * @param callback  Invoked with the first resolved IPv4 address, or
             *                  an unspecified address on failure/timeout.
             */
            static void                                     ResolveHostnameAsync(
                boost::asio::io_context&                    context,
                const ppp::string&                          hostname,
                const ExitIpCallback&                       callback) noexcept;

        private:
            boost::asio::io_context&                        context_;
            ProtectSocketCallback                           protect_socket_;
            std::atomic<bool>                               allow_ipv6_response_{ false }; ///< When false, AAAA queries are answered with empty NOERROR. Default false; promoted to true by OnInformation when the server assigns IPv6.

            /**
             * @brief Cache of OpenSSL session tickets keyed by "<host>:<port>".
             *
             * @details Populated after each successful DoH/DoT TLS handshake and
             *          consumed before the next handshake to the same upstream so
             *          that TLS 1.2/1.3 session resumption (1-RTT or 0-RTT) is
             *          used in place of a full handshake. Lifetime of the stored
             *          SSL_SESSION* is owned by this map; SSL_SESSION_free is
             *          called on replace and on resolver destruction.
             */
            struct TlsSessionCacheEntry {
                ssl_session_st*                         session = NULLPTR;
                ppp::list<ppp::string>::iterator        lru;
            };

            mutable std::mutex                              tls_session_mutex_;
            ppp::list<ppp::string>                          tls_session_lru_;
            ppp::unordered_map<ppp::string, TlsSessionCacheEntry> tls_session_cache_;
        };

    } // namespace dns
} // namespace ppp
