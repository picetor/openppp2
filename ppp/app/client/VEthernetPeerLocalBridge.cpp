#include <ppp/app/client/VEthernetPeerLocalBridge.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/net/native/ip.h>
#include <ppp/transmissions/ITransmission.h>

namespace ppp {
    namespace app {
        namespace client {
            VEthernetPeerLocalBridgeConnection::VEthernetPeerLocalBridgeConnection(
                const VEthernetExchangerPtr& exchanger,
                const ITransmissionPtr& transmission,
                uint32_t client_ip,
                uint16_t client_port,
                uint32_t server_ip,
                uint16_t server_port) noexcept
                : context_(NULLPTR)
                , switcher_(NULLPTR)
                , exchanger_(exchanger)
                , transmission_(transmission)
                , client_ip_(client_ip)
                , client_port_(client_port)
                , server_ip_(server_ip)
                , server_port_(server_port) {
                disposed_    = false;
                connected_   = false;
                syn_ack_sent_ = false;
                established_ = false;
                finalize_    = false;
                timeout_     = Executors::GetTickCount() + (UInt64)60 * 1000;

                if (NULLPTR != exchanger) {
                    context_ = exchanger->GetContext();
                    configuration_ = exchanger->GetConfiguration();
                    switcher_ = exchanger->GetSwitcher();
                }
            }

            VEthernetPeerLocalBridgeConnection::~VEthernetPeerLocalBridgeConnection() noexcept {
                Finalize();
            }

            void VEthernetPeerLocalBridgeConnection::Finalize() noexcept {
                if (finalize_) {
                    return;
                }

                MarkFinalize();
                disposed_ = true;

                ppp::net::Socket::Closesocket(socket_);
                socket_.reset();
                buffer_.reset();
                transmission_.reset();
            }

            void VEthernetPeerLocalBridgeConnection::Dispose() noexcept {
                auto self = shared_from_this();
                ppp::threading::Executors::Post(context_, strand_,
                    [self, this]() noexcept {
                        Finalize();
                    });
            }

            void VEthernetPeerLocalBridgeConnection::UpdateTimeout() noexcept {
                timeout_ = Executors::GetTickCount() + (UInt64)60 * 1000;
            }

            bool VEthernetPeerLocalBridgeConnection::Open(YieldContext& y) noexcept {
                if (disposed_ || NULLPTR == context_ || NULLPTR == transmission_) {
                    return false;
                }

                std::shared_ptr<boost::asio::ip::tcp::socket> socket =
                    make_shared_object<boost::asio::ip::tcp::socket>(*context_);
                if (NULLPTR == socket) {
                    return false;
                }

                boost::system::error_code ec;
                socket->open(boost::asio::ip::tcp::v4(), ec);
                if (ec) {
                    return false;
                }

                socket_ = socket;
                buffer_ = configuration_->GetBufferAllocator()->MakeArray<Byte>(PPP_BUFFER_SIZE);
                if (NULLPTR == buffer_) {
                    ppp::net::Socket::Closesocket(socket_);
                    socket_.reset();
                    return false;
                }

                return StartLocalConnect();
            }

            bool VEthernetPeerLocalBridgeConnection::StartLocalConnect() noexcept {
                std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
                if (NULLPTR == socket) {
                    return false;
                }

                boost::asio::ip::tcp::endpoint destinationEP(
                    boost::asio::ip::address_v4(ntohl(server_ip_)), 
                    ntohs(server_port_));

                auto self = shared_from_this();
                socket->async_connect(destinationEP,
                    [self, this](const boost::system::error_code& ec) noexcept {
                        OnLocalConnected(ec);
                    });
                return true;
            }

