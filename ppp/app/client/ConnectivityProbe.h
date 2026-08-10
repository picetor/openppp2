#pragma once

#include <ppp/stdafx.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/coroutines/YieldContext.h>

namespace ppp {
    namespace app {
        namespace client {
            /**
             * @brief Connectivity probe for VPN tunnel entries.
             *
             * Probes a single tunnel endpoint up to a configurable depth
             * (L1 TCP connect, L3 TLS/WebSocket upgrade) and reports
             * reachability plus round-trip latency.  Probing never performs
             * the L4/L5 link-layer handshake because that creates a real
             * session on the peer (ghost sessions).  All probing I/O runs on
             * the caller's io_context and honours a per-entry timeout.
             *
             * Available on desktop, Android and iOS.
             */
            class ConnectivityProbe final {
            public:
                typedef ppp::coroutines::YieldContext                  YieldContext;
                typedef boost::asio::ip::tcp::endpoint                 TCPEndPoint;
                typedef boost::asio::ip::udp::endpoint                 UDPEndPoint;

            public:
                typedef enum {
                    ProbeType_Tcp          = 0,  ///< Raw PPP/TCP tunnel (scheme ppp/tcp).
                    ProbeType_WebSocket    = 1,  ///< Plain WebSocket tunnel (scheme ws/http).
                    ProbeType_WebSocketSSL = 2,  ///< TLS WebSocket tunnel (scheme wss/https).
                    ProbeType_Udp          = 3,  ///< Static UDP channel (udp.static.servers).
                }                                       ProbeType;
                typedef ppp::function<bool(int)>                                ProtectSocketHandler;

            public:
                /**
                 * @brief One cached probe outcome for an entry.
                 */
                struct Result final {
                    ppp::string                         entry;          ///< Normalized "host:port" entry.
                    uint8_t                             type = 0;       ///< ProbeType.
                    bool                                reachable = false;  ///< Reached the configured probe depth.
                    int                                 rtt_ms = 0;     ///< Round-trip latency to the deepest stage reached.
                    uint8_t                             stage = 0;      ///< Deepest layer reached (1=TCP, 2=TLS, 3=WS upgrade).
                    uint64_t                            timestamp = 0;  ///< Probe time (Executors::GetTickCount).
                    int                                 ttl_ms = 0;     ///< Result validity before refresh.
                    uint64_t                            penalty_until = 0;  ///< Tick-count deadline while the entry is temporarily blacklisted.
                };

            public:
                static bool                             ProbeTcp(const TCPEndPoint& remoteEP, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect = NULLPTR) noexcept;
                static bool                             ProbeWebSocket(const TCPEndPoint& remoteEP, const ppp::string& host, const ppp::string& path, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect = NULLPTR) noexcept;
                static bool                             ProbeWebSocketSSL(const TCPEndPoint& remoteEP, const ppp::string& host, const ppp::string& sni, const ppp::string& path, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect = NULLPTR) noexcept;
                static bool                             ProbeUdp(const UDPEndPoint& remoteEP, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect = NULLPTR) noexcept;
            };
        }
    }
}
