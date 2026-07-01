#pragma once

#include <ppp/app/client/proxys/VEthernetLocalProxyConnection.h>

namespace ppp {
    namespace app {
        namespace client {
            class VEthernetExchanger;

            namespace proxys {
                class VEthernetSocksProxySwitcher;

                class VEthernetSocksProxyConnection : public VEthernetLocalProxyConnection {
                public:
                    typedef std::shared_ptr<VEthernetSocksProxySwitcher>                VEthernetSocksProxySwitcherPtr;
                    typedef ppp::unordered_map<boost::asio::ip::udp::endpoint,
                        boost::asio::ip::udp::endpoint>                                 UdpAssociateEndpointTable;

                public:
                    VEthernetSocksProxyConnection(const VEthernetSocksProxySwitcherPtr& proxy,
                        const VEthernetExchangerPtr&                                    exchanger, 
                        const std::shared_ptr<boost::asio::io_context>&                 context,
                        const ppp::threading::Executors::StrandPtr&                     strand,
                        const std::shared_ptr<boost::asio::ip::tcp::socket>&            socket) noexcept;
                    virtual ~VEthernetSocksProxyConnection() noexcept;

                private:
                    int                                                                 SelectMethod(YieldContext& y, int& method) noexcept;
                    bool                                                                Replay(YieldContext& y, int k, int v) noexcept;
                    int                                                                 Authentication(YieldContext& y) noexcept;
                    int                                                                 Requirement(YieldContext& y, ppp::string& address, int& port, ppp::app::protocol::AddressType& address_type, int& command) noexcept;
                    bool                                                                OpenUdpAssociate(YieldContext& y) noexcept;
                    bool                                                                UdpAssociateLoopback(YieldContext& y) noexcept;
                    bool                                                                ForwardUdpAssociatePacket(const boost::asio::ip::udp::endpoint& source_ep, const boost::asio::ip::udp::endpoint& destination_ep, void* packet, int packet_length) noexcept;
                    bool                                                                SendUdpAssociatePacketToClient(const boost::asio::ip::udp::endpoint& source_endpoint, void* packet, int packet_length) noexcept;

                protected:
                    virtual bool                                                        Handshake(YieldContext& y) noexcept override;
                    virtual bool                                                        RunAfterHandshakeWithoutBridge(YieldContext& y) noexcept override;

                private:
                    std::shared_ptr<boost::asio::ip::udp::socket>                       udp_socket_;
                    std::shared_ptr<Byte>                                               udp_buffer_;
                    boost::asio::ip::udp::endpoint                                      udp_remote_ep_;
                    boost::asio::ip::udp::endpoint                                      udp_client_ep_;
                    UdpAssociateEndpointTable                                           udp_destination_clients_;
                };
            }
        }
    }
}