#pragma once

#include <ppp/configurations/AppConfiguration.h>
#include <ppp/transmissions/ITransmission.h>
#include <ppp/ethernet/VNetstack.h>
#include <ppp/threading/Executors.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/net/rinetd/RinetdConnection.h>
#include <ppp/net/asio/IAsynchronousWriteIoQueue.h>

#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>

#include <ppp/app/protocol/VirtualEthernetLinklayer.h>
#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>

#include <ppp/app/mux/vmux_net.h>
#include <ppp/app/mux/vmux_skt.h>

namespace ppp {
    namespace app {
        namespace client {
            class VEthernetNetworkTcpipConnection : public ppp::ethernet::VNetstack::TapTcpClient {
            public:
                typedef ppp::app::protocol::VirtualEthernetTcpipConnection  VirtualEthernetTcpipConnection;
                typedef ppp::net::rinetd::RinetdConnection                  RinetdConnection;
                typedef ppp::configurations::AppConfiguration               AppConfiguration;

            public:
                VEthernetNetworkTcpipConnection(const std::shared_ptr<VEthernetExchanger>& exchanger, const std::shared_ptr<boost::asio::io_context>& context, const ppp::threading::Executors::StrandPtr& strand) noexcept;
                virtual ~VEthernetNetworkTcpipConnection() noexcept;

            public:
                std::shared_ptr<VEthernetExchanger>                         GetExchanger() noexcept { return exchanger_; }
                virtual void                                                Dispose() noexcept override;

            public:
                template <class TReference>
                static int                                                  Rinetd(
                    const std::shared_ptr<TReference>&                      reference,
                    const std::shared_ptr<VEthernetExchanger>&              exchanger,
                    const std::shared_ptr<boost::asio::io_context>&         context,
                    const ppp::threading::Executors::StrandPtr&             strand,
                    const std::shared_ptr<AppConfiguration>&                configuration,
                    const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket,
                    const boost::asio::ip::tcp::endpoint&                   remoteEP, 
                    std::shared_ptr<RinetdConnection>&                      out,
                    ppp::coroutines::YieldContext&                          y) noexcept {

                    std::shared_ptr<VEthernetNetworkSwitcher> switcher = exchanger->GetSwitcher();
                    if (NULLPTR == switcher) {
                        return -1;
                    }

                    boost::asio::ip::address remote_address = remoteEP.address();
                    bool bypass_ip_address_ok = false;
                    if (remote_address.is_v4()) {
                        bypass_ip_address_ok = switcher->IsBypassIpAddress(remote_address);
                    }
                    elif (remote_address.is_v6()) {
                        bypass_ip_address_ok = switcher->IsBypassIpAddress6(remote_address);
                    }
                    if (!bypass_ip_address_ok) {
                        return 1;
                    }

                    class VEthernetRinetdConnection final : public RinetdConnection {
                    public:
                        VEthernetRinetdConnection(
                            const std::shared_ptr<TReference>&                              owner,
                            const std::shared_ptr<ppp::configurations::AppConfiguration>&   configuration, 
                            const std::shared_ptr<boost::asio::io_context>&                 context, 
                            const ppp::threading::Executors::StrandPtr&                     strand,
                            const std::shared_ptr<boost::asio::ip::tcp::socket>&            local_socket) noexcept 
                                : RinetdConnection(configuration, context, strand, local_socket)
                                , owner_(owner) {

                            }
                        virtual ~VEthernetRinetdConnection() noexcept {
                            Finalize();
                        }

                    public:
                        virtual void                                                        Dispose() noexcept override {
                            RinetdConnection::Dispose();
                        }
                        virtual void                                                        Update() noexcept override {
                            std::shared_ptr<TReference> owner = owner_;
                            if (NULLPTR != owner) {
                                owner->Update();
                            }
                        }

                    private:
                        void                                                                Finalize() noexcept {
                            std::shared_ptr<TReference> owner = std::move(owner_);
                            if (NULLPTR != owner) {
                                owner->Dispose();
                            }
                        }

                    private:
                        std::shared_ptr<TReference>                                         owner_;
                    };

                    std::shared_ptr<VEthernetRinetdConnection> connection_rinetd = 
                        make_shared_object<VEthernetRinetdConnection>(reference, configuration, context, strand, socket);
                    if (NULLPTR == connection_rinetd) {
                        return -1;
                    }

#if defined(_LINUX)
                    connection_rinetd->ProtectorNetwork = switcher->GetProtectorNetwork();
#endif

                    bool run_ok = connection_rinetd->Open(remoteEP, y);
                    if (!run_ok) {
                        return -1;
                    }

                    out = std::move(connection_rinetd);
                    return 0;
                }

                template <class TReference>
                static int                                                  Mux(
                    const std::shared_ptr<TReference>&                      reference,
                    const std::shared_ptr<VEthernetExchanger>&              exchanger,
                    const char*                                             source,
                    const void*                                             trace,
                    const ppp::string&                                      host,
                    const int                                               port,
                    const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket,
                    std::shared_ptr<vmux::vmux_skt>&                        out,
                    ppp::coroutines::YieldContext&                          y) noexcept {

                    typedef VEthernetExchanger::NetworkState NetworkState;
                    typedef std::shared_ptr<vmux::vmux_skt> VmuxSktPtr;

                    LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: source=%s, trace=%p, outbound=%s, enter, host=%s, port=%d",
                        NULLPTR == source ? "unknown" : source, trace, exchanger->GetOutboundTag().data(), host.data(), port);

