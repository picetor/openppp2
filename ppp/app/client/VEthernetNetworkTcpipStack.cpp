#include <ppp/app/client/VEthernetNetworkTcpipStack.h>
#include <ppp/app/client/VEthernetNetworkTcpipConnection.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>

#include <ppp/IDisposable.h>
#include <ppp/net/Ipep.h>
#include <ppp/threading/Executors.h>

namespace ppp {
    namespace app {
        namespace client {
            VEthernetNetworkTcpipStack::VEthernetNetworkTcpipStack(const std::shared_ptr<VEthernetNetworkSwitcher>& ethernet) noexcept
                : VNetstack()
                , Ethernet(ethernet)
                , configuration_(ethernet->GetConfiguration()) {

            }

            std::shared_ptr<VEthernetNetworkTcpipStack::TapTcpClient> VEthernetNetworkTcpipStack::BeginAcceptClient(const boost::asio::ip::tcp::endpoint& localEP, const boost::asio::ip::tcp::endpoint& remoteEP) noexcept {
                using NetworkState = VEthernetExchanger::NetworkState;

                std::shared_ptr<VEthernetNetworkSwitcher> ethernet = this->Ethernet;
                if (NULLPTR == ethernet) {
                    return NULLPTR;
                }

                std::shared_ptr<VEthernetExchanger> exchanger = ethernet->GetExchanger(remoteEP.address());
                if (NULLPTR == exchanger) {
                    LOG_DEBUG("VEthernetNetworkTcpipStack::BeginAcceptClient: source=tap, destination=%s, port=%u, selected_outbound=direct_or_unavailable",
                        ppp::net::Ipep::ToAddressString<ppp::string>(remoteEP).data(),
                        static_cast<unsigned int>(remoteEP.port()));
                    return NULLPTR;
                }

                NetworkState network_state = exchanger->GetNetworkState();
                if (network_state != NetworkState::NetworkState_Established) {
                    LOG_DEBUG("VEthernetNetworkTcpipStack::BeginAcceptClient: source=tap, destination=%s, port=%u, selected_outbound=%s, reason=outbound_not_established",
                        ppp::net::Ipep::ToAddressString<ppp::string>(remoteEP).data(),
                        static_cast<unsigned int>(remoteEP.port()), exchanger->GetOutboundTag().data());
                    return NULLPTR;
                }
                
                ppp::threading::Executors::ContextPtr context;
                ppp::threading::Executors::StrandPtr strand;
                context = ppp::threading::Executors::SelectScheduler(strand);

                if (NULLPTR == context) {
                    return NULLPTR;
                }

                auto connection = make_shared_object<VEthernetNetworkTcpipConnection>(exchanger, context, strand);
                if (NULLPTR == connection) {
                    return NULLPTR;
                }

                LOG_DEBUG("VEthernetNetworkTcpipStack::BeginAcceptClient: source=tap, trace=%p, transport_trace=%p, destination=%s, port=%u, selected_outbound=%s",
                    connection.get(), strand.get(), ppp::net::Ipep::ToAddressString<ppp::string>(remoteEP).data(),
                    static_cast<unsigned int>(remoteEP.port()), exchanger->GetOutboundTag().data());
                connection->Open(localEP, remoteEP);
                return connection;
            }

            uint64_t VEthernetNetworkTcpipStack::GetMaxConnectTimeout() noexcept {
                uint64_t tcp_connect_timeout = (uint64_t)configuration_->tcp.connect.timeout;
                return (tcp_connect_timeout + 1) * 1000;
            }

            uint64_t VEthernetNetworkTcpipStack::GetMaxEstablishedTimeout() noexcept {
                uint64_t tcp_inactive_timeout = (uint64_t)configuration_->tcp.inactive.timeout;
                return (tcp_inactive_timeout + 1) * 1000;
            }
        }
    }
}
