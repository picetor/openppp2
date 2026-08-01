#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetDatagramPort.h>
#include <ppp/app/protocol/VirtualEthernetPacket.h>
#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/collections/Dictionary.h>
#include <ppp/auxiliary/UriAuxiliary.h>
#include <ppp/auxiliary/StringAuxiliary.h>
#include <ppp/IDisposable.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/asio/asio.h>
#include <ppp/net/packet/IPFrame.h>
#include <ppp/threading/Timer.h>
#include <ppp/threading/Executors.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/transmissions/ITransmission.h>
#include <ppp/transmissions/ITcpipTransmission.h>
#include <ppp/transmissions/IWebsocketTransmission.h>
#include <ppp/diagnostics/Telemetry.h>

typedef ppp::app::protocol::VirtualEthernetInformation              VirtualEthernetInformation;
typedef ppp::app::protocol::VirtualEthernetPacket                   VirtualEthernetPacket;
typedef ppp::collections::Dictionary                                Dictionary;
typedef ppp::auxiliary::StringAuxiliary                             StringAuxiliary;
typedef ppp::net::AddressFamily                                     AddressFamily;
typedef ppp::net::Socket                                            Socket;
typedef ppp::net::IPEndPoint                                        IPEndPoint;
typedef ppp::net::Ipep                                              Ipep;
typedef ppp::threading::Timer                                       Timer;
typedef ppp::threading::Executors                                   Executors;
typedef ppp::transmissions::ITransmission                           ITransmission;
typedef ppp::transmissions::ITcpipTransmission                      ITcpipTransmission;
typedef ppp::transmissions::IWebsocketTransmission                  IWebsocketTransmission;
typedef ppp::transmissions::ISslWebsocketTransmission               ISslWebsocketTransmission;

namespace ppp {
    namespace app {
        namespace client {
            static constexpr int SEND_ECHO_KEEP_ALIVE_PACKET_MIN_TIMEOUT = 1000;
            static constexpr int SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT = 5000;
            static constexpr int SEND_ECHO_KEEP_ALIVE_PACKET_MMX_TIMEOUT = SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT << 2;
            static constexpr int STATIC_ECHO_KEEP_ALIVED_ID              = IPEndPoint::NoneAddress - 1;

            VEthernetExchanger::VEthernetExchanger(
                const VEthernetNetworkSwitcherPtr&      switcher,
                const AppConfigurationPtr&              configuration,
                const ContextPtr&                       context,
                const Int128&                           id,
                const ppp::string&                      outbound_tag,
                bool                                    primary_outbound) noexcept
                : VirtualEthernetLinklayer(configuration, context, id)
                , disposed_(false)
                , sekap_last_(0)
                , sekap_next_(0)
                , switcher_(switcher)
                , outbound_tag_(outbound_tag)
                , primary_outbound_(primary_outbound)
                , network_state_(NetworkState_Connecting)
                , static_echo_input_(false)
                , static_echo_timeout_(UINT64_MAX)
                , static_echo_session_id_(0)
                , static_echo_remote_port_(IPEndPoint::MinPort) {

                if (configuration->key.protocol.size() > 0 && configuration->key.protocol_key.size() > 0 && 
                    configuration->key.transport.size() > 0 && configuration->key.transport_key.size() > 0) {
                    if (Ciphertext::Support(configuration->key.protocol) && Ciphertext::Support(configuration->key.transport)) {
                        static_echo_protocol_ = make_shared_object<Ciphertext>(configuration->key.protocol, configuration->key.protocol_key);
                        static_echo_transport_ = make_shared_object<Ciphertext>(configuration->key.transport, configuration->key.transport_key);
                    }
                }
                
                buffer_                   = Executors::GetCachedBuffer(context);
                server_url_.port          = 0;
                server_url_.protocol_type = ProtocolType::ProtocolType_PPP;
            }

            VEthernetExchanger::~VEthernetExchanger() noexcept {
                Finalize();
            }

            bool VEthernetExchanger::TranslateIPv6Packet(Byte* packet, int packet_length, bool outbound) noexcept {
                if (primary_outbound_ || NULLPTR == packet || packet_length < ppp::ipv6::IPv6_HEADER_MIN_SIZE) return true;
                if ((packet[0] >> 4) != ppp::ipv6::IPv6_VERSION) return true;

                boost::asio::ip::address assigned;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    assigned = assigned_ipv6_address_;
                }
                if (NULLPTR == switcher_) return false;
                auto tap = switcher_->GetTap();
                if (!assigned.is_v6() || NULLPTR == tap || !tap->IPv6Address.is_v6()) {
                    LOG_DEBUG("VEthernetExchanger::TranslateIPv6Packet: outbound=%s has no usable IPv6 assignment",
                        outbound_tag_.data());
                    return false;
                }

                boost::asio::ip::address_v6 source;
                boost::asio::ip::address_v6 destination;
                Byte next_header = 0;
                int payload_length = 0;
                if (!ppp::ipv6::TryParsePacket(packet, packet_length, source, destination,
                    &next_header, &payload_length)) return false;

                boost::asio::ip::address_v6 tap_address = tap->IPv6Address.to_v6();
                boost::asio::ip::address_v6 assigned_address = assigned.to_v6();
                if (outbound) {
                    if (source == assigned_address) return true;
                    if (source != tap_address) return false;
                    source = assigned_address;
                }
                else {
                    if (destination == tap_address) return true;
                    if (destination != assigned_address) return false;
                    destination = tap_address;
                }

                int checksum_offset = -1;
                if (next_header == IPPROTO_TCP && payload_length >= 20) checksum_offset = 16;
                elif(next_header == IPPROTO_UDP && payload_length >= 8) checksum_offset = 6;
                elif(next_header == IPPROTO_ICMPV6 && payload_length >= 4) checksum_offset = 2;
                else return false;

                ppp::ipv6::PacketHeader* header = reinterpret_cast<ppp::ipv6::PacketHeader*>(packet);
                auto source_bytes = source.to_bytes();
                auto destination_bytes = destination.to_bytes();
                memcpy(header->Source, source_bytes.data(), source_bytes.size());
                memcpy(header->Destination, destination_bytes.data(), destination_bytes.size());

                Byte* payload = packet + ppp::ipv6::IPv6_HEADER_MIN_SIZE;
                payload[checksum_offset] = 0;
                payload[checksum_offset + 1] = 0;
                unsigned short checksum = ppp::ipv6::ComputePseudoChecksum(payload,
                    static_cast<unsigned int>(payload_length), source, destination, next_header);
                if (next_header == IPPROTO_UDP && checksum == 0) checksum = 0xffff;
                memcpy(payload + checksum_offset, &checksum, sizeof(checksum));
                return true;
            }

            void VEthernetExchanger::Finalize() noexcept {
                VirtualEthernetMappingPortTable mappings;
                VEthernetDatagramPortTable datagrams;
                ITransmissionPtr transmission;
                DeadlineTimerTable deadline_timers;
                std::shared_ptr<vmux::vmux_net> mux;
                IForwardingPtr forwarding;

                for (;;) {
                    SynchronizedObjectScope scope(syncobj_);
                    disposed_ = true;

                    mappings = std::move(mappings_);
                    mappings_.clear();

                    datagrams = std::move(datagrams_);
                    datagrams_.clear();

                    deadline_timers = std::move(deadline_timers_);
                    deadline_timers_.clear();
                    
                    mux_vlan_ = 0;
                    mux = std::move(mux_);
                    forwarding = std::move(forwarding_);
                    transmission = std::move(transmission_);
                    break;
                }

                StaticEchoClean();
                if (NULLPTR != transmission) {
                    LOG_DEBUG("VEthernetExchanger::Finalize: disposing transmission, disposed=%d", (int)disposed_);
                    transmission->Dispose();
                }

                for (auto&& [_, deadline_timer] : deadline_timers) {
                    ppp::net::Socket::Cancel(*deadline_timer);
                }

                Dictionary::ReleaseAllObjects(mappings);
                Dictionary::ReleaseAllObjects(datagrams);

                if (NULLPTR != mux) {
                    mux->close_exec();
                }
                if (NULLPTR != forwarding) {
                    forwarding->Dispose();
                }
            }

            void VEthernetExchanger::Dispose() noexcept {
                if (disposed_.exchange(true)) {
                    return;
                }

                LOG_DEBUG("VEthernetExchanger::Dispose: posting Finalize, disposed=1");
                auto self = shared_from_this();
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                boost::asio::post(*context, 
                    [self, this, context]() noexcept {
                        Finalize();
                    });
            }

            VEthernetExchanger::ITransmissionPtr VEthernetExchanger::NewTransmission(
                const ContextPtr&                                                   context,
                const StrandPtr&                                                    strand,
                const std::shared_ptr<boost::asio::ip::tcp::socket>&                socket,
                ProtocolType                                                        protocol_type,
                const ppp::string&                                                  host,
                const ppp::string&                                                  path) noexcept {

                ITransmissionPtr transmission;
                if (protocol_type == ProtocolType::ProtocolType_Http ||
                    protocol_type == ProtocolType::ProtocolType_WebSocket) {
                    transmission = NewWebsocketTransmission<IWebsocketTransmission>(context, strand, socket, host, path);
                }
                elif(protocol_type == ProtocolType::ProtocolType_HttpSSL ||
                    protocol_type == ProtocolType::ProtocolType_WebSocketSSL) {
                    transmission = NewWebsocketTransmission<ISslWebsocketTransmission>(context, strand, socket, host, path);
                }
                else {
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    transmission = make_shared_object<ITcpipTransmission>(context, strand, socket, configuration);
                }

                if (NULLPTR != transmission) {
                    transmission->QoS = switcher_->GetQoS();
                    transmission->Statistics = switcher_->GetStatistics();
                }
                
                return transmission;
            }