                    if (auto mux = exchanger->GetMux(); NULLPTR != mux) {
                        auto network_state = exchanger->GetMuxNetworkState();
                        LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: mux present, state=%d, established=%d, disposed=%d, host=%s, port=%d",
                            (int)network_state, (int)mux->is_established(), (int)mux->is_disposed(), host.data(), port);
                        if (network_state != NetworkState::NetworkState_Established && !mux->is_disposed()) {
                            int64_t deadline = ppp::threading::Executors::GetTickCount() + std::max<int>(1000, exchanger->GetConfiguration()->mux.connect.timeout);
                            for (;;) {
                                if (exchanger->GetMux().get() != mux.get() || mux->is_disposed()) {
                                    LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: mux changed while waiting, host=%s, port=%d", host.data(), port);
                                    return 1;
                                }

                                network_state = exchanger->GetMuxNetworkState();
                                if (network_state == NetworkState::NetworkState_Established) {
                                    LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: mux became established, host=%s, port=%d", host.data(), port);
                                    break;
                                }

                                if (static_cast<uint64_t>(ppp::threading::Executors::GetTickCount()) >= static_cast<uint64_t>(deadline)) {
                                    LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: wait established timeout, state=%d, host=%s, port=%d",
                                        (int)network_state, host.data(), port);
                                    return 1;
                                }

                                if (!exchanger->Sleep(10, reference->GetContext(), y)) {
                                    LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: wait interrupted, host=%s, port=%d", host.data(), port);
                                    return -1;
                                }
                            }
                        }

                        if (network_state == NetworkState::NetworkState_Established) {
                            std::shared_ptr<VmuxSktPtr> pmux_connection = make_shared_object<VmuxSktPtr>();
                            if (NULLPTR == pmux_connection) {
                                LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: allocation failed, host=%s, port=%d", host.data(), port);
                                return -1;
                            }

                            if (!mux->connect_yield(
                                y, 
                                reference->GetContext(),
                                reference->GetStrand(),
                                socket, 
                                host,
                                port,
                                pmux_connection)) {
                                LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: connect_yield failed, host=%s, port=%d", host.data(), port);
                                return -1;
                            }
                            else {
                                reference->Update();
                            }
                            
                            VmuxSktPtr mux_connection = *pmux_connection;
                            if (NULLPTR == mux_connection) {
                                LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: connect_yield returned null, host=%s, port=%d", host.data(), port);
                                return -1;
                            }

                            mux_connection->disposed_event = 
                                [reference](vmux::vmux_skt*) noexcept {
                                    reference->Dispose();
                                };
                            mux_connection->active_event = 
                                [reference](vmux::vmux_skt*, bool success) noexcept {
                                    if (success) {
                                        reference->Update();
                                    }
                                    else {
                                        reference->Dispose();
                                    }
                                };

                            out = mux_connection;
                            LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: connected, host=%s, port=%d", host.data(), port);
                            return 0;
                        }
                    }
                    else {
                        LOG_DEBUG("VEthernetNetworkTcpipConnection::Mux: source=%s, trace=%p, outbound=%s, no mux, host=%s, port=%d",
                            NULLPTR == source ? "unknown" : source, trace, exchanger->GetOutboundTag().data(), host.data(), port);
                    }

                    return 1;
                }

                template <class TReference>
                static int                                                  Mux(
                    const std::shared_ptr<TReference>&                      reference,
                    const std::shared_ptr<VEthernetExchanger>&              exchanger,
                    const char*                                             source,
                    const void*                                             trace,
                    const boost::asio::ip::tcp::endpoint&                   remoteEP, 
                    const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket,
                    std::shared_ptr<vmux::vmux_skt>&                        out,
                    ppp::coroutines::YieldContext&                          y) noexcept {

                    ppp::string host = ppp::net::Ipep::ToAddressString<ppp::string>(remoteEP);
                    return Mux(reference, exchanger, source, trace, host, remoteEP.port(), socket, out, y); /* https://www.youtube.com/watch?v=FdScisAHKBE */
                }

            protected:
                virtual bool                                                Establish() noexcept override;
                virtual bool                                                BeginAccept() noexcept override;
                virtual bool                                                EndAccept(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket, const boost::asio::ip::tcp::endpoint& natEP) noexcept override;

            private:
                void                                                        Finalize() noexcept;
                void                                                        ReleaseActiveTransmission() noexcept;
                bool                                                        Loopback(ppp::coroutines::YieldContext& y) noexcept;
                bool                                                        ConnectToPeer(ppp::coroutines::YieldContext& y) noexcept;
                bool                                                        Spawn(const ppp::function<bool(ppp::coroutines::YieldContext&)>& coroutine) noexcept;

            private:
                std::shared_ptr<VEthernetExchanger>                         exchanger_;
                std::shared_ptr<VirtualEthernetTcpipConnection>             connection_;
                std::shared_ptr<RinetdConnection>                           connection_rinetd_;
                std::shared_ptr<vmux::vmux_skt>                             connection_mux_;                       
            };
        }
    }
}