            void VEthernetPeerLocalBridgeConnection::OnLocalConnected(const boost::system::error_code& ec) noexcept {
                if (disposed_) {
                    return;
                }

                if (ec) {
                    Dispose();
                    return;
                }

                connected_ = true;
                UpdateTimeout();

                // If the PVE SYN was already received, complete the simulated
                // server handshake by replying SYN-ACK back through the tunnel.
                if (!syn_ack_sent_ && client_isn_ != 0) {
                    if (SendSynAck()) {
                        ReceiveSocketToTransmission();
                    }
                    else {
                        Dispose();
                    }
                }
                else {
                    ReceiveSocketToTransmission();
                }
            }

            bool VEthernetPeerLocalBridgeConnection::SendSynAck() noexcept {
                if (syn_ack_sent_) {
                    return true;
                }

                server_isn_  = static_cast<uint32_t>(RandomNext());
                server_seq_  = server_isn_;
                server_ack_  = client_isn_ + 1;
                syn_ack_sent_ = true;
                return SendPacket(server_isn_, client_isn_ + 1,
                    tcp_hdr::TCP_SYN | tcp_hdr::TCP_ACK, NULLPTR, 0);
            }

            bool VEthernetPeerLocalBridgeConnection::OnTunnelPacket(const void* packet, int packet_length) noexcept {
                if (disposed_ || NULLPTR == packet || packet_length < (int)sizeof(ip_hdr)) {
                    return false;
                }

                int ip_length = packet_length;
                ip_hdr* ip = ip_hdr::Parse((void*)packet, ip_length);
                if (NULLPTR == ip || ip_length < (int)sizeof(ip_hdr)) {
                    return false;
                }

                if (ip_hdr::IPH_PROTO(ip) != ip_hdr::IP_PROTO_TCP) {
                    return false;
                }

                Byte* tcp_start = (Byte*)packet + (ip_hdr::IPH_HL(ip) << 2);
                int tcp_available = ip_length - (int)(tcp_start - (Byte*)packet);
                if (tcp_available < (int)sizeof(tcp_hdr)) {
                    return false;
                }

                tcp_hdr* tcp = tcp_hdr::Parse(ip, tcp_start, tcp_available);
                if (NULLPTR == tcp) {
                    return false;
                }

                // Five-tuple must match this bridge.
                if (ip->src != client_ip_ || tcp->src != client_port_ ||
                    ip->dest != server_ip_ || tcp->dest != server_port_) {
                    return false;
                }

                UpdateTimeout();

                Byte flags = tcp_hdr::TCPH_FLAGS(tcp);
                int tcp_header_length = tcp_hdr::TCPH_HDRLEN_BYTES(tcp);
                Byte* payload = tcp_start + tcp_header_length;
                int payload_length = tcp_available - tcp_header_length;
                uint32_t seq = ntohl(tcp->seqno);

                if (flags & tcp_hdr::TCP_RST) {
                    Dispose();
                    return true;
                }

                if (flags & tcp_hdr::TCP_FIN) {
                    // Peer closed: half-close the local socket write side and
                    // relay the FIN once all pending local data is flushed.
                    if (NULLPTR != socket_) {
                        boost::system::error_code ec;
                        socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
                    }
                    SendFin();
                    return true;
                }

                if (!(flags & tcp_hdr::TCP_SYN) && established_) {
                    // Data / ACK phase: accept contiguous payload only.
                    if (payload_length > 0) {
                        if (seq != client_seq_) {
                            // Out-of-order: drop silently; the PVE side will retransmit.
                            return true;
                        }

                        if (!WriteToLocalSocket(payload, payload_length)) {
                            Dispose();
                            return true;
                        }

                        client_seq_ = seq + (uint32_t)payload_length;
                        client_ack_ = client_seq_;
                    }
                    else {
                        // Pure ACK (including the final handshake ACK).
                        if (seq != client_seq_) {
                            // Allow the handshake ACK exactly once.
                            if (!syn_ack_sent_ || seq == client_isn_ + 1) {
                                client_seq_ = seq;
                                client_ack_ = client_seq_;
                            }
                        }
                        else {
                            client_ack_ = client_seq_;
                        }
                    }
                    return true;
                }

                if (flags & tcp_hdr::TCP_SYN) {
                    // Handshake: record the PVE ISN (or a retransmitted SYN).
                    client_isn_ = seq;
                    client_seq_ = seq + 1;
                    client_ack_ = client_seq_;

                    if (connected_) {
                        // Local socket already connected; finish the handshake now.
                        if (!syn_ack_sent_) {
                            if (SendSynAck()) {
                                established_ = true;
                                ReceiveSocketToTransmission();
                            }
                            else {
                                Dispose();
                            }
                        }
                        else {
                            // Retransmitted SYN: replay SYN-ACK.
                            SendPacket(server_isn_, client_isn_ + 1,
                                tcp_hdr::TCP_SYN | tcp_hdr::TCP_ACK, NULLPTR, 0);
                        }
                    }
                    return true;
                }

                if (!syn_ack_sent_) {
                    // SYN-ACK was never sent; nothing more to do.
                    return true;
                }

                // Handshake ACK from PVE (third packet). Mark established.
                established_ = true;
                if (payload_length > 0) {
                    if (seq == client_seq_) {
                        if (!WriteToLocalSocket(payload, payload_length)) {
                            Dispose();
                            return true;
                        }
                        client_seq_ = seq + (uint32_t)payload_length;
                        client_ack_ = client_seq_;
                    }
                }
                else {
                    client_ack_ = client_seq_;
                }
                ReceiveSocketToTransmission();
                return true;
            }