            std::shared_ptr<boost::asio::ip::tcp::socket> VEthernetExchanger::NewAsynchronousSocket(const ContextPtr& context, const StrandPtr& strand, const boost::asio::ip::tcp& protocol, ppp::coroutines::YieldContext& y) noexcept {
                if (disposed_) {
                    return NULLPTR;
                }

                if (!context) {
                    return NULLPTR;
                }

                std::shared_ptr<boost::asio::ip::tcp::socket> socket = strand ?
                    make_shared_object<boost::asio::ip::tcp::socket>(*strand) : make_shared_object<boost::asio::ip::tcp::socket>(*context);
                if (!socket) {
                    return NULLPTR;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (!configuration) {
                    return NULLPTR;
                }

                boost::system::error_code open_ec;
                if (!ppp::coroutines::asio::async_open(y, *socket, protocol, &open_ec)) {
                    LOG_DEBUG("VEthernetExchanger::NewAsynchronousSocket: async_open failed, protocol=%d, ec=%d, category=%s, message=%s",
                        protocol.family(), open_ec.value(), open_ec.category().name(), open_ec.message().data());
                    return NULLPTR;
                }

                Socket::SetWindowSizeIfNotZero(socket->native_handle(), configuration->tcp.cwnd, configuration->tcp.rwnd);
                Socket::AdjustSocketOptional(*socket, protocol == boost::asio::ip::tcp::v4(), configuration->tcp.fast_open, configuration->tcp.turbo);
                return socket;
            }

            bool VEthernetExchanger::GetRemoteEndPoint(YieldContext* y, ppp::string& hostname, ppp::string& address, ppp::string& path, int& port, ProtocolType& protocol_type, ppp::string& server, boost::asio::ip::tcp::endpoint& remoteEP) noexcept {
                if (disposed_) {
                    return false;
                }

                if (server_url_.port > IPEndPoint::MinPort && server_url_.port <= IPEndPoint::MaxPort) {
                    remoteEP      = server_url_.remoteEP;
                    hostname      = server_url_.hostname;
                    address       = server_url_.address;
                    path          = server_url_.path;
                    server        = server_url_.server;
                    port          = server_url_.port;
                    protocol_type = server_url_.protocol_type;
                    return true;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }

                ppp::string& client_server_string = configuration->client.server;
                if (client_server_string.empty()) {
                    return false;
                }

                // The primary keeps the traditional switcher-owned proxy. Each
                // secondary owns a forwarding proxy built from its own JSON.
                std::shared_ptr<ppp::transmissions::proxys::IForwarding> forwarding;
                if (primary_outbound_) {
                    forwarding = switcher_->GetForwarding();
                }
                else {
                    SynchronizedObjectScope scope(syncobj_);
                    forwarding = forwarding_;
                }
                if (NULLPTR != forwarding) {
                    ppp::string abs_url;
                    server = UriAuxiliary::Parse(client_server_string, hostname, address, path, port, protocol_type, &abs_url, *y, false);
                }
                else {
                    server = UriAuxiliary::Parse(client_server_string, hostname, address, path, port, protocol_type, *y);
                }

                if (server.empty()) {
                    return false;
                }

                if (hostname.empty()) {
                    return false;
                }

                if (NULLPTR != forwarding) {
                    boost::asio::ip::tcp::endpoint forwarding_to_endpoint = forwarding->GetLocalEndPoint();
                    if (int forwarding_to_port = forwarding_to_endpoint.port(); forwarding_to_port > IPEndPoint::MinPort && forwarding_to_port <= IPEndPoint::MaxPort) {
                        forwarding->SetRemoteEndPoint(hostname, port);
                        port = forwarding_to_port;
                        address = forwarding_to_endpoint.address().to_string();
                    }
                }

                if (address.empty()) {
                    return false;
                }

                if (port <= IPEndPoint::MinPort || port > IPEndPoint::MaxPort) {
                    return false;
                }

                IPEndPoint ipep(address.data(), port);
                if (IPEndPoint::IsInvalid(ipep)) {
                    return false;
                }

                remoteEP                  = IPEndPoint::ToEndPoint<boost::asio::ip::tcp>(ipep);
                server_url_.remoteEP      = remoteEP;
                server_url_.hostname      = hostname;
                server_url_.address       = address;
                server_url_.path          = path;
                server_url_.server        = server;
                server_url_.port          = port;
                server_url_.protocol_type = protocol_type;
                return true;
            }

            VEthernetExchanger::ITransmissionPtr VEthernetExchanger::OpenTransmission(const ContextPtr& context, const StrandPtr& strand, YieldContext& y) noexcept {
                boost::asio::ip::tcp::endpoint remoteEP;
                ppp::string hostname;
                ppp::string address;
                ppp::string path;
                ppp::string server;
                int port = IPEndPoint::MinPort;
                ProtocolType protocol_type = ProtocolType::ProtocolType_PPP;

                if (!GetRemoteEndPoint(y.GetPtr(), hostname, address, path, port, protocol_type, server, remoteEP)) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: GetRemoteEndPoint failed");
                    return NULLPTR;
                }

                boost::asio::ip::address remoteIP = remoteEP.address();
                if (IPEndPoint::IsInvalid(remoteIP)) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: invalid remote IP");
                    return NULLPTR;
                }

                int remotePort = remoteEP.port();
                if (remotePort <= IPEndPoint::MinPort || remotePort > IPEndPoint::MaxPort) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: invalid remote port=%d", remotePort);
                    return NULLPTR;
                }

#if defined(_WIN32)
                if (remoteIP.is_v4() && !remoteIP.is_loopback() &&
                    !switcher_->EnsureWindowsIPv4ServerRoute(remoteIP)) {
                    LOG_ERROR("VEthernetExchanger::OpenTransmission: cannot pin IPv4 server route, outbound=%s, remote=%s:%d",
                        outbound_tag_.data(), address.data(), remotePort);
                    return NULLPTR;
                }

                // The switcher intentionally suppresses the physical IPv6 default
                // route while the peer has not provided an IPv6 data plane. The
                // VPN transport itself is the exception: resolve it first, then pin
                // that exact /128 through the physical gateway before connecting.
                if (remoteIP.is_v6() && !remoteIP.is_loopback() &&
                    !switcher_->EnsureWindowsIPv6ServerRoute(remoteIP)) {
                    LOG_ERROR("VEthernetExchanger::OpenTransmission: cannot pin IPv6 server route, outbound=%s, remote=%s:%d",
                        outbound_tag_.data(), address.data(), remotePort);
                    return NULLPTR;
                }
#endif

                LOG_DEBUG("VEthernetExchanger::OpenTransmission: outbound=%s, transport_trace=%p, connecting to %s:%d, protocol=%d, hostname=%s, path=%s",
                    outbound_tag_.data(), strand.get(), address.data(), remotePort, (int)protocol_type, hostname.data(), path.data());

                std::shared_ptr<boost::asio::ip::tcp::socket> socket = NewAsynchronousSocket(context, strand, remoteEP.protocol(), y);
                if (!socket) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: NewAsynchronousSocket failed");
                    return NULLPTR;
                }

#if defined(_LINUX)
                // The VPN transport socket must bypass the VPN itself or it
                // loops back into the tunnel. On Android, VpnService claims
                // 0.0.0.0/0 AND ::/0 (IPv6 leak protection), and the ip rule
                // set hijacks any unmarked socket (fwmark 0x0/0x20000) back
                // into tun0. A server that is IPv6-only (e.g. a ppp://
                // endpoint on 2400::/12) previously skipped protect() here
                // because "VPN is IPv4", so the handshake packet entered the
                // tunnel, got re-dispatched as ordinary VPN traffic, and the
                // server was never reached - phase stayed "connected" (the
                // TUN exists) while every tunneled flow timed out. Protect
                // IPv4 and IPv6 alike.
                if (!remoteIP.is_loopback()) {
                    auto protector_network = switcher_->GetProtectorNetwork(); 
                    if (NULLPTR != protector_network) {
                        if (!protector_network->Protect(socket->native_handle(), y)) {
                            LOG_DEBUG("VEthernetExchanger::OpenTransmission: Protect failed (Linux)");
                            return NULLPTR;
                        }
                    }
                }
#elif defined(_WIN32)
                // Windows 不绑定物理网卡接口索引，依赖路由表防止环路。
                // 原版 openppp2_main 没有 IP_UNICAST_IF 也能正常工作，
                // 因为 AddRoute() 机制已经将 VPN 服务器 IP 通过物理网卡添加了特定路由。
                // 移除 IP_UNICAST_IF 避免物理网卡索引变化（WiFi 重连、睡眠唤醒等）导致断流。
#endif

                boost::system::error_code connect_ec;
                bool ok = ppp::coroutines::asio::async_connect(*socket, remoteEP, y, &connect_ec);
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::OpenTransmission: outbound=%s, transport_trace=%p, async_connect failed, remote=%s:%d, native=%lld, ec=%d, category=%s, message=%s",
                        outbound_tag_.data(), strand.get(), address.data(), remotePort, (long long)socket->native_handle(), connect_ec.value(),
                        connect_ec.category().name(), connect_ec.message().data());
                    return NULLPTR;
                }

                LOG_DEBUG("VEthernetExchanger::OpenTransmission: outbound=%s, transport_trace=%p, connected to %s:%d, creating transmission",
                    outbound_tag_.data(), strand.get(), address.data(), remotePort);
                return NewTransmission(context, strand, socket, protocol_type, hostname, path);
            }

            bool VEthernetExchanger::Open() noexcept {
                if (disposed_) {
                    return false;
                }

                AppConfigurationPtr configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }

                ContextPtr context = GetContext();
                if (!context) {
                    return false;
                }

                // A secondary configuration may use a different upstream proxy.
                // Failure is fatal for that outbound: silently connecting directly
                // would violate the selected route.
                if (!primary_outbound_ && !configuration->client.server_proxy.empty()) {
                    IForwardingPtr forwarding = make_shared_object<IForwarding>(context, configuration);
                    if (NULLPTR == forwarding || !forwarding->Open()) {
                        if (NULLPTR != forwarding) forwarding->Dispose();
                        LOG_ERROR("VEthernetExchanger::Open: cannot open forwarding proxy for outbound '%s'",
                            outbound_tag_.data());
                        return false;
                    }
#if defined(_LINUX)
                    forwarding->ProtectorNetwork = switcher_->GetProtectorNetwork();
#endif
                    SynchronizedObjectScope scope(syncobj_);
                    forwarding_ = std::move(forwarding);
                }

                auto self = shared_from_this();
                auto allocator = configuration->GetBufferAllocator();

                return YieldContext::Spawn(allocator.get(), *context,
                    [self, this, context](YieldContext& y) noexcept {
                        Loopback(context, y);
                    });
            }

            bool VEthernetExchanger::Update() noexcept {
                if (disposed_) {
                    return false;
                }

                auto self = shared_from_this();
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                boost::asio::post(*context, 
                    [self, this, context]() noexcept {
                        uint64_t now = ppp::threading::Executors::GetTickCount();
                        IForwardingPtr forwarding;
                        {
                            SynchronizedObjectScope scope(syncobj_);
                            forwarding = forwarding_;
                        }
                        if (NULLPTR != forwarding) {
                            forwarding->Update(now);
                        }
                        SendEchoKeepAlivePacket(now, false);
                        if (disposed_) {
                            return;
                        }
                        DoMuxEvents();
                        if (disposed_) {
                            return;
                        }
                        DoKeepAlived(GetTransmission(), now);

                        for (;;) {
                            SynchronizedObjectScope scope(syncobj_);
                            Dictionary::UpdateAllObjects(datagrams_, now);
                            Dictionary::UpdateAllObjects2(mappings_, now);
                            break;
                        }
                    });
                return true;
            }

#if defined(PPP_LOG_VERBOSE)
            void VEthernetExchanger::GetDebugObjectCounts(size_t& mappings, size_t& datagrams, size_t& timers) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                mappings = mappings_.size();
                datagrams = datagrams_.size();
                timers = deadline_timers_.size();
            }
