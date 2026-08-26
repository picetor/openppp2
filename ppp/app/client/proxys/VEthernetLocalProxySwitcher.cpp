#include <ppp/app/client/proxys/VEthernetLocalProxySwitcher.h>
#include <ppp/app/client/proxys/VEthernetLocalProxyConnection.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>

#include <ppp/threading/Timer.h>
#include <ppp/threading/Executors.h>

#include <ppp/collections/Dictionary.h>
#include <ppp/coroutines/YieldContext.h>

namespace ppp {
    namespace app {
        namespace client {
            namespace proxys {
                VEthernetLocalProxySwitcher::VEthernetLocalProxySwitcher(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept
                    : disposed_(false)
                    , exchanger_(exchanger)
                    , context_(ppp::threading::Executors::GetDefault())
                    , configuration_(exchanger->GetConfiguration()) {

                }

                VEthernetLocalProxySwitcher::~VEthernetLocalProxySwitcher() noexcept {
                    Finalize();
                }

                std::shared_ptr<VEthernetExchanger> VEthernetLocalProxySwitcher::GetExchanger() noexcept {
                    SynchronizedObjectScope scope(syncobj_);
                    return exchanger_;
                }

                std::shared_ptr<ppp::configurations::AppConfiguration> VEthernetLocalProxySwitcher::GetConfiguration() noexcept {
                    SynchronizedObjectScope scope(syncobj_);
                    return configuration_;
                }

                void VEthernetLocalProxySwitcher::SetExchanger(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept {
                    SynchronizedObjectScope scope(syncobj_);
                    exchanger_ = exchanger;
                    configuration_ = NULLPTR != exchanger ? exchanger->GetConfiguration() : NULLPTR;
                }

                bool VEthernetLocalProxySwitcher::ReconfigureExchanger(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept {
                    if (NULLPTR == exchanger) {
                        return false;
                    }

                    std::shared_ptr<ppp::net::SocketAcceptor> old_acceptor;
                    std::shared_ptr<ppp::threading::Timer> old_timeout;
                    boost::asio::ip::tcp::endpoint old_endpoint;
                    bool had_acceptor = false;
                    bool had_timeout = false;
                    {
                        SynchronizedObjectScope scope(syncobj_);
                        exchanger_ = exchanger;
                        configuration_ = exchanger->GetConfiguration();
                        if (NULLPTR != acceptor_) {
                            old_endpoint = ppp::net::Socket::GetLocalEndPoint(acceptor_->GetHandle());
                            had_acceptor = true;
                        }
                        had_timeout = NULLPTR != timeout_;
                    }

                    int bind_port = ppp::net::IPEndPoint::MinPort;
                    boost::asio::ip::address bind_address = MyLocalEndPoint(bind_port);
                    const bool enabled = bind_port > ppp::net::IPEndPoint::MinPort &&
                        bind_port <= ppp::net::IPEndPoint::MaxPort &&
                        (bind_address.is_v4() || bind_address.is_v6()) &&
                        !bind_address.is_multicast() &&
                        (bind_address.is_unspecified() ||
                            !ppp::net::IPEndPoint::IsInvalid(bind_address));

                    if (had_acceptor && enabled && old_endpoint.port() == bind_port &&
                        old_endpoint.address() == bind_address) {
                        return true;
                    }

                    if (had_acceptor || had_timeout) {
                        {
                            SynchronizedObjectScope scope(syncobj_);
                            old_acceptor = std::move(acceptor_);
                            old_timeout = std::move(timeout_);
                        }
                        if (NULLPTR != old_timeout) {
                            old_timeout->Dispose();
                        }
                        if (NULLPTR != old_acceptor) {
                            old_acceptor->Dispose();
                        }
                    }

                    // Port 0/invalid means that this proxy is disabled in
                    // the newly promoted main profile.  The old listener has
                    // already been closed, so this is a successful reconfig.
                    if (!enabled) {
                        return true;
                    }

                    if (!Open()) {
                        LOG_ERROR("VEthernetLocalProxySwitcher::ReconfigureExchanger: cannot reopen listener on %s:%d",
                            bind_address.to_string().data(), bind_port);
                        return false;
                    }
                    return true;
                }

                void VEthernetLocalProxySwitcher::Finalize() noexcept {
                    VEthernetLocalProxyConnectionTable connections;
                    for (;;) {
                        SynchronizedObjectScope scope(syncobj_);
                        connections = std::move(connections_);
                        connections_.clear();
                        break;
                    }

                    std::shared_ptr<ppp::threading::Timer> timeout = std::move(timeout_); 
                    std::shared_ptr<ppp::net::SocketAcceptor> acceptor = std::move(acceptor_); 

                    if (NULLPTR != timeout) {
                        timeout->Dispose();
                    }

                    if (NULLPTR != acceptor) {
                        acceptor->Dispose();
                    }

                    disposed_ = true;
                    ppp::collections::Dictionary::ReleaseAllObjects(connections);
                }

                void VEthernetLocalProxySwitcher::Update(UInt64 now) noexcept {
                    SynchronizedObjectScope scope(syncobj_);
                    ppp::collections::Dictionary::UpdateAllObjects(connections_, now);
                }

                void VEthernetLocalProxySwitcher::Dispose() noexcept {
                    auto self = shared_from_this();
                    boost::asio::post(*context_, 
                        [self, this]() noexcept {
                            Finalize();
                        });
                }

                bool VEthernetLocalProxySwitcher::Open() noexcept {
                    if (NULLPTR != acceptor_) {
                        return false;
                    }

                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger();
                    if (NULLPTR == configuration || NULLPTR == exchanger) {
                        return false;
                    }

                    std::shared_ptr<ppp::net::SocketAcceptor> acceptor;
                    if (disposed_) {
                        return false;
                    }
                    else {
                        int bind_port = configuration->client.http_proxy.port;
                        // MyLocalEndPoint is the complete bind policy for this
                        // listener.  Do not fall back to an unspecified address:
                        // a failed loopback bind must not silently expose the
                        // proxy on every interface.
                        boost::asio::ip::address interfaceIP = MyLocalEndPoint(bind_port);
                        if (bind_port <= ppp::net::IPEndPoint::MinPort || bind_port > ppp::net::IPEndPoint::MaxPort) {
                            return false;
                        }

                        bool proxy_only = false;
                        if (auto switcher = exchanger->GetSwitcher(); NULLPTR != switcher) {
                            proxy_only = switcher->IsProxyOnly();
                        }
                        if (interfaceIP.is_multicast() ||
                            !(interfaceIP.is_v4() || interfaceIP.is_v6()) ||
                            (!interfaceIP.is_unspecified() &&
                                ppp::net::IPEndPoint::IsInvalid(interfaceIP))) {
                            return false;
                        }

                        std::shared_ptr<ppp::net::SocketAcceptor> t = ppp::net::SocketAcceptor::New();
                        if (NULLPTR == t) {
                            return false;
                        }

                        ppp::string address_string = ppp::net::Ipep::ToAddressString<ppp::string>(interfaceIP);
                        if (t->Open(address_string.data(), bind_port, configuration->tcp.backlog)) {
                            acceptor = std::move(t);
                        }

                        if (NULLPTR == acceptor) {
                            LOG_ERROR("VEthernetLocalProxySwitcher::Open: cannot bind %s proxy listener on %s:%d",
                                proxy_only ? "proxy-only" : "local", address_string.data(), bind_port);
                            return false;
                        }
                    }

                    int sockfd = acceptor->GetHandle();
                    ppp::net::Socket::AdjustDefaultSocketOptional(sockfd, false);
                    ppp::net::Socket::SetTypeOfService(sockfd);
                    ppp::net::Socket::SetSignalPipeline(sockfd, false);
                    ppp::net::Socket::SetWindowSizeIfNotZero(sockfd, configuration->tcp.cwnd, configuration->tcp.rwnd);

                    auto self = shared_from_this();
                    acceptor->AcceptSocket = 
                        [self, this](ppp::net::SocketAcceptor*, ppp::net::SocketAcceptor::AcceptSocketEventArgs& e) noexcept {
                            int sockfd = e.Socket;
                            while (!disposed_) {
                                std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger();
                                if (NULLPTR == exchanger) {
                                    break;
                                }

                                ppp::threading::Executors::ContextPtr context;
                                ppp::threading::Executors::StrandPtr strand;
                                context = ppp::threading::Executors::SelectScheduler(strand);
                                
                                if (NULLPTR == context) {
                                    break;
                                }

                                return ppp::threading::Executors::Post(context, strand, 
                                    std::bind(&VEthernetLocalProxySwitcher::ProcessAcceptSocket, self, context, strand, sockfd));
                            }

                            ppp::net::Socket::Closesocket(sockfd);
                            return false;
                        };

                    bool bok = CreateAlwaysTimeout();
                    if (!bok) {
                        acceptor->Dispose();
                        return false;
                    }

                    acceptor_ = std::move(acceptor);
                    return bok;
                }

                void VEthernetLocalProxySwitcher::ReleaseConnection(VEthernetLocalProxyConnection* connection) noexcept {
                    if (NULLPTR != connection) {
                        auto self = shared_from_this();
                        std::shared_ptr<boost::asio::io_context> context = GetContext();
                        boost::asio::post(*context, 
                            [self, this, connection]() noexcept {
                                RemoveConnection(connection);
                            });
                    }
                }

                bool VEthernetLocalProxySwitcher::RemoveConnection(VEthernetLocalProxyConnection* connection) noexcept {
                    VEthernetLocalProxyConnectionPtr r; 
                    if (NULLPTR != connection) {
                        SynchronizedObjectScope scope(syncobj_);
                        r = ppp::collections::Dictionary::ReleaseObjectByKey(connections_, connection); 
                    }

                    return NULLPTR != r;
                }

                std::shared_ptr<boost::asio::ip::tcp::socket> VEthernetLocalProxySwitcher::NewSocket(const std::shared_ptr<boost::asio::io_context>& context, const ppp::threading::Executors::StrandPtr& strand, int sockfd) noexcept {
                    if (NULLPTR == context) {
                        return NULLPTR;
                    }

                    boost::asio::ip::tcp::endpoint remoteEP = ppp::net::Socket::GetRemoteEndPoint(sockfd);
                    boost::system::error_code ec = boost::asio::error::operation_aborted;

                    std::shared_ptr<boost::asio::ip::tcp::socket> socket = strand ?
                        make_shared_object<boost::asio::ip::tcp::socket>(*strand) : make_shared_object<boost::asio::ip::tcp::socket>(*context);
                    try {
                        if (NULLPTR == socket) {
                            return NULLPTR;
                        }
                        else {
                            socket->assign(remoteEP.protocol(), sockfd, ec);
                        }
                    }
                    catch (const std::exception&) {}

                    if (ec) {
                        ppp::net::Socket::Closesocket(sockfd);
                        return NULLPTR;
                    }
                    
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    if (NULLPTR == configuration) {
                        ppp::net::Socket::Closesocket(socket);
                        return NULLPTR;
                    }
                    ppp::net::Socket::AdjustDefaultSocketOptional(*socket, configuration->tcp.turbo);
                    ppp::net::Socket::SetWindowSizeIfNotZero(socket->native_handle(), configuration->tcp.cwnd, configuration->tcp.rwnd);
                    return socket;
                }

                bool VEthernetLocalProxySwitcher::AddConnection(const std::shared_ptr<VEthernetLocalProxyConnection>& connection) noexcept {
                    if (NULLPTR == connection) {
                        return false;
                    }
                    
                    SynchronizedObjectScope scope(syncobj_);
                    return ppp::collections::Dictionary::TryAdd(connections_, connection.get(), connection);
                }

                bool VEthernetLocalProxySwitcher::ProcessAcceptSocket(const std::shared_ptr<boost::asio::io_context>& context, const ppp::threading::Executors::StrandPtr& strand, int sockfd) noexcept {
                    if (NULLPTR == context) {
                        ppp::net::Socket::Closesocket(sockfd);
                        return false;
                    }

                    std::shared_ptr<boost::asio::ip::tcp::socket> socket = NewSocket(context, strand, sockfd);
                    if (NULLPTR == socket) {
                        return false;
                    }

                    std::shared_ptr<VEthernetLocalProxyConnection> connection = NewConnection(context, strand, socket);
                    if (NULLPTR == connection) {
                        return false;
                    }

                    bool bok = false;
                    for (;;) {
                        bok = AddConnection(connection);
                        if (!bok) {
                            break;
                        }

                        auto allocator = GetBufferAllocator();
                        auto self = shared_from_this();

                        bok = ppp::coroutines::YieldContext::Spawn(allocator.get(), *context, strand.get(),
                            [self, this, context, strand, connection](ppp::coroutines::YieldContext& y) noexcept {
                                bool bok = connection->Run(y);
                                if (!bok) {
                                    connection->Dispose();
                                }
                            });

                        break;
                    }
                    
                    if (!bok) {
                        if (RemoveConnection(connection.get())) {
                            connection->Dispose(); 
                        }
                    }

                    return bok;
                }

                std::shared_ptr<ppp::threading::BufferswapAllocator> VEthernetLocalProxySwitcher::GetBufferAllocator() noexcept {
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    return NULLPTR != configuration ? configuration->GetBufferAllocator() : NULLPTR;
                }

                bool VEthernetLocalProxySwitcher::CreateAlwaysTimeout() noexcept {
                    if (disposed_) {
                        return false;
                    }

                    auto self = shared_from_this();
                    auto timeout = make_shared_object<ppp::threading::Timer>(context_);
                    if (!timeout) {
                        return false;
                    }

                    timeout_ = timeout;
                    timeout->TickEvent = 
                        [self, this](ppp::threading::Timer* sender, ppp::threading::Timer::TickEventArgs& e) noexcept {
                            UInt64 now = ppp::threading::Executors::GetTickCount();
                            Update(now);
                        };
                    return timeout->SetInterval(1000) && timeout->Start();
                }

                boost::asio::ip::tcp::endpoint VEthernetLocalProxySwitcher::GetLocalEndPoint() noexcept {
                    std::shared_ptr<ppp::net::SocketAcceptor> acceptor = acceptor_;
                    if (NULLPTR != acceptor) {
                        return ppp::net::Socket::GetLocalEndPoint(acceptor->GetHandle());
                    }

                    return boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), ppp::net::IPEndPoint::MinPort);
                }
            }
        }
    }
}
