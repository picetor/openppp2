#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>
#include <ppp/app/protocol/templates/TVEthernetTcpipConnection.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetNetworkTcpipConnection.h>
#include <ppp/app/client/proxys/VEthernetLocalProxySwitcher.h>
#include <ppp/app/client/proxys/VEthernetLocalProxyConnection.h>

#include <ppp/IDisposable.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/threading/Executors.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>

namespace ppp {
    namespace app {
        namespace client {
            namespace proxys {
                VEthernetLocalProxyConnection::VEthernetLocalProxyConnection(const VEthernetLocalProxySwitcherPtr& proxy, const VEthernetExchangerPtr& exchanger, const std::shared_ptr<boost::asio::io_context>& context, const ppp::threading::Executors::StrandPtr& strand, const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept
                    : disposed_(false)
                    , context_(context)
                    , strand_(strand)
                    , timeout_(0)
                    , exchanger_(exchanger)
                    , socket_(socket)
                    , configuration_(proxy->GetConfiguration())
                    , proxy_(proxy)
                    , allocator_(configuration_->GetBufferAllocator()) {
                    Update();
                }

                VEthernetLocalProxyConnection::~VEthernetLocalProxyConnection() noexcept {
                    Finalize();
                }

                void VEthernetLocalProxyConnection::Dispose() noexcept {
                    std::shared_ptr<VEthernetLocalProxyConnection> self = shared_from_this();
                    ppp::threading::Executors::ContextPtr context = context_;
                    ppp::threading::Executors::StrandPtr strand = strand_;

                    auto finalize = 
                        [self, this, context, strand]() noexcept {
                            Finalize();
                        };

                    std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_; 
                    if (NULLPTR != socket) {
                        boost::asio::post(socket->get_executor(), finalize);
                    }
                    else {
                        ppp::threading::Executors::Post(context, strand, finalize);
                    }
                }

                void VEthernetLocalProxyConnection::Finalize() noexcept {
                    for (;;) {
                        std::shared_ptr<VirtualEthernetTcpipConnection> connection = std::move(connection_);
                        std::shared_ptr<RinetdConnection> connection_rinetd = std::move(connection_rinetd_); 
                        std::shared_ptr<vmux::vmux_skt> connection_mux = std::move(connection_mux_);
      
                        if (NULLPTR != connection) {
                            connection->Dispose();
                        }

                        if (NULLPTR != connection_rinetd) {
                            connection_rinetd->Dispose();
                        }

                        if (NULLPTR != connection_mux) {
                            connection_mux->close();
                        }

                        ppp::net::Socket::Closesocket(socket_);
                        break;
                    }

                    disposed_ = true;
                    proxy_->ReleaseConnection(this);
                }

                bool VEthernetLocalProxyConnection::Run(YieldContext& y) noexcept {
                    if (!WaitForExchanger(y)) {
                        return false;
                    }

                    bool ok = this->Handshake(y);
                    if (!ok) {
                        return false;
                    }
                    elif(disposed_) {
                        return false;
                    }
                    elif(VirtualEthernetTcpipConnectionPtr connection = this->connection_; NULLPTR != connection) {
                        this->Update();
                        return connection->Run(y);
                    }
                    elif(std::shared_ptr<RinetdConnection> connection = this->connection_rinetd_; NULLPTR != connection) {
                        this->Update();
                        return connection->Run();
                    }
                    elif(std::shared_ptr<vmux::vmux_skt> connection = this->connection_mux_; NULLPTR != connection) {
                        this->Update();
                        return connection->run();
                    }
                    else {
                        return RunAfterHandshakeWithoutBridge(y);
                    }
                }

                bool VEthernetLocalProxyConnection::WaitForExchanger(YieldContext& y) noexcept {
                    const int configured_timeout = configuration_ != NULLPTR
                        ? std::max<int>(1, configuration_->tcp.connect.timeout) : 10;
                    const uint64_t deadline = Executors::GetTickCount() +
                        static_cast<uint64_t>(std::min<int>(30, configured_timeout)) * 1000ULL;

                    while (!disposed_) {
                        // A new proxy connection may have been accepted while
                        // the primary exchanger was being hot-switched.  Read
                        // the listener's current exchanger before waiting so
                        // this connection does not remain attached to a retired
                        // object.
                        std::shared_ptr<VEthernetExchanger> current = proxy_ != NULLPTR
                            ? proxy_->GetExchanger() : exchanger_;
                        if (current == NULLPTR) {
                            return false;
                        }

                        if (current->GetNetworkState() ==
                            VEthernetExchanger::NetworkState_Established) {
                            exchanger_ = current;
                            // The connection may have been accepted just
                            // before a primary hot switch.  Once it begins
                            // its handshake, bind it to the selected
                            // outbound's complete client configuration as
                            // well, especially SOCKS5 credentials and the
                            // per-client timeout/buffer settings.
                            configuration_ = current->GetConfiguration();
                            if (configuration_ == NULLPTR) {
                                return false;
                            }
                            allocator_ = configuration_->GetBufferAllocator();
                            return true;
                        }

                        const uint64_t now = Executors::GetTickCount();
                        if (now >= deadline) {
                            return false;
                        }

                        const int64_t wait_ms = static_cast<int64_t>(
                            std::min<uint64_t>(100, deadline - now));
                        if (!current->Sleep(wait_ms, context_, y)) {
                            return false;
                        }
                    }
                    return false;
                }

                bool VEthernetLocalProxyConnection::RunAfterHandshakeWithoutBridge(YieldContext& y) noexcept {
                    return false;
                }

                bool VEthernetLocalProxyConnection::SendBufferToPeer(YieldContext& y, const void* messages, int messages_size) noexcept {
                    if (NULLPTR == messages || messages_size < 1) {
                        return false;
                    }

                    if (disposed_) {
                        return false;
                    }

                    VirtualEthernetTcpipConnectionPtr V = this->connection_; 
                    if (NULLPTR != V) {
                        return V->SendBufferToPeer(y, messages, messages_size);
                    }

                    std::shared_ptr<RinetdConnection> R = this->connection_rinetd_;
                    if (NULLPTR != R) {
                        std::shared_ptr<boost::asio::ip::tcp::socket> socket = R->GetRemoteSocket(); 
                        if (NULLPTR == socket) {
                            return false;
                        }

                        return ppp::coroutines::asio::async_write(*socket, boost::asio::buffer(messages, messages_size), y);
                    }
                    
                    std::shared_ptr<vmux::vmux_skt> K = this->connection_mux_;
                    if (NULLPTR != K) {
                        return K->send_to_peer_yield(messages, messages_size, y);
                    }

                    return false;
                }
 
                bool VEthernetLocalProxyConnection::ConnectBridgeToPeer(const std::shared_ptr<ppp::app::protocol::AddressEndPoint>& destinationEP, YieldContext& y) noexcept {
                    using VEthernetTcpipConnection = ppp::app::protocol::templates::TVEthernetTcpipConnection<VEthernetLocalProxyConnection>;
                    
                    if (NULLPTR == destinationEP) {
                        return false;
                    }

                    LOG_DEBUG("VEthernetLocalProxyConnection::ConnectBridgeToPeer: source=local-proxy, trace=%p, transport_trace=%p, outbound=%s, destination=%s:%d",
                        this, strand_.get(), exchanger_->GetOutboundTag().data(), destinationEP->Host.data(), destinationEP->Port);

                    std::shared_ptr<boost::asio::ip::tcp::socket> socket = GetSocket();
                    if (NULLPTR == socket || !socket->is_open()) {
                        return false;
                    }

                    auto self = shared_from_this();
                    std::shared_ptr<VEthernetNetworkSwitcher> switcher = exchanger_->GetSwitcher();
                    bool force_direct = false;
                    boost::asio::ip::address direct_address;

                    // System-proxy connections bypass the TUN packet path, so
                    // apply the same bypass policy before selecting the MUX
                    // exchanger. This is required in both normal TUN mode
                    // and proxy-only mode.
                    if (NULLPTR != switcher) {
                        std::shared_ptr<VEthernetExchanger> selected;
                        if (destinationEP->Type == ppp::app::protocol::AddressType::Domain) {
                            if (switcher->IsDirectProxyHost(destinationEP->Host)) {
                                direct_address = ppp::coroutines::asio::GetAddressByHostName<boost::asio::ip::tcp>(
                                    destinationEP->Host.data(), destinationEP->Port, y).address();
                                if (ppp::net::IPEndPoint::IsInvalid(direct_address)) {
                                    return false;
                                }
                                force_direct = true;
                            }
                            elif (switcher->UsesProxyIpRules()) {
                                // IP-list mode has no domain rule to consult.
                                // Resolve only to test the bypass CIDR; a
                                // non-matching domain remains a domain and is
                                // resolved by the tunnel peer.
                                direct_address = ppp::coroutines::asio::GetAddressByHostName<boost::asio::ip::tcp>(
                                    destinationEP->Host.data(), destinationEP->Port, y).address();
                                if (!ppp::net::IPEndPoint::IsInvalid(direct_address) &&
                                    switcher->IsDirectProxyAddress(direct_address)) {
                                    force_direct = true;
                                }
                            }
                            else {
                                selected = switcher->GetExchanger(destinationEP->Host);
                            }
                        }
                        else {
                            boost::system::error_code ec;
                            direct_address = StringToAddress(destinationEP->Host.data(), ec);
                            if (ec || ppp::net::IPEndPoint::IsInvalid(direct_address)) {
                                return false;
                            }
                            // In --bypass-mode=no, the normal TUN RIB contains
                            // the tunnel default route. It is not a direct
                            // proxy rule, so only consult the address bypass
                            // helper for explicit IP or GEO split modes.
                            if ((switcher->UsesProxyIpRules() || switcher->GetGeoRules() != NULLPTR) &&
                                switcher->IsDirectProxyAddress(direct_address)) {
                                force_direct = true;
                            }
                            else {
                                selected = switcher->GetExchanger(direct_address);
                            }
                        }

                        if (NULLPTR != selected) {
                            exchanger_ = selected;
                        }

                        const char* bypass_mode = switcher->GetGeoRules() != NULLPTR
                            ? "geo" : switcher->UsesProxyIpRules() ? "ip" : "no";
                        LOG_DEBUG("VEthernetLocalProxyConnection::ConnectBridgeToPeer: source=local-proxy, trace=%p, bypass_mode=%s, policy_outbound=%s, direct=%d, destination=%s:%d",
                            this, bypass_mode, exchanger_->GetOutboundTag().data(), (int)force_direct,
                            destinationEP->Host.data(), destinationEP->Port);
                    }

                    auto configuration = exchanger_->GetConfiguration();
                    if (NULLPTR == configuration) {
                        return false;
                    }

                    if (force_direct) {
                        int rinetd_status = VEthernetNetworkTcpipConnection::Rinetd(self,
                            exchanger_,
                            context_,
                            strand_,
                            configuration,
                            socket,
                            boost::asio::ip::tcp::endpoint(direct_address, destinationEP->Port),
                            connection_rinetd_,
                            y,
                            true);
                        if (rinetd_status < 1) {
                            return rinetd_status == 0;
                        }
                        destinationEP->Host = direct_address.to_string();
                        destinationEP->Type = direct_address.is_v4()
                            ? ppp::app::protocol::AddressType::IPv4
                            : ppp::app::protocol::AddressType::IPv6;
                    }

                    if (auto switcher = exchanger_->GetSwitcher(); NULLPTR != switcher) {
                        if (auto tap = switcher->GetTap(); NULLPTR != tap && tap->IsHostedNetwork()) {
                            // Domain targets must keep their hostname so the tunnel server
                            // resolves them (OnConnectHost).  The local resolver may be
                            // DNS-polluted (e.g. GFW poisoning), which would rewrite
                            // www.google.com into an attacker IP and break the tunnel.
                            if (destinationEP->Type != ppp::app::protocol::AddressType::Domain) {
                                boost::system::error_code ec;
                                boost::asio::ip::address address = StringToAddress(destinationEP->Host.data(), ec);
                                if (ec) {
                                    address = ppp::coroutines::asio::GetAddressByHostName<boost::asio::ip::tcp>(destinationEP->Host.data(), destinationEP->Port, y).address();
                                }

                                if (ppp::net::IPEndPoint::IsInvalid(address)) {
                                    return false;
                                }

                                int rinetd_status = VEthernetNetworkTcpipConnection::Rinetd(self,
                                    exchanger_,
                                    context_,
                                    strand_,
                                    configuration,
                                    socket,
                                    boost::asio::ip::tcp::endpoint(address, destinationEP->Port),
                                    connection_rinetd_,
                                    y);
                                if (rinetd_status < 1) {
                                    return rinetd_status == 0;
                                }

                                destinationEP->Host = address.to_string();
                                destinationEP->Type = address.is_v4() ? ppp::app::protocol::AddressType::IPv4 : ppp::app::protocol::AddressType::IPv6;
                            }
                        }
                    }

                    // Keep domain targets as domains.  VirtualEthernetTcpipConnection
                    // forwards them in PREPARED_CONNECT and the tunnel server resolves
                    // them in OnConnectHost.  Resolving here would leak DNS to the local
                    // resolver on Linux and would also make proxy-only behavior differ
                    // between platforms.

                    int mux_status = VEthernetNetworkTcpipConnection::Mux(self, exchanger_, "local-proxy", this,
                        destinationEP->Host, destinationEP->Port, socket, connection_mux_, y);
                    if (mux_status < 1) {
                        return mux_status == 0;
                    }

                    std::shared_ptr<ppp::transmissions::ITransmission> transmission = exchanger_->ConnectTransmission(context_, strand_, y);
                    if (NULLPTR == transmission) {
                        LOG_DEBUG("VEthernetLocalProxyConnection::ConnectBridgeToPeer: source=local-proxy, trace=%p, transport_trace=%p, outbound=%s, ConnectTransmission failed, destination=%s:%d",
                            this, strand_.get(), exchanger_->GetOutboundTag().data(), destinationEP->Host.data(), destinationEP->Port);
                        return false;
                    }

                    std::shared_ptr<VEthernetTcpipConnection> connection =
                        make_shared_object<VEthernetTcpipConnection>(self, configuration, context_, strand_, exchanger_->GetId(), socket);
                    if (NULLPTR == connection) {
                        IDisposable::DisposeReferences(transmission);
                        return false;
                    }

#if defined(_LINUX)
                    auto connection_switcher = exchanger_->GetSwitcher();
                    if (NULLPTR != connection_switcher) {
                        connection->ProtectorNetwork = connection_switcher->GetProtectorNetwork();
                    }
#endif

                    bool ok = connection->Connect(y, transmission, destinationEP->Host, destinationEP->Port);
                    if (!ok) {
                        IDisposable::DisposeReferences(connection, transmission);
                        return false;
                    }

                    this->connection_ = std::move(connection);
                    LOG_DEBUG("VEthernetLocalProxyConnection::ConnectBridgeToPeer: source=local-proxy, trace=%p, transport_trace=%p, outbound=%s, connected, destination=%s:%d",
                        this, strand_.get(), exchanger_->GetOutboundTag().data(), destinationEP->Host.data(), destinationEP->Port);
                    return true;
                }

                std::shared_ptr<ppp::app::protocol::AddressEndPoint> VEthernetLocalProxyConnection::GetAddressEndPointByProtocol(const ppp::string& host, int port) noexcept {
                    if (port <= ppp::net::IPEndPoint::MinPort || port > ppp::net::IPEndPoint::MaxPort) {
                        return NULLPTR;
                    }

                    if (host.empty()) {
                        return NULLPTR;
                    }

                    std::shared_ptr<ppp::app::protocol::AddressEndPoint> destinationEP = make_shared_object<ppp::app::protocol::AddressEndPoint>();
                    if (NULLPTR == destinationEP) {
                        return NULLPTR;
                    }

                    boost::system::error_code ec;
                    boost::asio::ip::address address = StringToAddress(host, ec);

                    if (ec) {
                        destinationEP->Type = ppp::app::protocol::AddressType::Domain;
                    }
                    elif(address.is_v4()) {
                        if (address.is_unspecified() || address.is_multicast()) {
                            return NULLPTR;
                        }
                        destinationEP->Type = ppp::app::protocol::AddressType::IPv4;
                    }
                    elif(address.is_v6()) {
                        if (address.is_unspecified() || address.is_multicast()) {
                            return NULLPTR;
                        }
                        destinationEP->Type = ppp::app::protocol::AddressType::IPv6;
                    }
                    else {
                        return NULLPTR;
                    }

                    destinationEP->Host = host;
                    destinationEP->Port = port;
                    return destinationEP;
                }

                void VEthernetLocalProxyConnection::Update() noexcept {
                    bool linked = false;
                    if (VirtualEthernetTcpipConnectionPtr connection = connection_; NULLPTR != connection) {
                        linked = connection->IsLinked();
                    }
                    elif(std::shared_ptr<RinetdConnection> connection = connection_rinetd_; NULLPTR != connection) {
                        linked = connection->IsLinked();
                    }
                    elif(std::shared_ptr<vmux::vmux_skt> connection = connection_mux_; NULLPTR != connection) {
                        linked = connection->is_connected();
                    }

                    uint64_t now = Executors::GetTickCount();
                    if (linked) {
                        timeout_ = now + (UInt64)configuration_->tcp.inactive.timeout * 1000;
                    }
                    else {
                        timeout_ = now + (UInt64)configuration_->tcp.connect.timeout * 1000;
                    }
                }
            }
        }
    }
}