#endif

            bool VEthernetExchanger::DoKeepAlived(const ITransmissionPtr& transmission, uint64_t now) noexcept {
                if (disposed_) {
                    return false;
                }
                
                NetworkState network_state = GetNetworkState();
                if (network_state != NetworkState_Established) {
                    return true;
                }

                if (VirtualEthernetLinklayer::DoKeepAlived(transmission, now)) {
                    return true;
                }

                LOG_DEBUG("VEthernetExchanger::DoKeepAlived: keepalive timeout, disposing transmission");
                IDisposable::Dispose(transmission);
                return false;
            }

            VEthernetExchanger::ITransmissionPtr VEthernetExchanger::ConnectTransmission(const ContextPtr& context, const StrandPtr& strand, YieldContext& y) noexcept {
                if (NULLPTR == context) {
                    return NULLPTR;
                }

                if (disposed_) {
                    return NULLPTR;
                }

                // VPN client A link can be created only after a link is established between the local switch and the remote VPN server.
                ITransmissionPtr owner_link = transmission_; 
                if (NULLPTR == owner_link) {
                    return NULLPTR;
                }

                ITransmissionPtr transmission = OpenTransmission(context, strand, y);
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                bool noerror = transmission->HandshakeServer(y, GetId(), false);
                {
                    auto logger = switcher_->GetLogger();
                    if (NULLPTR != logger) {
                        logger->Handshake(GetId(), transmission, "", "", "", noerror);
                    }
                }
                if (noerror) {
                    return transmission;
                }
                else {
                    transmission->Dispose();
                    return NULLPTR;
                }
            }

            bool VEthernetExchanger::AcquireActiveTransmission(const ContextPtr& context, YieldContext& y) noexcept {
                return true;
            }

            void VEthernetExchanger::ReleaseActiveTransmission() noexcept {
            }

#if defined(_ANDROID)
            bool VEthernetExchanger::AwaitJniAttachThread(const ContextPtr& context, YieldContext& y) noexcept {
                // On the Android platform, when the VPN tunnel transport layer is enabled, 
                // Ensure that the JVM thread has been attached to the PPP. Otherwise, the link cannot be protected, 
                // Resulting in loop problems and VPN loopback crashes.
                bool attach_ok = false;
                while (!disposed_) {
                    if (std::shared_ptr<ppp::net::ProtectorNetwork> protector = switcher_->GetProtectorNetwork(); NULLPTR != protector) {
                        if (NULLPTR != protector->GetContext() && NULLPTR != protector->GetEnvironment()) {
                            attach_ok = true;
                            break;
                        }
                    }

                    bool sleep_ok = Sleep(10, context, y); // Poll.
                    if (!sleep_ok) {
                        break;
                    }
                }

                return attach_ok;
            }
#endif

            bool VEthernetExchanger::Loopback(const ContextPtr& context, YieldContext& y) noexcept {
                AppConfigurationPtr configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }
#if defined(_ANDROID)
                elif(!AwaitJniAttachThread(context, y)) {
                    return false;
                }
#endif
                bool run_once = false;
                while (!disposed_) {
                    ExchangeToConnectingState(); {
                        LOG_DEBUG("VEthernetExchanger::Loopback: connecting to server...");
                        ITransmissionPtr transmission = OpenTransmission(context, y);
                        if (transmission) {
                            LOG_DEBUG("VEthernetExchanger::Loopback: TCP connected, starting handshake...");
                            bool handshake_ok = transmission->HandshakeServer(y, GetId(), true);
                            {
                                auto logger = switcher_->GetLogger();
                                if (NULLPTR != logger) {
                                    logger->Handshake(GetId(), transmission, "", "", "", handshake_ok);
                                }
                            }
                            LOG_DEBUG("VEthernetExchanger::Loopback: handshake %s", handshake_ok ? "success" : "failed");
                            if (handshake_ok && EchoLanToRemoteExchanger(transmission, y) > -1) {
                                LOG_DEBUG("VEthernetExchanger::Loopback: link established, entering data loop");
                                ExchangeToEstablishState(); {
                                    transmission_ = transmission; {
                                        SendRequestedPeerRouteAnnounce(transmission, y);
                                        RegisterAllMappingPorts();
                                        if (StaticEchoAllocatedToRemoteExchanger(y) && Run(transmission, y)) {
                                            run_once = true;
                                            StaticEchoClean();
                                        }

                                        UnregisterAllMappingPorts();
                                    }
                                    transmission_.reset();
                                }
                                LOG_DEBUG("VEthernetExchanger::Loopback: data loop exited, reconnecting...");
                            }
                            else {
                                LOG_DEBUG("VEthernetExchanger::Loopback: handshake or LAN exchange failed");
                            }

                            transmission->Dispose();
                        }
                        else {
                            LOG_DEBUG("VEthernetExchanger::Loopback: OpenTransmission failed");
                        }
                    } ExchangeToReconnectingState();

                    // Dispose can race with the data loop ending. Do not create a new
                    // reconnect wait after shutdown has already begun; Finalize will
                    // cancel any timers that were active before Dispose.
                    if (disposed_) {
                        break;
                    }

                    int64_t reconnection_timeout = static_cast<int64_t>(configuration->client.reconnections.timeout) * 1000;
                    LOG_DEBUG("VEthernetExchanger::Loopback: waiting %lld ms before reconnection attempt #%d", reconnection_timeout, (int)reconnection_count_);
                    if (!Sleep(reconnection_timeout, context, y)) {
                        break;
                    }
                }
                return run_once;
            }

            bool VEthernetExchanger::DoMuxEvents() noexcept {
                bool successes = false;
                while (!disposed_) {
                    uint16_t max_connections = switcher_->mux_;
                    if (max_connections == 0) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: mux disabled by switcher");
                        break;
                    }

                    if (network_state_.load() != NetworkState_Established) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: network not established, state=%d", (int)network_state_.load());
                        break;
                    }

                    AppConfigurationPtr configuration = GetConfiguration();
                    if (NULLPTR == configuration) {
                        LOG_DEBUG("VEthernetExchanger::DoMuxEvents: configuration missing");
                        break;
                    }

                    std::shared_ptr<vmux::vmux_net> mux = mux_;
                    if (NULLPTR != mux) {
                        bool breaking = true;
                        successes = true;

                        if (mux->Vlan != mux_vlan_) {
                            LOG_DEBUG("VEthernetExchanger::DoMuxEvents: VLAN mismatch, mux_vlan=%u, expected=%u, closing", mux->Vlan, mux_vlan_);
                            mux->close_exec();
                        }
                        elif(!mux->update()) {
                            int64_t reconnection_timeout = static_cast<int64_t>(configuration->client.reconnections.timeout) * 1000;
                            uint64_t mux_last = mux->get_last();

                            uint64_t now = mux->now_tick();
                            LOG_DEBUG("VEthernetExchanger::DoMuxEvents: mux update failed, last=%llu, now=%llu, timeout=%lld",
                                (unsigned long long)mux_last, (unsigned long long)now, (long long)reconnection_timeout);
                            if (now >= (mux_last + (uint64_t)reconnection_timeout)) {
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: mux reconnection timeout reached, resetting mux");
                                mux_.reset();
                                breaking = false;
                            }

                            mux->close_exec();
                        }

                        if (breaking) {
                            LOG_DEBUG("VEthernetExchanger::DoMuxEvents: keeping existing mux, state=%d, established=%d, disposed=%d",
                                (int)GetMuxNetworkState(), (int)mux->is_established(), (int)mux->is_disposed());
                            if (mux->is_established()) {
                                int grow = mux->take_turbo_pending_grow();
                                if (grow > 0) {
                                    MuxGrowLinklayers(switcher_->GetBufferAllocator(), mux, grow);
                                }
                            }
                            break;
                        }
                    }

                    ppp::threading::Executors::StrandPtr vmux_strand;
                    ppp::threading::Executors::ContextPtr vmux_context = ppp::threading::Executors::SelectScheduler(vmux_strand);
                    if (NULLPTR == vmux_context) {
                        break;
                    }
                    else {
                        vmux::vmux_net::mux_mode mux_mode = vmux::vmux_net::parse_mode(configuration->GetEffectiveMuxMode());
                        mux = make_shared_object<vmux::vmux_net>(vmux_context, vmux_strand, max_connections, false,
                            (switcher_->mux_acceleration_ & PPP_MUX_ACCELERATION_LOCAL) != 0, mux_mode);
                        if (NULLPTR == mux) {
                            break;
                        }

                        if (mux_mode == vmux::vmux_net::mux_mode_flow && configuration->mux.turbo) {
                            uint32_t hard = static_cast<uint32_t>(max_connections) * static_cast<uint32_t>(PPP_MUX_TURBO_FACTOR_MAX);
                            mux->set_pool_hard_max(static_cast<uint16_t>(std::min<uint32_t>(hard, UINT16_MAX)));
                        }
                    }

                    ITransmissionPtr vnet_transmission = GetTransmission();
                    if (NULLPTR == vnet_transmission) {
                        break;
                    }

                    ppp::threading::Executors::ContextPtr vnet_context = GetContext();
                    if (NULLPTR == vnet_context) {
                        break;
                    }

                    std::shared_ptr<ppp::threading::BufferswapAllocator> buffer_allocator = switcher_->GetBufferAllocator();
                    mux->AppConfiguration = configuration;
                    mux->BufferAllocator  = buffer_allocator;
#if defined(_LINUX)
                    mux->ProtectorNetwork = switcher_->GetProtectorNetwork();
