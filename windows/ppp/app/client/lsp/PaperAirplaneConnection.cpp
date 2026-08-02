#include <windows/ppp/app/client/lsp/PaperAirplaneConnection.h>
#include <windows/ppp/app/client/lsp/PaperAirplaneController.h>

#include <ppp/IDisposable.h>
#include <ppp/net/Ipep.h>
#include <ppp/threading/Executors.h>

#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetNetworkTcpipConnection.h>
#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>
#include <ppp/app/protocol/templates/TVEthernetTcpipConnection.h>

namespace ppp
{
    namespace app
    {
        namespace client
        {
            namespace lsp
            {
                PaperAirplaneConnection::PaperAirplaneConnection(const std::shared_ptr<PaperAirplaneController>& controller, const ContextPtr& context, const ppp::threading::Executors::StrandPtr& strand, const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept
                    : disposed_(false)
                    , timeout_(0)
                    , controller_(controller)
                    , context_(context)
                    , strand_(strand)
                    , socket_(socket)
                    , configuration_(controller->GetConfiguration())
                {
                    Update();
                }

                PaperAirplaneConnection::~PaperAirplaneConnection() noexcept
                {
                    Finalize();
                }

                void PaperAirplaneConnection::Finalize() noexcept
                {
                    exchangeof(disposed_, true);
                    for (;;)
                    {
                        std::shared_ptr<VirtualEthernetTcpipConnection> connection = std::move(connection_);
                        std::shared_ptr<RinetdConnection> connection_rinetd = std::move(connection_rinetd_);
                        std::shared_ptr<vmux::vmux_skt> connection_mux = std::move(connection_mux_);

                        if (NULLPTR != connection)
                        {
                            connection->Dispose();
                        }

                        if (NULLPTR != connection_rinetd)
                        {
                            connection_rinetd->Dispose();
                        }

                        if (NULLPTR != connection_mux) 
                        {
                            connection_mux->close();
                        }

                        ppp::net::Socket::Closesocket(socket_);
                        break;
                    }

                    controller_->ReleaseConnection(this);
                }

                void PaperAirplaneConnection::Update() noexcept
                {
                    bool linked = false;
                    if (VirtualEthernetTcpipConnectionPtr connection = connection_; NULLPTR != connection)
                    {
                        linked = connection->IsLinked();
                    }
                    elif(std::shared_ptr<RinetdConnection> connection_rinetd = connection_rinetd_; NULLPTR != connection_rinetd)
                    {
                        linked = connection_rinetd->IsLinked();
                    }
                    elif(std::shared_ptr<vmux::vmux_skt> connection_mux = connection_mux_; NULLPTR != connection_mux)
                    {
                        linked = connection_mux->is_connected();
                    }

                    uint64_t now = Executors::GetTickCount();
                    if (linked)
                    {
                        timeout_ = now + (UInt64)configuration_->tcp.inactive.timeout * 1000ULL;
                    }
                    else
                    {
                        timeout_ = now + (UInt64)configuration_->tcp.connect.timeout * 1000ULL;
                    }
                }

                void PaperAirplaneConnection::Dispose() noexcept
                {
                    auto self = shared_from_this();
                    ppp::threading::Executors::ContextPtr context = context_;
                    ppp::threading::Executors::StrandPtr strand = strand_;

                    ppp::threading::Executors::Post(context, strand,
                        [self, this, context, strand]() noexcept
                        {
                            Finalize();
                        });
                }

                PaperAirplaneConnection::VEthernetExchangerPtr PaperAirplaneConnection::GetExchanger() noexcept
                {
                    PaperAirplaneControllerPtr controller = GetController();
                    if (NULLPTR == controller)
                    {
                        return NULLPTR;
                    }
                    else
                    {
                        return controller->GetExchanger();
                    }
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> PaperAirplaneConnection::GetBufferAllocator() noexcept
                {
                    AppConfigurationPtr configuration = GetConfiguration();
                    if (NULLPTR == configuration)
                    {
                        return NULLPTR;
                    }
                    else
                    {
                        return configuration->GetBufferAllocator();
                    }
                }

                bool PaperAirplaneConnection::Run(const boost::asio::ip::address& host, int port, YieldContext& y) noexcept
                {
                    bool ok = this->OnConnect(host, port, y);
                    if (!ok)
                    {
                        return false;
                    }

                    if (disposed_) 
                    {
                        return false;
                    }

                    VirtualEthernetTcpipConnectionPtr connection = this->connection_;
                    if (NULLPTR != connection) 
                    {
                        this->Update();
                        return connection->Run(y);
                    }

                    std::shared_ptr<RinetdConnection> connection_rinetd = this->connection_rinetd_;
                    if (NULLPTR != connection_rinetd)
                    {
                        this->Update();
                        return connection_rinetd->Run();
                    }

                    std::shared_ptr<vmux::vmux_skt> connection_mux = this->connection_mux_;
                    if (NULLPTR != connection_mux)
                    {
                        this->Update();
                        return connection_mux->run();
                    }

                    return false;
                }

                bool PaperAirplaneConnection::OnConnect(const boost::asio::ip::address& host, int port, YieldContext& y) noexcept
                {
                    using VEthernetTcpipConnection = ppp::app::protocol::templates::TVEthernetTcpipConnection<PaperAirplaneConnection>;

                    if (disposed_)
                    {
                        return false;
                    }

                    if (!y)
                    {
                        return false;
                    }

                    std::shared_ptr<boost::asio::io_context> context = GetContext();
                    if (NULLPTR == context)
                    {
                        return false;
                    }

                    std::shared_ptr<boost::asio::ip::tcp::socket> socket = GetSocket();
                    if (NULLPTR == socket)
                    {
                        return false;
                    }

                    VEthernetExchangerPtr exchanger = GetExchanger();
                    if (NULLPTR == exchanger)
                    {
                        return false;
                    }

                    auto self = shared_from_this();
                    boost::asio::ip::tcp::endpoint remoteEP(host, port);
                    ppp::string remote_host = ppp::net::Ipep::ToAddressString<ppp::string>(remoteEP);

                    // PaperAirplane is created with the primary exchanger, but the
                    // destination is only known after the intercepted connection has
                    // reached this point. Re-select here so DNS-learned Geo policies
                    // cannot silently send a secondary-outbound flow through main.
                    if (std::shared_ptr<VEthernetNetworkSwitcher> switcher = exchanger->GetSwitcher(); NULLPTR != switcher)
                    {
                        // Peer-prefix destinations (e.g. 192.168.11.0/24 announced by
                        // a peer gateway) must never use mux/direct sub-transmission:
                        // both terminate on the server, which cannot reach peer LANs.
                        // Connect locally instead so the OS routes the flow through the
                        // TAP data plane, where peer-prefix TCP is NAT'd to the
                        // announcing peer gateway (exactly like ICMP).
                        if (host.is_v4() && NULLPTR != switcher->FindAppliedPeerPrefixRoute(htonl(host.to_v4().to_uint())))
                        {
                            AppConfigurationPtr direct_configuration = exchanger->GetConfiguration();
                            if (NULLPTR == direct_configuration)
                            {
                                return false;
                            }

                            int rinetd_status = VEthernetNetworkTcpipConnection::Rinetd(self, exchanger, context, strand_,
                                direct_configuration, socket, remoteEP, connection_rinetd_, y);
                            LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, selected_outbound=peer-prefix-local, status=%d",
                                this, remote_host.data(), rinetd_status);
                            return rinetd_status == 0;
                        }

                        std::shared_ptr<VEthernetExchanger> selected = switcher->GetExchanger(host);
                        if (NULLPTR == selected)
                        {
                            bool force_direct = host.is_v4() ?
                                switcher->IsBypassIpAddress(host) :
                                switcher->IsBypassIpAddress6(host);
                            if (!force_direct)
                            {
                                LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, selected_outbound=none, reason=outbound_unavailable",
                                    this, remote_host.data());
                                return false;
                            }

                            AppConfigurationPtr direct_configuration = exchanger->GetConfiguration();
                            if (NULLPTR == direct_configuration)
                            {
                                return false;
                            }

                            int rinetd_status = VEthernetNetworkTcpipConnection::Rinetd(self, exchanger, context, strand_,
                                direct_configuration, socket, remoteEP, connection_rinetd_, y);
                            LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, selected_outbound=direct, reason=final_reselection, status=%d",
                                this, remote_host.data(), rinetd_status);
                            return rinetd_status == 0;
                        }

                        if (selected->GetNetworkState() != VEthernetExchanger::NetworkState_Established)
                        {
                            LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, requested_outbound=%s, reason=outbound_not_established",
                                this, remote_host.data(), selected->GetOutboundTag().data());
                            return false;
                        }

