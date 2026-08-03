#pragma once

/**
 * @file VEthernetPeerLocalBridge.h
 * @brief Bridges inbound tunnel TCP/UDP packets to the locally announced
 *        peer LAN (e.g. 192.168.68.0/24) by actively connecting to the
 *        target device from the local host.
 *
 * When client.peer-local-bridge is enabled, inbound IPv4 packets whose
 * destination matches a prefix announced via peer-route-announce are no
 * longer injected into the TAP/OS protocol stack for forwarding. Instead
 * openppp2 itself opens a local TCP/UDP socket towards the target device.
 * The source address of that local connection is the host's own address on
 * the LAN (e.g. 192.168.68.249), so return traffic is delivered straight
 * back to the host and never traverses an intermediate gateway that lacks
 * a route back to the tunnel side.
 *
 * @author ("OPENPPP2 Team")
 * @license ("GPL-3.0")
 */

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/threading/Executors.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/transmissions/ITransmission.h>
#include <ppp/net/native/ip.h>
#include <ppp/net/native/tcp.h>
#include <ppp/net/native/udp.h>
#include <ppp/net/native/icmp.h>
#include <ppp/net/native/checksum.h>
#include <boost/asio/ip/icmp.hpp>

#if defined(_WIN32)
#include <windows/ppp/net/QoSS.h>
#elif defined(_LINUX)
#include <linux/ppp/net/ProtectorNetwork.h>
#endif

namespace ppp {
    namespace app {
        namespace client {
            class VEthernetExchanger;
            class VEthernetNetworkSwitcher;

            /**
             * @brief Identifies one active TCP bridge by its five-tuple
             *        (client_ip:client_port -> server_ip:server_port).
             *        All fields are in network byte order.
             */
            struct VEthernetPeerLocalBridgeKey {
                uint32_t                                                client_ip   = 0;
                uint16_t                                                client_port = 0;
                uint32_t                                                server_ip   = 0;
                uint16_t                                                server_port = 0;

                bool                                                    operator==(const VEthernetPeerLocalBridgeKey& other) const noexcept {
                    return client_ip == other.client_ip &&
                           client_port == other.client_port &&
                           server_ip == other.server_ip &&
                           server_port == other.server_port;
                }
            };

            /**
             * @brief One active TCP bridge between the tunnel (PVE side) and a
             *        locally connected LAN device (e.g. 192.168.68.10).
             *
             * The tunnel side speaks raw IPv4 TCP packets (PacketAction_NAT),
             * while the local side is a plain asio TCP socket. This class
             * maintains a minimal server-side TCP state machine (ISN/SEQ/ACK)
             * so that the PVE peer sees a normal TCP handshake.
             */
            class VEthernetPeerLocalBridgeConnection : public std::enable_shared_from_this<VEthernetPeerLocalBridgeConnection> {
                friend class                                            VEthernetExchanger;

            public:
                enum BridgeProtocol {
                    BridgeProtocol_TCP = 0,
                    BridgeProtocol_ICMP = 1
                };
                typedef ppp::configurations::AppConfiguration           AppConfiguration;
                typedef std::shared_ptr<AppConfiguration>               AppConfigurationPtr;
                typedef ppp::threading::Executors                       Executors;
                typedef ppp::threading::Executors::StrandPtr            StrandPtr;
                typedef std::shared_ptr<boost::asio::io_context>        ContextPtr;
                typedef ppp::transmissions::ITransmission               ITransmission;
                typedef std::shared_ptr<ITransmission>                  ITransmissionPtr;
                typedef std::mutex                                      SynchronizedObject;
                typedef std::lock_guard<SynchronizedObject>             SynchronizedObjectScope;
                typedef std::shared_ptr<VEthernetExchanger>             VEthernetExchangerPtr;
                typedef std::shared_ptr<VEthernetNetworkSwitcher>       VEthernetNetworkSwitcherPtr;
                typedef ppp::net::native::ip_hdr                        ip_hdr;
                typedef ppp::net::native::tcp_hdr                       tcp_hdr;
                typedef ppp::coroutines::YieldContext                   YieldContext;

            public:
                /**
                 * @brief Initializes a TCP bridge.
                 * @param exchanger Owning data-plane exchanger.
                 * @param transmission Tunnel transmission channel.
                 * @param client_ip   PVE-side source IP (network order).
                 * @param client_port PVE-side source port (network order).
                 * @param server_ip   LAN target IP (network order), e.g. 192.168.68.10.
                 * @param server_port LAN target port (network order), e.g. 22.
                 */
                VEthernetPeerLocalBridgeConnection(
                    const VEthernetExchangerPtr&                        exchanger,
                    const ITransmissionPtr&                             transmission,
                    uint32_t                                            client_ip,
                    uint16_t                                            client_port,
                    uint32_t                                            server_ip,
                    uint16_t                                            server_port,
                    BridgeProtocol                                      protocol) noexcept;
                virtual ~VEthernetPeerLocalBridgeConnection() noexcept;