#endif

                    for (;;) {
                        uint16_t vlan = (uint16_t)vmux::vmux_net::ftt_random_aid(1, UINT16_MAX);
                        if (vlan != 0 && vlan != mux_vlan_) {
                            mux_vlan_ = vlan;
                            mux->Vlan = vlan;
                            break;
                        }
                    }

                    LOG_DEBUG("VEthernetExchanger::DoMuxEvents: creating new mux, vlan=%u, max_connections=%u", mux->Vlan, max_connections);
                    std::shared_ptr<VirtualEthernetLinklayer> self = shared_from_this();
                    mux_ = mux;

                    successes = YieldContext::Spawn(buffer_allocator.get(), *vnet_context, 
                        [self, this, vnet_transmission, mux, vnet_context, configuration](YieldContext& y) noexcept {
                            bool ok = false;
                            if (!disposed_) {
                                uint16_t max_connections = mux->get_max_connections();
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: sending mux request, vlan=%u, max_connections=%u, acceleration=%d",
                                    mux->Vlan, max_connections, (int)((switcher_->mux_acceleration_ & PPP_MUX_ACCELERATION_REMOTE) != 0));
                                bool advertise_flow_v2 = vmux::vmux_net::mode_requires_flow_v2(
                                    mux->get_mode(), configuration->mux.turbo);
                                Byte ordering_caps = advertise_flow_v2
                                    ? static_cast<Byte>(vmux::vmux_net::ordering_caps_flow_v2)
                                    : static_cast<Byte>(0);
                                ok = DoMux(vnet_transmission, mux->Vlan, max_connections,
                                    (switcher_->mux_acceleration_ & PPP_MUX_ACCELERATION_REMOTE) != 0,
                                    ordering_caps, y);
                            }

                            if (!ok) {
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: DoMux failed, closing mux");
                                mux->close_exec();
                            }
                            else {
                                LOG_DEBUG("VEthernetExchanger::DoMuxEvents: DoMux succeeded");
                            }
                        });
                    break;
                }

                if (!successes) {
                    LOG_DEBUG("VEthernetExchanger::DoMuxEvents: no mux active, cleaning up");
                    std::shared_ptr<vmux::vmux_net> mux = std::move(mux_);
                    if (NULLPTR != mux) {
                        mux->close_exec();
                    }
                }

                return successes;
            }

            VEthernetExchanger::NetworkState VEthernetExchanger::GetMuxNetworkState() noexcept {
                if (disposed_) {
                    return NetworkState_Reconnecting;
                }

                std::shared_ptr<vmux::vmux_net> mux = mux_;
                if (NULLPTR == mux) {
                    return NetworkState_Connecting;
                }

                if (mux->is_disposed()) {
                    return NetworkState_Reconnecting;
                }

                if (mux->is_established()) {
                    return NetworkState_Established;
                }

                return NetworkState_Connecting;
            }

            bool VEthernetExchanger::MuxConnectAllLinklayers(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator, const std::shared_ptr<vmux::vmux_net>& mux) noexcept {
                using ppp::app::protocol::VirtualEthernetTcpipConnection;
                
                std::shared_ptr<boost::asio::io_context> context = mux->get_context();
                if (NULLPTR == context) {
                    return false;
                }

                auto self = shared_from_this();
                auto strand = mux->get_strand();

                return YieldContext::Spawn(allocator.get(), *context, strand.get(),
                    [self, this, mux, context, strand](YieldContext& y) noexcept -> bool {
                        if (disposed_ || mux != mux_) {
                            mux->close_exec();
                            return false;
                        }

                        int max_connections = mux->get_max_connections();
                        int bok_connections = 0;

                        const uint32_t& tx_seq = mux->get_tx_seq();
                        const uint32_t& rx_ack = mux->get_rx_ack();
                        if (!mux->ftt(vmux::vmux_net::ftt_random_aid(1, INT32_MAX), vmux::vmux_net::ftt_random_aid(1, INT32_MAX))) {
                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: ftt failed");
                            mux->close_exec();
                            return false;
                        }

                        auto context = mux->get_context();
                        auto strand = mux->get_strand();
                        
                        LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: connecting %d linklayers, vlan=%u", max_connections, mux->Vlan);
                        for (int i = 0; i < max_connections; i++) {
                            if (disposed_ || mux != mux_) {
                                bok_connections = -1;
                                break;
                            }

                            if (mux->is_established()) {
                                LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: mux already established at connection %d/%d", i, max_connections);
                                return true;
                            }

                            ITransmissionPtr transmission = ConnectTransmission(context, strand, y);
                            if (NULLPTR == transmission) {
                                LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: ConnectTransmission failed at %d/%d", i, max_connections);
                                break;
                            }

                            std::shared_ptr<boost::asio::ip::tcp::socket> default_socket;
                            std::shared_ptr<VirtualEthernetTcpipConnection> connection =
                                make_shared_object<VirtualEthernetTcpipConnection>(
                                    mux->AppConfiguration, context, strand, GetId(), default_socket);
                            if (NULLPTR == connection) {
                                LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: failed to create connection at %d/%d", i, max_connections);
                                break;
                            }

                            // In this lightweight and simple vmux circuit switch, seq and ack are delivered by the client, and the server and client are opposite.
                            if (!connection->ConnectMux(y, transmission, mux->Vlan, rx_ack, tx_seq)) {
                                LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: ConnectMux failed at %d/%d", i, max_connections);
                                break;
                            }

                            bool bok = mux->do_yield(y,
                                [self, mux, connection]() noexcept -> bool {
                                    vmux::vmux_net::vmux_linklayer_ptr linklayer;
                                    vmux::vmux_net::vmux_native_add_linklayer_after_success_before_callback handling;
                                    return mux->add_linklayer(connection, linklayer, handling);
                                });

                            if (!bok) {
                                LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: add_linklayer failed at %d/%d", i, max_connections);
                                break;
                            }

                            bok_connections++;
                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: linklayer %d/%d connected", bok_connections, max_connections);
                        }

                        if (bok_connections >= max_connections) {
                            LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: all %d linklayers connected successfully", max_connections);
                            return true;
                        }

                        LOG_DEBUG("VEthernetExchanger::MuxConnectAllLinklayers: only %d/%d connected, closing mux", bok_connections, max_connections);
                        mux->close_exec();
                        return false;
                    });
            }

            bool VEthernetExchanger::MuxGrowLinklayers(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator, const std::shared_ptr<vmux::vmux_net>& mux, int count) noexcept {
                using ppp::app::protocol::VirtualEthernetTcpipConnection;

                if (NULLPTR == mux || count <= 0) {
                    return false;
                }

                std::shared_ptr<boost::asio::io_context> context = mux->get_context();
                if (NULLPTR == context) {
                    return false;
                }

                auto self = shared_from_this();
                auto strand = mux->get_strand();
                return YieldContext::Spawn(allocator.get(), *context, strand.get(),
                    [self, this, mux, count, context, strand](YieldContext& y) noexcept -> bool {
                        const uint32_t& tx_seq = mux->get_tx_seq();
                        const uint32_t& rx_ack = mux->get_rx_ack();

                        for (int i = 0; i < count; i++) {
                            if (disposed_ || mux != mux_ || mux->is_disposed()) {
                                break;
                            }

                            ITransmissionPtr transmission = ConnectTransmission(context, strand, y);
                            if (NULLPTR == transmission) {
                                break;
                            }

                            std::shared_ptr<boost::asio::ip::tcp::socket> default_socket;
                            std::shared_ptr<VirtualEthernetTcpipConnection> connection =
                                make_shared_object<VirtualEthernetTcpipConnection>(
                                    mux->AppConfiguration, context, strand, GetId(), default_socket);
                            if (NULLPTR == connection) {
                                break;
                            }

                            if (!connection->ConnectMux(y, transmission, mux->Vlan, rx_ack, tx_seq)) {
                                break;
                            }

                            bool added = mux->do_yield(y,
                                [self, mux, connection]() noexcept -> bool {
                                    vmux::vmux_net::vmux_linklayer_ptr linklayer;
                                    vmux::vmux_net::vmux_native_add_linklayer_after_success_before_callback handling;
                                    return mux->add_linklayer(connection, linklayer, handling);
                                });
                            if (!added) {
                                break;
                            }
                        }

                        // Runtime growth is best-effort. The established base pool
                        // remains usable when an extra carrier cannot be created.
                        return true;
                    });
            }

            bool VEthernetExchanger::ReleaseDeadlineTimer(const boost::asio::deadline_timer* deadline_timer) noexcept {
                if (NULLPTR == deadline_timer) {
                    return false;
                }

                DeadlineTimerPtr reference;
                for (;;) {
                    SynchronizedObjectScope scope(syncobj_);
                    Dictionary::TryRemove(deadline_timers_, (void*)deadline_timer, reference);
                    break;
                }

                if (NULLPTR == reference) {
                    return false;
                }

                Socket::Cancel(*reference);
                return true;
            }

            bool VEthernetExchanger::NewDeadlineTimer(const ContextPtr& context, int64_t timeout, const ppp::function<void(bool)>& event) noexcept {
                std::shared_ptr<boost::asio::deadline_timer> t = make_shared_object<boost::asio::deadline_timer>(*context);
                if (NULLPTR == t) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                if (disposed_) {
                    return false;
                }
                else {
                    timeout = std::max<int64_t>(1, timeout);
                }

                auto self = shared_from_this();
                boost::asio::deadline_timer* deadline_timer = t.get();

                t->expires_from_now(Timer::DurationTime(timeout));
                t->async_wait(
                    [self, this, deadline_timer, event](const boost::system::error_code& ec) noexcept {
                        ReleaseDeadlineTimer(deadline_timer);
                        event(ec == boost::system::errc::success);
                    });

                auto r = deadline_timers_.emplace(deadline_timer, std::move(t));
                if (r.second) {
                    return true;
                }

                Socket::Cancel(*t);
                return false;
            }

            void VEthernetExchanger::ExchangeToEstablishState() noexcept {
                uint64_t now = Executors::GetTickCount();
                sekap_last_ = now;
                sekap_next_ = now + RandomNext(SEND_ECHO_KEEP_ALIVE_PACKET_MIN_TIMEOUT, SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT);
                network_state_.exchange(NetworkState_Established);
                reconnection_count_ = 0;
            }

            void VEthernetExchanger::ExchangeToConnectingState() noexcept {
                sekap_last_ = 0;
                sekap_next_ = 0;
                network_state_.exchange(NetworkState_Connecting);
            }

            void VEthernetExchanger::ExchangeToReconnectingState() noexcept {
                sekap_last_ = 0;
                sekap_next_ = 0;
                network_state_.exchange(NetworkState_Reconnecting);
                reconnection_count_++;
            }

            bool VEthernetExchanger::RegisterAllMappingPorts() noexcept {
                if (disposed_) {
                    return false;
                }

                AppConfigurationPtr configuration = GetConfiguration();
                for (AppConfiguration::MappingConfiguration& mapping : configuration->client.mappings) {
                    RegisterMappingPort(mapping);
                }

                return true;
            }

            void VEthernetExchanger::UnregisterAllMappingPorts() noexcept {
                VirtualEthernetMappingPortTable mappings; {
                    SynchronizedObjectScope scope(syncobj_);
                    mappings = std::move(mappings_);
                    mappings_.clear();
                }

                ppp::collections::Dictionary::ReleaseAllObjects(mappings);
            }

            void VEthernetExchanger::ResetDataChannels() noexcept {
                VirtualEthernetMappingPortTable mappings;
                VEthernetDatagramPortTable datagrams;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    mappings = std::move(mappings_);
                    mappings_.clear();
                    datagrams = std::move(datagrams_);
                    datagrams_.clear();
                }
                Dictionary::ReleaseAllObjects(mappings);
                Dictionary::ReleaseAllObjects(datagrams);
                LOG_INFO("VEthernetExchanger::ResetDataChannels: outbound=%s, tcp=%llu, udp=%llu",
                    outbound_tag_.data(),
                    (unsigned long long)mappings.size(),
                    (unsigned long long)datagrams.size());
            }

            bool VEthernetExchanger::OnLan(const ITransmissionPtr& transmission, uint32_t ip, uint32_t mask, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnNat(const ITransmissionPtr& transmission, Byte* packet, int packet_length, YieldContext& y) noexcept {
                bool vnet = switcher_->IsVNet();
                // Android keeps vnet=false but still forwards IPv6: the
                // inbound (server -> TUN) translation below only rewrites
                // IPv6 headers, and IPv4 never reaches OnNat unless vnet is
                // enabled. TranslateIPv6Packet() passes non-IPv6 packets
                // through untouched, so the vnet branch below is unchanged.
                bool is_ipv6 = NULLPTR != packet && packet_length >= ppp::ipv6::IPv6_HEADER_MIN_SIZE &&
                    (packet[0] >> 4) == ppp::ipv6::IPv6_VERSION;
                if (vnet || is_ipv6) {
                    // Peer gateway forward gate: unless client.peer-gateway-forward
                    // is enabled, inbound packets must be addressed to this host's
                    // own virtual address.  Otherwise a malicious peer could use
                    // this client as an open relay through the gateway session.
                    AppConfigurationPtr configuration = GetConfiguration();
                    std::shared_ptr<ppp::tap::ITap> tap = switcher_->GetTap();
                    if (NULLPTR != configuration && NULLPTR != tap && !configuration->client.peer_gateway_forward) {
                        bool destination_matches = false;
                        const Byte version = packet[0] >> 4;
                        if (version == ppp::net::native::ip_hdr::IP_VER) {
                            int ip_length = packet_length;
                            ppp::net::native::ip_hdr* ip = ppp::net::native::ip_hdr::Parse(packet, ip_length);
                            destination_matches = NULLPTR != ip && ip->dest == tap->IPAddress;
                        }
                        elif (version == ppp::ipv6::IPv6_VERSION) {
                            boost::asio::ip::address_v6 source;
                            boost::asio::ip::address_v6 destination;
                            boost::asio::ip::address assigned = assigned_ipv6_address_;
                            destination_matches = assigned.is_v6() &&
                                ppp::ipv6::TryParsePacket(packet, packet_length, source, destination) &&
                                destination == assigned.to_v6();
                        }

                        if (!destination_matches) {
                            ppp::telemetry::Log(ppp::telemetry::Level::kInfo, "client_exchanger", "peer gateway forward rejected");
                            ppp::telemetry::Count("client.peer_gateway_forward.rejected", 1);
                            return false;
                        }
                    }
                    if (!TranslateIPv6Packet(packet, packet_length, false)) return false;
                    return switcher_->Output(packet, packet_length);
                }
                else {
                    return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
                }
            }

            /** @brief Sends the configured peer prefix gateway announcement to the server. */
            bool VEthernetExchanger::SendRequestedPeerRouteAnnounce(const ITransmissionPtr& transmission, YieldContext& y) noexcept {
                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR == configuration || NULLPTR == transmission) {
                    return false;
                }

                InformationEnvelope envelope;
                if (!configuration->client.peer_route_announce.empty()) {
                    envelope.Extensions.PeerRouteAnnounce.enabled = true;
                    envelope.Extensions.PeerRouteAnnounce.action = "register";
                    for (const auto& item : configuration->client.peer_route_announce) {
                        ppp::app::protocol::PeerPrefixRouteEntry entry;
                        entry.network = item.network;
                        entry.prefix = item.prefix;
                        envelope.Extensions.PeerRouteAnnounce.prefixes.emplace_back(entry);
                    }
                }

                if (!envelope.Extensions.HasAny()) {
                    return false;
                }

                envelope.ExtendedJson = envelope.Extensions.ToJson();
                return DoInformation(transmission, envelope, y);
            }

            bool VEthernetExchanger::OnMux(const ITransmissionPtr& transmission, uint16_t vlan, uint16_t max_connections, bool acceleration, Byte ordering_caps, YieldContext& y) noexcept {
                LOG_DEBUG("VEthernetExchanger::OnMux: vlan=%u, max_connections=%u, acceleration=%d, ordering_caps=%u",
                    vlan, max_connections, (int)acceleration, (unsigned int)ordering_caps);
                std::shared_ptr<vmux::vmux_net> mux = mux_;
                if (NULLPTR != mux) {
                    bool successed = false;
                    if (vlan != 0 && max_connections > 0 && mux->Vlan == vlan && max_connections == mux->get_max_connections() && !mux->is_disposed()) {
                        bool established = mux->is_established();
                        successed = true;

                        if (!established) {
                            auto configuration = GetConfiguration();
                            auto allocator = configuration->GetBufferAllocator();

                            bool local_supports_flow_v2 = vmux::vmux_net::mode_requires_flow_v2(
                                mux->get_mode(), configuration->mux.turbo);
                            bool agreed_flow_v2 = local_supports_flow_v2 &&
                                ((ordering_caps & vmux::vmux_net::ordering_caps_flow_v2) != 0);
                            mux->set_ordering_mode(agreed_flow_v2
                                ? vmux::vmux_net::ordering_flow_v2
                                : vmux::vmux_net::ordering_compat);

                            successed = MuxConnectAllLinklayers(allocator, mux);
                        }
                    }
                    
                    if (!successed) {
                        LOG_DEBUG("VEthernetExchanger::OnMux: mux rejected, local_vlan=%u, local_max=%u, disposed=%d",
                            mux->Vlan, mux->get_max_connections(), (int)mux->is_disposed());
                        mux->close_exec();
                    }
                }
                else {
                    LOG_DEBUG("VEthernetExchanger::OnMux: no local mux exists");
                }

                return true;
            }

            bool VEthernetExchanger::OnInformation(const ITransmissionPtr& transmission, const VirtualEthernetInformation& information, YieldContext& y) noexcept {
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                auto ei = make_shared_object<VirtualEthernetInformation>(information);
                if (NULLPTR == ei) {
                    return false;
                }
                
                auto self = shared_from_this();
                boost::asio::post(*context, 
                    [self, this, context, ei]() noexcept {
                        information_ = ei;
                        if (!disposed_) {
                            if (!primary_outbound_) {
                                SynchronizedObjectScope scope(syncobj_);
                                assigned_ipv6_address_ = boost::asio::ip::address();
                            }
                            if (primary_outbound_) {
                                switcher_->OnInformation(ei);
                            }
                            elif(!ei->Valid()) {
                                if (ITransmissionPtr transmission = transmission_; NULLPTR != transmission) {
                                    transmission->Dispose();
                                }
                            }
                        }
                    });
                return true;
            }

            bool VEthernetExchanger::OnInformation(const ITransmissionPtr& transmission, const InformationEnvelope& information, YieldContext& y) noexcept {
                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                // Process base information (quota/expiry) same as before
                auto ei = make_shared_object<VirtualEthernetInformation>(information.Base);
                if (NULLPTR == ei) {
                    return false;
                }

                auto self = shared_from_this();
                boost::asio::post(*context,
                    [self, this, context, ei, information]() noexcept {
                        information_ = ei;
                        if (!disposed_) {
                            {
                                // A reconnect may intentionally return no IPv6 lease,
                                // so an old per-outbound address must not survive it.
                                SynchronizedObjectScope scope(syncobj_);
                                assigned_ipv6_address_ = information.Extensions.AssignedIPv6Address.is_v6()
                                    ? information.Extensions.AssignedIPv6Address
                                    : boost::asio::ip::address();
                            }
                            if (primary_outbound_) {
                                switcher_->OnInformation(ei);
                            }
                            elif(!ei->Valid()) {
                                if (ITransmissionPtr transmission = transmission_; NULLPTR != transmission) {
                                    transmission->Dispose();
                                }
                            }

                            // Apply IPv6 (and optionally IPv4) assignment from ExtendedJson
                            LOG_DEBUG("VEthernetExchanger::OnInformation: outbound=%s, primary=%d, ExtendedJson.empty()=%d, len=%d, AssignedIPv6Mode=%d",
                                outbound_tag_.data(), (int)primary_outbound_,
                                (int)information.ExtendedJson.empty(),
                                (int)information.ExtendedJson.size(),
                                (int)information.Extensions.AssignedIPv6Mode);
                            if (primary_outbound_ && !information.ExtendedJson.empty()) {
                                switcher_->ApplyIPv6Assignment(information.Extensions);
                                switcher_->ApplyPeerPrefixRoutes(information.Extensions);
                            }
                        }
                    });
                return true;
            }

            bool VEthernetExchanger::OnPush(const ITransmissionPtr& transmission, int connection_id, Byte* packet, int packet_length, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnConnect(const ITransmissionPtr& transmission, int connection_id, const boost::asio::ip::tcp::endpoint& destinationEP, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnConnectOK(const ITransmissionPtr& transmission, int connection_id, Byte error_code, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnDisconnect(const ITransmissionPtr& transmission, int connection_id, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnStatic(const ITransmissionPtr& transmission, YieldContext& y) noexcept {
                return false; // Immediate return false and forcefully close the connection due to a suspected malicious attack on the client.
            }

            bool VEthernetExchanger::OnStatic(const ITransmissionPtr& transmission, Int128 fsid, int session_id, int remote_port, YieldContext& y) noexcept {                
                if (remote_port < IPEndPoint::MinPort || remote_port > IPEndPoint::MaxPort) {
                    return false;
                }

                if (session_id < 0) {
                    return false;
                }

                // If the server does not support static tunneling, clean up the pre-prepared resources.
                if (remote_port == IPEndPoint::MinPort || session_id == 0) {
                    StaticEchoClean();
                }
                else {
                    static_echo_session_id_ = session_id;
                    static_echo_remote_port_ = remote_port;

                    AppConfigurationPtr configuration = GetConfiguration();
                    VirtualEthernetPacket::Ciphertext(configuration, GetId(), fsid, session_id, static_echo_protocol_, static_echo_transport_);
                }

                StaticEchoGatewayServer(STATIC_ECHO_KEEP_ALIVED_ID);
                return true;
            }

            bool VEthernetExchanger::OnEcho(const ITransmissionPtr& transmission, int ack_id, YieldContext& y) noexcept {
                if (ack_id != 0) {
                    switcher_->ERORTE(ack_id);
                }
                
                return true;
            }

            bool VEthernetExchanger::OnEcho(const ITransmissionPtr& transmission, Byte* packet, int packet_length, YieldContext& y) noexcept {
                switcher_->Output(packet, packet_length);
                return true;
            }

            bool VEthernetExchanger::OnSendTo(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length, YieldContext& y) noexcept {
                ReceiveFromDestination(sourceEP, destinationEP, packet, packet_length);
                return true;
            }

            bool VEthernetExchanger::ReceiveFromDestination(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length) noexcept {
                if (disposed_) {
                    return false;
                }

                if (TryHandleDatagram(sourceEP, destinationEP, packet, packet_length)) {
                    return true;
                }

                VEthernetDatagramPortPtr datagram = GetDatagramPort(sourceEP);
                if (NULLPTR != datagram) {
                    if (NULLPTR != packet && packet_length > 0) {
                        datagram->OnMessage(packet, packet_length, destinationEP);
                    }
                    else {
                        datagram->MarkFinalize();
                        datagram->Dispose();
                    }
                }
                elif(NULLPTR != packet && packet_length > 0) {
                    switcher_->DatagramOutput(sourceEP, destinationEP, packet, packet_length);
                }

                return true;
            }

            bool VEthernetExchanger::SendTo(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, const void* packet, int packet_size) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                VEthernetDatagramPortPtr datagram = AddNewDatagramPort(transmission, sourceEP);
                if (NULLPTR == datagram) {
                    return false;
                }

                return datagram->SendTo(packet, packet_size, destinationEP);
            }

            bool VEthernetExchanger::RegisterDatagramHandler(const boost::asio::ip::udp::endpoint& sourceEP, const DatagramPacketHandler& handler) noexcept {
                if (sourceEP.port() <= ppp::net::IPEndPoint::MinPort) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                auto r = datagram_handlers_.emplace(sourceEP, handler);
                return r.second;
            }

            bool VEthernetExchanger::ReleaseDatagramHandler(const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                if (sourceEP.port() <= ppp::net::IPEndPoint::MinPort) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                auto r = datagram_handlers_.erase(sourceEP);
                return r > 0;
            }

            bool VEthernetExchanger::TryHandleDatagram(const boost::asio::ip::udp::endpoint& sourceEP, const boost::asio::ip::udp::endpoint& destinationEP, Byte* packet, int packet_length) noexcept {
                if (sourceEP.port() <= ppp::net::IPEndPoint::MinPort) {
                    return false;
                }

                DatagramPacketHandler handler;
                {
                    SynchronizedObjectScope scope(syncobj_);
                    auto tail = datagram_handlers_.find(sourceEP);
                    if (tail == datagram_handlers_.end()) {
                        return false;
                    }
                    handler = tail->second;
                }
                if (NULLPTR == handler) return false;
                // A handler may enter the switcher and later release/register an
                // exchanger handler.  Invoking it while syncobj_ is held creates a
                // lock inversion with the switcher's DNS request lock and can stall
                // every UDP response during a query burst.
                return handler(sourceEP, destinationEP, packet, packet_length);
            }

            bool VEthernetExchanger::Echo(int ack_id) noexcept {
                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                bool ok = DoEcho(transmission, ack_id, nullof<YieldContext>());
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::Echo(ack_id=%d): DoEcho failed, disposing transmission", ack_id);
                    transmission->Dispose();
                }

                return ok;
            }

            bool VEthernetExchanger::Echo(const void* packet, int packet_size) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                bool ok = DoEcho(transmission, (Byte*)packet, packet_size, nullof<YieldContext>());
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::Echo(packet): DoEcho failed, disposing transmission, packet_size=%d", packet_size);
                    transmission->Dispose();
                }

                return ok;
            }

            bool VEthernetExchanger::Nat(const void* packet, int packet_size) noexcept {
                if (NULLPTR == packet || packet_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                if (!TranslateIPv6Packet((Byte*)packet, packet_size, true)) {
                    return false;
                }

                bool ok = DoNat(transmission, (Byte*)packet, packet_size, nullof<YieldContext>());
                if (!ok) {
                    LOG_DEBUG("VEthernetExchanger::Nat: DoNat failed, disposing transmission, packet_size=%d", packet_size);
                    transmission->Dispose();
                }

                return ok;
            }

            int VEthernetExchanger::EchoLanToRemoteExchanger(const ITransmissionPtr& transmission, YieldContext& y) noexcept {
                if (disposed_) {
                    return -1;
                }

                bool vnet = switcher_->IsVNet();
                if (!vnet) {
                    return 0;
                }

                if (NULLPTR == transmission) {
                    return -1;
                }

                std::shared_ptr<ppp::tap::ITap> tap = switcher_->GetTap();
                if (NULLPTR == tap) {
                    return -1;
                }

                bool ok = DoLan(transmission, tap->IPAddress, tap->SubmaskAddress, y);
                if (ok) {
                    return 1;
                }

                LOG_DEBUG("VEthernetExchanger::EchoLanToRemoteExchanger: DoLan failed, disposing transmission");
                transmission->Dispose();
                return -1;
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::AddNewDatagramPort(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                VEthernetDatagramPortPtr datagram = GetDatagramPort(sourceEP);
                if (NULLPTR != datagram) {
                    return datagram;
                }

                if (disposed_) {
                    return NULLPTR;
                }

                bool ok = true; 
                datagram = NewDatagramPort(transmission, sourceEP);

                if (NULLPTR == datagram) {
                    return NULLPTR;
                }
                else {
                    SynchronizedObjectScope scope(syncobj_);
                    auto r = datagrams_.emplace(sourceEP, datagram);
                    ok = r.second;
                }

                if (!ok) {
                    datagram->Dispose();
                    return NULLPTR;
                }

                return datagram;
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::NewDatagramPort(const ITransmissionPtr& transmission, const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                auto my = shared_from_this();
                std::shared_ptr<VEthernetExchanger> exchanger 
                    = std::dynamic_pointer_cast<VEthernetExchanger>(my);

                return make_shared_object<VEthernetDatagramPort>(exchanger, transmission, sourceEP);
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::GetDatagramPort(const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return Dictionary::FindObjectByKey(datagrams_, sourceEP);
            }

            VEthernetExchanger::VEthernetDatagramPortPtr VEthernetExchanger::ReleaseDatagramPort(const boost::asio::ip::udp::endpoint& sourceEP) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return Dictionary::ReleaseObjectByKey(datagrams_, sourceEP);
            }

            bool VEthernetExchanger::SendEchoKeepAlivePacket(UInt64 now, bool immediately) noexcept {
                if (network_state_ != NetworkState_Established) {
                    return false;
                }

                UInt64 next = sekap_last_ + SEND_ECHO_KEEP_ALIVE_PACKET_MMX_TIMEOUT;
                if (now >= next) {
                    LOG_DEBUG("VEthernetExchanger::SendEchoKeepAlivePacket: echo timeout (%llu ms since last), disposing transmission",
                        (unsigned long long)(now - sekap_last_));
                    ITransmissionPtr transmission = transmission_;
                    if (transmission) {
                        transmission->Dispose();
                        return false;
                    }
                }

                if (!immediately) {
                    if (now < sekap_next_) {
                        return false;
                    }
                }

                sekap_next_ = now + RandomNext(SEND_ECHO_KEEP_ALIVE_PACKET_MIN_TIMEOUT, SEND_ECHO_KEEP_ALIVE_PACKET_MAX_TIMEOUT);
                return Echo(0);
            }

            bool VEthernetExchanger::PacketInput(const ITransmissionPtr& transmission, Byte* p, int packet_length, YieldContext& y) noexcept {
                bool successed = VirtualEthernetLinklayer::PacketInput(transmission, p, packet_length, y);
                if (successed) {
                    if (network_state_ == NetworkState_Established) {
                        sekap_last_ = Executors::GetTickCount();
                    }
                }

                return successed;
            }

            bool VEthernetExchanger::RegisterMappingPort(ppp::configurations::AppConfiguration::MappingConfiguration& mapping) noexcept {
                if (disposed_) {
                    return false;
                }

                boost::system::error_code ec;
                boost::asio::ip::address local_ip = StringToAddress(mapping.local_ip.data(), ec);
                if (ec) {
                    return false;
                }

                boost::asio::ip::address remote_ip = StringToAddress(mapping.remote_ip.data(), ec);
                if (ec) {
                    return false;
                }

                bool in = remote_ip.is_v4();
                bool protocol_tcp_or_udp = mapping.protocol_tcp_or_udp;

                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, protocol_tcp_or_udp, mapping.remote_port);
                if (NULLPTR != mapping_port) {
                    return false;
                }

                mapping_port = NewMappingPort(in, protocol_tcp_or_udp, mapping.remote_port);
                if (NULLPTR == mapping_port) {
                    return false;
                }

                bool ok = mapping_port->OpenFrpClient(local_ip, mapping.local_port);
                if (ok) {
                    SynchronizedObjectScope scope(syncobj_);
                    ok = VirtualEthernetMappingPort::AddMappingPort(mappings_, in, protocol_tcp_or_udp, mapping.remote_port, mapping_port);
                }

                if (!ok) {
                    mapping_port->Dispose();
                }

                return ok;
            }

            VEthernetExchanger::VirtualEthernetMappingPortPtr VEthernetExchanger::NewMappingPort(bool in, bool tcp, int remote_port) noexcept {
                class VIRTUAL_ETHERNET_MAPPING_PORT final : public VirtualEthernetMappingPort {
                public:
                    VIRTUAL_ETHERNET_MAPPING_PORT(const std::shared_ptr<VirtualEthernetLinklayer>& linklayer, const ITransmissionPtr& transmission, bool tcp, bool in, int remote_port) noexcept
                        : VirtualEthernetMappingPort(linklayer, transmission, tcp, in, remote_port) {

                    }

                public:
                    virtual void Dispose() noexcept override {
                        if (std::shared_ptr<VirtualEthernetLinklayer> linklayer = GetLinklayer();  NULLPTR != linklayer) {
                            VEthernetExchanger* exchanger = dynamic_cast<VEthernetExchanger*>(linklayer.get());
                            if (NULLPTR != exchanger) {
                                SynchronizedObjectScope scope(exchanger->syncobj_);
                                VirtualEthernetMappingPort::DeleteMappingPort(
                                    exchanger->mappings_, ProtocolIsNetworkV4(), ProtocolIsTcpNetwork(), GetRemotePort());
                            }
                        }

                        VirtualEthernetMappingPort::Dispose();
                    }
                };

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return NULLPTR;
                }

                auto self = shared_from_this();
                return make_shared_object<VIRTUAL_ETHERNET_MAPPING_PORT>(self, transmission, tcp, in, remote_port);
            }

            VEthernetExchanger::VirtualEthernetMappingPortPtr VEthernetExchanger::GetMappingPort(bool in, bool tcp, int remote_port) noexcept {
                SynchronizedObjectScope scope(syncobj_);
                return VirtualEthernetMappingPort::FindMappingPort(mappings_, in, tcp, remote_port);
            }

            bool VEthernetExchanger::OnFrpSendTo(const ITransmissionPtr& transmission, bool in, int remote_port, const boost::asio::ip::udp::endpoint& sourceEP, Byte* packet, int packet_length, YieldContext& y) noexcept {
#if defined(_ANDROID)
                AppConfigurationPtr configuration = GetConfiguration();
                if (!configuration) {
                    return false;
                }

                std::shared_ptr<Byte> packet_managed = ppp::net::asio::IAsynchronousWriteIoQueue::Copy(configuration->GetBufferAllocator(), packet, packet_length);
                Post(
                    [this, packet_managed, sourceEP, packet_length, in, remote_port]() noexcept {
                        VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, false, remote_port);
                        if (NULLPTR != mapping_port) {
                            mapping_port->Client_OnFrpSendTo(packet_managed.get(), packet_length, sourceEP);
                        }
                    });
#else
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, false, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpSendTo(packet, packet_length, sourceEP);
                }