                        if (selected.get() != exchanger.get())
                        {
                            LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, previous_outbound=%s, selected_outbound=%s, reason=final_reselection",
                                this, remote_host.data(), exchanger->GetOutboundTag().data(), selected->GetOutboundTag().data());
                        }
                        exchanger = std::move(selected);
                    }

                    AppConfigurationPtr configuration = exchanger->GetConfiguration();
                    if (NULLPTR == configuration)
                    {
                        return false;
                    }
                    configuration_ = configuration;

                    int mux_status = VEthernetNetworkTcpipConnection::Mux(self, exchanger, "paper-airplane", this,
                        remoteEP, socket, connection_mux_, y);
                    if (mux_status == 0)
                    {
                        LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, mux selected, status=%d",
                            this, remote_host.data(), mux_status);
                        return true;
                    }
                    if (mux_status < 0)
                    {
                        LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, mux connect_yield failed, fallback to direct, status=%d",
                            this, remote_host.data(), mux_status);
                    }

                    LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, using direct sub-transmission, mux_status=%d",
                        this, remote_host.data(), mux_status);

                    std::shared_ptr<ppp::transmissions::ITransmission> transmission = exchanger->ConnectTransmission(context, strand_, y);
                    if (NULLPTR == transmission)
                    {
                        LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, ConnectTransmission failed",
                            this, remote_host.data());
                        return false;
                    }

                    std::shared_ptr<VEthernetTcpipConnection> connection = 
                        make_shared_object<VEthernetTcpipConnection>(self, configuration, context, strand_, exchanger->GetId(), socket);
                    if (NULLPTR == connection)
                    {
                        IDisposable::DisposeReferences(transmission);
                        return false;
                    }

                    bool ok = connection->Connect(y, transmission, stl::transform<ppp::string>(host.to_string()), port);
                    if (!ok)
                    {
                        LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, remote CONNECT failed",
                            this, remote_host.data());
                        IDisposable::DisposeReferences(connection, transmission);
                        return false;
                    }

                    this->connection_ = std::move(connection);
                    LOG_DEBUG("PaperAirplaneConnection::OnConnect: source=paper-airplane, trace=%p, destination=%s, connected",
                        this, remote_host.data());
                    return true;
                }
            }
        }
    }
}