            bool VEthernetPeerLocalBridgeConnection::SendPacket(uint32_t seq, uint32_t ack, int flags, const void* payload, int payload_length) noexcept {
                if (NULLPTR == transmission_ || disposed_) {
                    return false;
                }

                int tcp_header_length = (int)sizeof(tcp_hdr);
                int total_length = (int)sizeof(ip_hdr) + tcp_header_length + std::max<int>(payload_length, 0);
                if (total_length > PPP_BUFFER_SIZE) {
                    return false;
                }

                std::shared_ptr<Byte> buffer = configuration_->GetBufferAllocator()->MakeArray<Byte>(total_length + 1);
                if (NULLPTR == buffer) {
                    return false;
                }

                Byte* p = buffer.get() + 1;   // +1 reserves the NAT action byte.
                ip_hdr* ip = reinterpret_cast<ip_hdr*>(p);
                ip->v_hl  = 0x45;
                ip->tos   = 0;
                ip->len   = htons((unsigned short)total_length);
                ip->id    = htons(ip_hdr::NewId());
                ip->flags = htons(ip_hdr::IP_DF);
                ip->ttl   = ppp::net::native::ip_hdr::IP_DFT_TTL;
                ip->proto = ip_hdr::IP_PROTO_TCP;
                ip->src   = server_ip_;
                ip->dest  = client_ip_;

                tcp_hdr* tcp = reinterpret_cast<tcp_hdr*>(p + sizeof(ip_hdr));
                tcp->src  = server_port_;
                tcp->dest = client_port_;
                tcp->seqno = htonl(seq);
                tcp->ackno = htonl(ack);
                tcp->hdrlen_rsvd_flags = htons((5 << 12) | (flags & tcp_hdr::TCP_FLAGS));
                tcp->wnd   = htons(65535);
                tcp->chksum = 0;
                tcp->urgp  = 0;

                if (payload_length > 0 && NULLPTR != payload) {
                    memcpy(p + sizeof(ip_hdr) + tcp_header_length, payload, payload_length);
                }

                // TCP checksum over the pseudo header.
                tcp->chksum = ppp::net::native::inet_chksum_pseudo(
                    reinterpret_cast<unsigned char*>(tcp),
                    IPPROTO_TCP,
                    (unsigned int)(tcp_header_length + std::max<int>(payload_length, 0)),
                    server_ip_, client_ip_);

                // IPv4 header checksum.
                ip->chksum = ppp::net::native::inet_chksum(ip, (int)sizeof(ip_hdr));

                // Wrap with the NAT action byte and write to the tunnel.
                buffer.get()[0] = (Byte)ppp::app::protocol::VirtualEthernetLinklayer::PacketAction_NAT;
                ITransmissionPtr transmission = transmission_;
                auto self = shared_from_this();
                return transmission->Write(buffer.get(), total_length + 1,
                    [self, this, buffer](bool ok) noexcept {
                        if (!ok) {
                            Dispose();
                        }
                        else {
                            UpdateTimeout();
                        }
                    });
            }