            public:
                std::shared_ptr<VEthernetPeerLocalBridgeConnection>     GetReference()  noexcept { return shared_from_this(); }
                VEthernetExchangerPtr                                   GetExchanger()  noexcept { return exchanger_; }
                ContextPtr                                              GetContext()    noexcept { return context_; }
                AppConfigurationPtr                                     GetConfiguration() noexcept { return configuration_; }
                uint32_t                                                GetClientIP()   noexcept { return client_ip_; }
                uint16_t                                                GetClientPort() noexcept { return client_port_; }
                uint32_t                                                GetServerIP()   noexcept { return server_ip_; }
                uint16_t                                                GetServerPort() noexcept { return server_port_; }
                bool                                                    IsConnected()   noexcept { return connected_ && !disposed_; }
                bool                                                    IsDisposed()    noexcept { return disposed_; }

            public:
                /**
                 * @brief Feeds one inbound tunnel IPv4 packet (already stripped
                 *        of the PacketAction_NAT action byte).
                 * @param packet       Raw IPv4 packet.
                 * @param packet_length IPv4 packet length.
                 * @return True when the packet was consumed (or queued).
                 * @note  Only TCP packets whose five-tuple matches this bridge
                 *        are accepted.
                 */
                bool                                                    OnTunnelPacket(const void* packet, int packet_length) noexcept;
                /**
                 * @brief Schedules asynchronous disposal.
                 * @return void.
                 */
                virtual void                                            Dispose() noexcept;

            private:
                void                                                    Finalize() noexcept;
                bool                                                    Open(YieldContext& y) noexcept;
                bool                                                    StartLocalConnect() noexcept;
                bool                                                    StartLocalIcmpBridge() noexcept;
                void                                                    OnLocalConnected(const boost::system::error_code& ec) noexcept;
                void                                                    OnLocalIcmpReceived(const boost::system::error_code& ec, std::size_t bytes_transferred) noexcept;
                bool                                                    SendSynAck() noexcept;
                bool                                                    SendIcmpToLocalSocket(const void* packet, int packet_length) noexcept;
                bool                                                    ReceiveIcmpFromLocal() noexcept;
                bool                                                    ForwardIcmpReplyToTunnel(const std::shared_ptr<Byte>& buffer, int bytes_transferred) noexcept;
                bool                                                    SendPacket(uint32_t seq, uint32_t ack, int flags, const void* payload, int payload_length) noexcept;
                bool                                                    SendFin() noexcept;
                void                                                    ReceiveSocketToTransmission() noexcept;
                bool                                                    ForwardSocketToTunnel(const std::shared_ptr<Byte>& buffer, int buffer_size, int bytes_transferred) noexcept;
                bool                                                    WriteToLocalSocket(const void* data, int data_length) noexcept;
                void                                                    UpdateTimeout() noexcept;
                void                                                    MarkFinalize() noexcept { finalize_ = true; }
                bool                                                    IsFinalized() noexcept { return finalize_; }

            private:
                struct {
                    bool                                                disposed_  : 1;
                    bool                                                connected_ : 1;
                    bool                                                syn_ack_sent_ : 1;
                    bool                                                established_ : 1;
                    bool                                                finalize_ : 4;
                    UInt64                                              timeout_   = 0;
                };
                SynchronizedObject                                      syncobj_;
                ContextPtr                                              context_;
                StrandPtr                                               strand_;
                VEthernetNetworkSwitcherPtr                             switcher_;
                VEthernetExchangerPtr                                   exchanger_;
                ITransmissionPtr                                        transmission_;
                AppConfigurationPtr                                     configuration_;
                std::shared_ptr<boost::asio::ip::tcp::socket>           socket_;
                std::shared_ptr<boost::asio::ip::icmp::socket>         icmp_socket_;
                std::shared_ptr<Byte>                                   buffer_;
                std::shared_ptr<Byte>                                   icmp_buffer_;
                BridgeProtocol                                          protocol_ = BridgeProtocol_TCP;
                uint32_t                                                client_ip_;
                uint16_t                                                client_port_;
                uint32_t                                                server_ip_;
                uint16_t                                                server_port_;
                uint32_t                                                client_isn_  = 0;   // PVE initial sequence number.
                uint32_t                                                server_isn_  = 0;   // Local simulated server ISN.
                uint32_t                                                client_seq_  = 0;   // Next expected PVE sequence number.
                uint32_t                                                server_seq_  = 0;   // Next local sequence number to transmit.
                uint32_t                                                client_ack_  = 0;   // Last ACK received from PVE.
                uint32_t                                                server_ack_  = 0;   // ACK number transmitted to PVE.
            };
        }
    }
}

namespace std {
    template <>
    struct hash<ppp::app::client::VEthernetPeerLocalBridgeKey> {
        size_t operator()(const ppp::app::client::VEthernetPeerLocalBridgeKey& key) const noexcept {
            size_t seed = 0;
            seed ^= (size_t)key.client_ip + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= (size_t)key.client_port + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= (size_t)key.server_ip + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= (size_t)key.server_port + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
}