#endif
                return true;
            }

            bool VEthernetExchanger::OnFrpConnect(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port, YieldContext& y) noexcept {
#if defined(_ANDROID)
                Post(
                    [this, in, remote_port, connection_id]() noexcept {
                        VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                        if (NULLPTR != mapping_port) {
                            mapping_port->Client_OnFrpConnect(connection_id);
                        }
                    });
#else
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpConnect(connection_id);
                }
#endif
                return true;
            }

            bool VEthernetExchanger::OnFrpDisconnect(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port) noexcept {
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpDisconnect(connection_id);
                }

                return true;
            }

            bool VEthernetExchanger::OnFrpPush(const ITransmissionPtr& transmission, int connection_id, bool in, int remote_port, const void* packet, int packet_length) noexcept {
                VirtualEthernetMappingPortPtr mapping_port = GetMappingPort(in, true, remote_port);
                if (NULLPTR != mapping_port) {
                    mapping_port->Client_OnFrpPush(connection_id, packet, packet_length);
                }

                return true;
            }

            void VEthernetExchanger::StaticEchoClean() noexcept {
                for (int i = 0; i < arraysizeof(static_echo_sockets_); i++) {
                    std::shared_ptr<StaticEchoDatagarmSocket>& r = static_echo_sockets_[i];
                    std::shared_ptr<StaticEchoDatagarmSocket> socket = std::move(r);
  
                    Socket::Closesocket(socket);
                }
                
                static_echo_input_       = false;
                static_echo_timeout_     = UINT64_MAX;
                static_echo_session_id_  = 0;
                static_echo_remote_port_ = IPEndPoint::MinPort;

                static_echo_protocol_    = NULLPTR;
                static_echo_transport_   = NULLPTR;
            }

            bool VEthernetExchanger::StaticEchoAllocated() noexcept {
                if (disposed_) {
                    return false;
                }

                std::shared_ptr<StaticEchoDatagarmSocket> socket = static_echo_sockets_[0];
                if (NULLPTR == socket) {
                    return false;
                }

                return socket->is_open() && static_echo_timeout_ != 0 && static_echo_session_id_ != 0 && static_echo_remote_port_ != 0;
            }

            bool VEthernetExchanger::StaticEchoSwapAsynchronousSocket() noexcept {
                if (disposed_) {
                    return false;
                }

                if (static_echo_timeout_ != UINT64_MAX && switcher_->StaticMode(NULLPTR)) {
                    UInt64 now = ppp::threading::Executors::GetTickCount();
                    if (now >= static_echo_timeout_) {
                        if (static_echo_input_) {
                            static_echo_input_ = false;
                            return StaticEchoNextTimeout();
                        }

                        std::shared_ptr<StaticEchoDatagarmSocket> socket = std::move(static_echo_sockets_[0]);
                        static_echo_sockets_[0] = std::move(static_echo_sockets_[1]);
                        static_echo_sockets_[1] = NULLPTR;
                        
                        static_echo_input_ = false;
                        if (!StaticEchoNextTimeout()) {
                            return false;
                        }

                        auto self = shared_from_this();
                        auto notifiy_if_need = 
                            [self, this]() noexcept {
                                // Notifies the VPN server of domestic port changes for smoother dynamic switchover of virtual links.
                                if (!static_echo_input_ && static_echo_sockets_[0]) {
                                    StaticEchoGatewayServer(STATIC_ECHO_KEEP_ALIVED_ID);
                                }
                            };
                        
                        // Here do not close the socket immediately, delay one second, because the data sent by the VPN server may not reach the network card, 
                        // Reduce the packet loss rate during switching and improve the smoothness of the cross.
                        bool closesocket = true;
                        std::shared_ptr<boost::asio::io_context> context = GetContext();
                        if (NULLPTR != context) {
                            int milliseconds = RandomNext(500, 1000);
                            std::shared_ptr<Timer> timeout = Timer::Timeout(context, milliseconds, 
                                [socket, notifiy_if_need](Timer*) noexcept {
                                    notifiy_if_need();
                                    Socket::Closesocket(socket);
                                });
                            if (NULLPTR != timeout) {
                                closesocket = false;
                            }
                        }

                        // Handles whether you can delay closing the socket. If not, close the socket immediately.
                        if (closesocket) {
                            Socket::Closesocket(socket);
                        }

                        notifiy_if_need();
                        if (NULLPTR == context) {
                            return false;
                        }

                        // Re-instance and try to open the Datagram Port.
                        socket = make_shared_object<StaticEchoDatagarmSocket>(*context);
                        if (NULLPTR == socket) {
                            return false;
                        }

                        auto configuration = GetConfiguration();
                        auto allocator = configuration->GetBufferAllocator();
                        static_echo_sockets_[1] = socket;

                        return YieldContext::Spawn(allocator.get(), *context,
                            [self, this, socket, context](YieldContext& y) noexcept {
                                bool opened = StaticEchoOpenAsynchronousSocket(*socket, y);
                                if (opened) {
                                    StaticEchoLoopbackSocket(socket);
                                }
                            });
                    }
                }

                return true;
            }

            bool VEthernetExchanger::StaticEchoGatewayServer(int ack_id) noexcept {
                if (disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::net::packet::IPFrame> packet = make_shared_object<ppp::net::packet::IPFrame>(); 
                if (NULLPTR == packet) {
                    return false;
                }

                packet->AddressesFamily = AddressFamily::InterNetwork;
                packet->Destination     = htonl(ack_id);
                packet->Id              = ppp::net::packet::IPFrame::NewId();
                packet->Source          = IPEndPoint::LoopbackAddress;
                packet->ProtocolType    = ppp::net::native::ip_hdr::IP_PROTO_ICMP;
                ppp::app::protocol::VirtualEthernetPacket::FillBytesToPayload(packet.get());
            
                return StaticEchoPacketToRemoteExchanger(packet.get());
            }

            bool VEthernetExchanger::StaticEchoAllocatedToRemoteExchanger(YieldContext& y) noexcept {
                StaticEchoClean();
                if (disposed_) {
                    return false;
                }

                if (StaticEchoAllocated()) {
                    return true;
                }

                std::shared_ptr<boost::asio::io_context> context = GetContext();
                if (NULLPTR == context) {
                    return false;
                }

                bool static_mode = switcher_->StaticMode(NULLPTR);
                if (!static_mode) {
                    return true;
                }

                for (int i = 0; i < arraysizeof(static_echo_sockets_); i++) {
                    std::shared_ptr<StaticEchoDatagarmSocket>& socket = static_echo_sockets_[i];
                    if (NULLPTR == socket) {
                        socket = make_shared_object<StaticEchoDatagarmSocket>(*context);
                        if (NULLPTR == socket) {
                            return false;
                        }
                    }

                    if (socket->is_open(true)) {
                        continue;
                    }

                    bool opened = StaticEchoOpenAsynchronousSocket(*socket, y) && StaticEchoLoopbackSocket(socket);
                    if (!opened) {
                        socket.reset();
                        return false;
                    }
                }

                ITransmissionPtr transmission = GetTransmission();
                if (NULLPTR == transmission) {
                    return false;
                }

                return DoStatic(transmission, y);
            }

            bool VEthernetExchanger::StaticEchoNextTimeout() noexcept {
                if (disposed_) {
                    return false;
                }

                std::shared_ptr<StaticEchoDatagarmSocket> socket = static_echo_sockets_[0];
                if (NULLPTR == socket) {
                    return false;
                }

                bool opened = socket->is_open(true);
                if (!opened) {
                    return false;
                }

                AppConfigurationPtr configuration = GetConfiguration();
                int min = std::max<int>(0, configuration->udp.static_.keep_alived[0]);
                int max = std::max<int>(0, configuration->udp.static_.keep_alived[1]);
                if (min == 0) {
                    min = PPP_UDP_KEEP_ALIVED_MIN_TIMEOUT;
                }

                if (max == 0) {
                    max = PPP_UDP_KEEP_ALIVED_MAX_TIMEOUT;
                }

                if (min > max) {
                    std::swap(min, max);
                }

                uint64_t tick = ppp::threading::Executors::GetTickCount();
                min = std::max<int>(1, min) * 1000;
                max = std::max<int>(1, max) * 1000;

                if (min == max) {
                    static_echo_timeout_ = tick + min;
                }
                else {
                    uint64_t next = RandomNext(min, max + 1);
                    static_echo_timeout_ = tick + next;
                }

                return true;
            }

            bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const ppp::net::packet::IPFrame* packet) noexcept {
                if (NULLPTR == packet || packet->AddressesFamily != AddressFamily::InterNetwork) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                int session_id = static_echo_session_id_;
                if (session_id < 1) {
                    return false;
                }

                int message_length = -1;
                std::shared_ptr<Byte> messages = VirtualEthernetPacket::Pack(configuration,
                    configuration->GetBufferAllocator(),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_protocol_; }),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_transport_; }),
                    session_id,
                    packet,
                    message_length);
                return StaticEchoPacketToRemoteExchanger(messages, message_length);
            }

            bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const std::shared_ptr<ppp::net::packet::UdpFrame>& frame) noexcept {
                if (NULLPTR == frame || frame->AddressesFamily != AddressFamily::InterNetwork) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                int session_id = static_echo_session_id_;
                if (session_id < 1) {
                    return false;
                }

                std::shared_ptr<ppp::net::packet::BufferSegment> payload_buffers = frame->Payload;
                if (NULLPTR == payload_buffers) {
                    return false;
                }

                int packet_length = -1;
                uint32_t source_ip = frame->Source.GetAddress();
                uint32_t destination_ip = frame->Destination.GetAddress();
                std::shared_ptr<Byte> packet = VirtualEthernetPacket::Pack(configuration,
                    configuration->GetBufferAllocator(),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_protocol_; }),
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_transport_; }),
                    session_id,
                    source_ip,
                    frame->Source.Port,
                    destination_ip,
                    frame->Destination.Port,
                    payload_buffers->Buffer.get(),
                    payload_buffers->Length,
                    packet_length);
                return StaticEchoPacketToRemoteExchanger(packet, packet_length);
            }

            bool VEthernetExchanger::StaticEchoPacketToRemoteExchanger(const std::shared_ptr<Byte>& packet, int packet_length) noexcept {
                if (NULLPTR == packet || packet_length < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                std::shared_ptr<StaticEchoDatagarmSocket> socket = static_echo_sockets_[0];
                if (NULLPTR == socket) {
                    return false;
                }

                bool opened = socket->is_open();
                if (!opened) {
                    return false;
                }

                boost::asio::ip::udp::endpoint serverEP = StaticEchoGetRemoteEndPoint();
                if (!socket->is_v6 && serverEP.address().is_v6()) {
                    boost::asio::ip::address_v6 address_v6 = serverEP.address().to_v6();
                    if (address_v6.is_v4_mapped()) {
                        boost::asio::ip::address_v6::bytes_type bytes = address_v6.to_bytes();
                        boost::asio::ip::address_v4::bytes_type v4_bytes = { bytes[12], bytes[13], bytes[14], bytes[15] };
                        serverEP = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4(v4_bytes), serverEP.port());
                    }
                }
                if (int serverPort = serverEP.port(); serverPort > IPEndPoint::MinPort && serverPort <= IPEndPoint::MaxPort) {
                    std::shared_ptr<ppp::transmissions::ITransmissionStatistics> statistics = switcher_->GetStatistics();
                    boost::asio::post(socket->get_executor(),
                        [statistics, socket, packet, packet_length, serverEP]() noexcept {
                            boost::system::error_code ec;
                            socket->send_to(boost::asio::buffer(packet.get(), packet_length), serverEP,
                                boost::asio::socket_base::message_end_of_record, ec);

                            if (ec == boost::system::errc::success) {
                                if (NULLPTR != statistics) {
                                    statistics->AddOutgoingTraffic(packet_length);
                                }
                            }
                        });
                    return true;
                }

                return false;
            }

            std::shared_ptr<ppp::app::protocol::VirtualEthernetPacket> VEthernetExchanger::StaticEchoReadPacket(const void* packet, int packet_length) noexcept {
                if (NULLPTR == packet || packet_length < 1) {
                    return NULLPTR;
                }

                if (disposed_) {
                    return NULLPTR;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return NULLPTR;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = configuration->GetBufferAllocator();
                return VirtualEthernetPacket::Unpack(configuration, 
                    allocator, 
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_protocol_; }), 
                    VirtualEthernetPacket::SessionCiphertext([this](int) noexcept { return static_echo_transport_; }),
                    packet, 
                    packet_length);
            }

            bool VEthernetExchanger::StaticEchoPacketInput(const std::shared_ptr<ppp::app::protocol::VirtualEthernetPacket>& packet) noexcept {
                if (NULLPTR == packet || disposed_) {
                    return false;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> allocator = configuration->GetBufferAllocator();
                static_echo_input_ = true;

                if (packet->Protocol == ppp::net::native::ip_hdr::IP_PROTO_UDP) {
                    auto tap = switcher_->GetTap();
                    if (NULLPTR == tap) {
                        return false;
                    }

                    std::shared_ptr<ppp::net::packet::UdpFrame> frame = packet->GetUdpPacket();
                    if (NULLPTR == frame) {
                        return false;
                    }

                    auto payload = frame->Payload;
                    if (NULLPTR != payload) {
                        const boost::asio::ip::udp::endpoint localEP =
                            IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Destination);
                        const boost::asio::ip::udp::endpoint remoteEP =
                            IPEndPoint::ToEndPoint<boost::asio::ip::udp>(frame->Source);
                        if (TryHandleDatagram(localEP, remoteEP,
                            payload->Buffer.get(), payload->Length)) {
                            LOG_DEBUG("VEthernetExchanger::StaticEchoPacketInput: DNS response handled, local=%s:%u, remote=%s:%u, bytes=%d",
                                localEP.address().to_string().data(), localEP.port(),
                                remoteEP.address().to_string().data(), remoteEP.port(), payload->Length);
                            return true;
                        }
                    }

                    std::shared_ptr<ppp::net::packet::IPFrame> ip = frame->ToIp(allocator);
                    if (NULLPTR == ip) {
                        return false;
                    }
                    
                    if (configuration->udp.dns.cache && frame->Source.Port == PPP_DNS_SYS_PORT) {
                        if (NULLPTR != payload) {
                            ppp::net::asio::vdns::AddCache(payload->Buffer.get(), payload->Length);
                        }
                    }

                    return switcher_->Output(ip.get());
                }
                elif(packet->Protocol == ppp::net::native::ip_hdr::IP_PROTO_IP) {
                    std::shared_ptr<ppp::net::packet::IPFrame> frame = packet->GetIPPacket(allocator);
                    if (NULLPTR == frame) {
                        return false;
                    }

                    if (frame->ProtocolType == ppp::net::native::ip_hdr::IP_PROTO_ICMP) {
                        if (frame->Source == IPEndPoint::LoopbackAddress) {
                            int ack_id = ntohl(frame->Destination);
                            if (ack_id == 0 || ack_id == STATIC_ECHO_KEEP_ALIVED_ID) {
                                return false;
                            }

                            return switcher_->ERORTE(ack_id);
                        }
                    }

                    return switcher_->Output(frame.get());
                }
                else {
                    return false;
                }
            }

            int VEthernetExchanger::StaticEchoYieldReceiveForm(Byte* incoming_packet, int incoming_traffic) noexcept {
                std::shared_ptr<VirtualEthernetPacket> packet = StaticEchoReadPacket(incoming_packet, incoming_traffic);
                if (NULLPTR != packet) {
                    StaticEchoPacketInput(packet);
                }

                auto statistics = switcher_->GetStatistics(); 
                if (NULLPTR != statistics) {
                    statistics->AddIncomingTraffic(incoming_traffic);
                }

                return incoming_traffic;
            }

            bool VEthernetExchanger::Sleep(int64_t timeout, const ContextPtr& context, YieldContext& y) noexcept {
                using atomic_int = std::atomic<int>;

                std::shared_ptr<atomic_int> status = ppp::make_shared_object<atomic_int>(-1);
                if (NULLPTR == status) {
                    return false;
                }

                auto self = shared_from_this();
                boost::asio::post(*context,
                    [self, this, context, timeout, status, &y]() noexcept {
                        bool ok = NewDeadlineTimer(context, timeout, 
                            [status, &y](bool b) noexcept {
                                ppp::coroutines::asio::R(y, *status, b);
                            });
                        
                        if (!ok) {
                            ppp::coroutines::asio::R(y, *status, false);
                        }
                    });

                y.Suspend();
                return status->load() > 0;
            }
            
            bool VEthernetExchanger::StaticEchoLoopbackSocket(const std::shared_ptr<StaticEchoDatagarmSocket>& socket) noexcept {
                if (disposed_) {
                    return false;
                }

                bool openped = socket->is_open();
                if (!openped) {
                    return false;
                }

                auto self = shared_from_this();
                if (std::shared_ptr<ppp::transmissions::ITransmissionQoS> qos = switcher_->GetQoS(); NULLPTR != qos) {
                    return qos->BeginRead(
                        [self, this, socket, qos]() noexcept {
                            socket->async_receive_from(boost::asio::buffer(buffer_.get(), PPP_BUFFER_SIZE), static_echo_source_ep_,
                                [self, this, qos, socket](const boost::system::error_code& ec, std::size_t sz) noexcept {
                                    int bytes_transferred = std::max<int>(-1, ec ? -1 : (int)sz);
                                    if (bytes_transferred > 0) { 
                                        qos->EndRead(StaticEchoYieldReceiveForm(buffer_.get(), bytes_transferred));
                                    }

                                    StaticEchoLoopbackSocket(socket);
                                });
                        });
                }
                else {
                    socket->async_receive_from(boost::asio::buffer(buffer_.get(), PPP_BUFFER_SIZE), static_echo_source_ep_,
                        [self, this, qos, socket](const boost::system::error_code& ec, std::size_t sz) noexcept {
                            int bytes_transferred = std::max<int>(-1, ec ? -1 : (int)sz);
                            if (bytes_transferred > 0) {
                                StaticEchoYieldReceiveForm(buffer_.get(), bytes_transferred);
                            }

                            StaticEchoLoopbackSocket(socket);
                        });
                    return true;
                }
            }

            bool VEthernetExchanger::StaticEchoAddRemoteEndPoint(boost::asio::ip::udp::endpoint& remoteEP) noexcept {
                boost::asio::ip::udp::endpoint destinationEP = Ipep::V4ToV6(remoteEP);
                boost::asio::ip::address destinationIP = destinationEP.address();
                if (!destinationIP.is_v6()) {
                    return false;
                }

                SynchronizedObjectScope scope(syncobj_);
                auto r = static_echo_server_ep_set_.emplace(destinationEP);
                if (!r.second) {
                    return false;
                }

                static_echo_server_ep_balances_.emplace_back(destinationEP);
                return true;
            }

            boost::asio::ip::udp::endpoint VEthernetExchanger::StaticEchoGetRemoteEndPoint() noexcept {
                std::shared_ptr<aggligator::aggligator> aggligator = switcher_->GetAggligator();
                if (NULLPTR != aggligator) {
#if !defined(_ANDROID) && !defined(_IPHONE)
                    auto ni = switcher_->GetUnderlyingNetworkInterface(); 
                    if (NULLPTR != ni) {
                        boost::asio::ip::udp::endpoint ep = aggligator->client_endpoint(ni->IPAddress);
                        return Ipep::V4ToV6(ep);
                    }
#endif
                    return aggligator->client_endpoint(boost::asio::ip::address_v6::loopback());
                }

                boost::asio::ip::udp::endpoint destinationEP;
                for (SynchronizedObjectScope scope(syncobj_);;) {
                    auto tail = static_echo_server_ep_balances_.begin();
                    auto endl = static_echo_server_ep_balances_.end();
                    if (tail == endl) {
                        destinationEP = boost::asio::ip::udp::endpoint(server_url_.remoteEP.address(), static_echo_remote_port_);
                        break;
                    }
                    
                    std::size_t server_addrsss_num = static_echo_server_ep_set_.size();
                    if (server_addrsss_num == 1) {
                        destinationEP = *static_echo_server_ep_balances_.begin();
                    }
                    else {
                        destinationEP = *tail;
                        static_echo_server_ep_balances_.erase(tail);
                        static_echo_server_ep_balances_.emplace_back(destinationEP);
                    }

                    break;
                }

                return Ipep::V4ToV6(destinationEP);
            }

            bool VEthernetExchanger::StaticEchoOpenAsynchronousSocket(StaticEchoDatagarmSocket& socket, YieldContext& y) noexcept {
                if (disposed_) {
                    return false;
                }

                bool opened = socket.is_open(true);
                if (opened) {
                    return true;
                }

                if (server_url_.port <= IPEndPoint::MinPort || server_url_.port > IPEndPoint::MaxPort) {
                    return false;
                }   

                AppConfigurationPtr configuration = GetConfiguration();
                if (NULLPTR == configuration) {
                    return false;
                }

                // Try IPv6 (dual-stack socket on Windows) first, fall back to IPv4
                // if the system has no IPv6 stack available.
                bool is_v6 = true;
                opened = ppp::coroutines::asio::async_open<boost::asio::ip::udp::socket>(y, socket, boost::asio::ip::udp::v6()) && !disposed_;
                if (!opened) {
                    is_v6 = false;
                    opened = ppp::coroutines::asio::async_open<boost::asio::ip::udp::socket>(y, socket, boost::asio::ip::udp::v4()) && !disposed_;
                }
                if (!opened) {
                    return false;
                }

                bool ok = false;
                for (;;) {
                    boost::asio::ip::address listen_address = is_v6
                        ? boost::asio::ip::address(boost::asio::ip::address_v6::any())
                        : boost::asio::ip::address(boost::asio::ip::address_v4::any());
                    opened = Socket::OpenSocket(socket, listen_address, IPEndPoint::MinPort, opened);
                    if (!opened) {
                        break;
                    }
                    else {
                        Socket::SetWindowSizeIfNotZero(socket.native_handle(), configuration->udp.cwnd, configuration->udp.rwnd);
                    }
                    
#if defined(_ANDROID)
                    std::shared_ptr<aggligator::aggligator> aggligator = switcher_->GetAggligator();
                    if (NULLPTR == aggligator) {
                        auto protector_network = switcher_->GetProtectorNetwork(); 
                        if (NULLPTR != protector_network) {
                            opened = protector_network->Protect(socket.native_handle(), y);
                            if (!opened) {
                                break;
                            }
                        }
                    }
#endif
                    // Mark that the socket has been opened.
                    socket.opened = opened;
                    socket.is_v6 = is_v6;

                    // Set the timeout period for closing and re-opening the socket next-timed.
                    ok = StaticEchoNextTimeout();
                    break;
                }

                if (!ok) {
                    Socket::Closesocket(socket);
                }

                return ok;
            }
        }
    }
}