            bool VEthernetPeerLocalBridgeConnection::SendFin() noexcept {
                if (!syn_ack_sent_) {
                    return false;
                }

                return SendPacket(server_seq_, client_ack_,
                    tcp_hdr::TCP_FIN | tcp_hdr::TCP_ACK, NULLPTR, 0);
            }

            bool VEthernetPeerLocalBridgeConnection::WriteToLocalSocket(const void* data, int data_length) noexcept {
                std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
                if (NULLPTR == socket || data_length < 1 || NULLPTR == data) {
                    return false;
                }

                auto self = shared_from_this();
                std::shared_ptr<Byte> copy = configuration_->GetBufferAllocator()->MakeArray<Byte>(data_length);
                if (NULLPTR == copy) {
                    return false;
                }

                memcpy(copy.get(), data, data_length);
                boost::asio::async_write(*socket, boost::asio::buffer(copy.get(), data_length),
                    [self, this, copy, data_length](const boost::system::error_code& ec, std::size_t bytes_transferred) noexcept {
                        if (ec || bytes_transferred != (std::size_t)data_length) {
                            Dispose();
                            return;
                        }

                        UpdateTimeout();
                    });
                return true;
            }

            void VEthernetPeerLocalBridgeConnection::ReceiveSocketToTransmission() noexcept {
                std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
                std::shared_ptr<Byte> buffer = buffer_;
                if (NULLPTR == socket || NULLPTR == buffer || disposed_) {
                    return;
                }

                auto self = shared_from_this();
                socket->async_read_some(boost::asio::buffer(buffer.get(), PPP_BUFFER_SIZE),
                    [self, this, buffer](const boost::system::error_code& ec, std::size_t sz) noexcept {
                        if (disposed_) {
                            return;
                        }

                        if (ec) {
                            // EOF or error: send FIN to the tunnel peer.
                            SendFin();
                            Dispose();
                            return;
                        }

                        int bytes_transferred = (int)sz;
                        if (bytes_transferred < 1) {
                            SendFin();
                            Dispose();
                            return;
                        }

                        if (ForwardSocketToTunnel(buffer, PPP_BUFFER_SIZE, bytes_transferred)) {
                            UpdateTimeout();
                        }
                        else {
                            Dispose();
                        }
                    });
            }

            bool VEthernetPeerLocalBridgeConnection::ForwardSocketToTunnel(const std::shared_ptr<Byte>& buffer, int buffer_size, int bytes_transferred) noexcept {
                if (NULLPTR == buffer || buffer_size < 1 || bytes_transferred < 1) {
                    return false;
                }

                if (disposed_ || !syn_ack_sent_) {
                    return false;
                }

                // Packet sequence: server_seq_ is the ISN; +1 for the SYN itself.
                uint32_t seq = server_seq_ == server_isn_ ? server_isn_ + 1 : server_seq_;
                bool ok = SendPacket(seq, client_ack_, tcp_hdr::TCP_PSH | tcp_hdr::TCP_ACK,
                    buffer.get(), bytes_transferred);
                if (ok) {
                    server_seq_ = seq + (uint32_t)bytes_transferred;
                }
                return ok;
            }
        }
    }
}
