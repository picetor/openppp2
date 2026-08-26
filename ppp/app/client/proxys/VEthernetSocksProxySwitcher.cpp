#include <ppp/app/client/proxys/VEthernetSocksProxySwitcher.h>
#include <ppp/app/client/proxys/VEthernetSocksProxyConnection.h>
#include <ppp/app/client/VEthernetExchanger.h>
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
                VEthernetSocksProxySwitcher::VEthernetSocksProxySwitcher(const std::shared_ptr<VEthernetExchanger>& exchanger) noexcept 
                    : VEthernetLocalProxySwitcher(exchanger) {

                }
                
                std::shared_ptr<VEthernetLocalProxyConnection> VEthernetSocksProxySwitcher::NewConnection(const std::shared_ptr<boost::asio::io_context>& context, const ppp::threading::Executors::StrandPtr& strand, const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept {
                    std::shared_ptr<VEthernetSocksProxySwitcher> self = std::dynamic_pointer_cast<VEthernetSocksProxySwitcher>(shared_from_this());
                    std::shared_ptr<VEthernetExchanger> exchanger = GetExchanger();

                    auto connection = make_shared_object<VEthernetSocksProxyConnection>(self, exchanger, context, strand, socket);
                    if (connection && exchanger) {
                        LOG_DEBUG("VEthernetSocksProxySwitcher::NewConnection: source=socks, trace=%p, transport_trace=%p, selected_outbound=%s",
                            connection.get(), strand.get(), exchanger->GetOutboundTag().data());
                    }
                    return connection;
                }

                boost::asio::ip::address VEthernetSocksProxySwitcher::MyLocalEndPoint(int& bind_port) noexcept {
                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration_ = GetConfiguration();
                    bind_port = configuration_->client.socks_proxy.port;
                    if (configuration_->client.socks_proxy.bind.empty()) {
                        // Keep the default listener loopback-only but dual-stack.
                        return boost::asio::ip::address_v6::loopback();
                    }

                    boost::system::error_code ec;
                    boost::asio::ip::address address = StringToAddress(configuration_->client.socks_proxy.bind.data(), ec);
                    if (ec) {
                        bind_port = ppp::net::IPEndPoint::MinPort;
                    }
                    return address;
                }
            }
        }
    }
}
