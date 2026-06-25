#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>
#include <ppp/app/protocol/templates/TVEthernetTcpipConnection.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/Socket.h>

#include <ppp/threading/Executors.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>

namespace ppp {
    namespace app {
        namespace protocol {
            class STATIC_VIRTUAL_ETHERNET_TCPIP_CONNECTOR_NEST final : public VirtualEthernetLinklayer {
                friend class VirtualEthernetTcpipConnection;

            public:
                STATIC_VIRTUAL_ETHERNET_TCPIP_CONNECTOR_NEST(
                    VirtualEthernetTcpipConnection*                         connection,
                    const AppConfigurationPtr&                              configuration,
                    const ContextPtr&                                       context,
                    const Int128&                                           id) noexcept
                    : VirtualEthernetLinklayer(configuration, context, id)
                    , ConnectId(0)
                    , ConnectOK(false)
                    , ErrorCode(0)
                    , Connect(false)
                    , Sequence(0)
                    , Acknowledge(0)
                    , Vlan(0)
                    , MuxON(false)
                    , connection_(connection) {

                }

            public:
                // PROTOCOL: PREPARED_CONNECT  CONNECT  CONNECT_OK
                int                                                         ConnectId;

                // PROTOCOL: PREPARED_CONNECT
                ppp::string                                                 Host;

                // PROTOCOL: CONNECT
                bool                                                        Connect;
                boost::asio::ip::tcp::endpoint                              Destination;

                // PROTOCOL: CONNECT_OK
                bool                                                        ConnectOK;
                Byte                                                        ErrorCode;

                // PROTOCOL: MUXON
                uint32_t                                                    Sequence;
                uint32_t                                                    Acknowledge;
                uint16_t                                                    Vlan;
                bool                                                        MuxON;

            public:
                virtual std::shared_ptr<ppp::net::Firewall>                 GetFirewall() noexcept {
                    return connection_->GetFirewall();
                }
                virtual bool                                                OnPreparedConnect(const ITransmissionPtr& transmission, int connection_id, const ppp::string& destinationHost, const boost::asio::ip::tcp::endpoint& destinationEP, YieldContext& y) noexcept override {
                    Host = destinationHost;
                    return true;
                }
                virtual bool                                                OnConnect(const ITransmissionPtr& transmission, int connection_id, const boost::asio::ip::tcp::endpoint& destinationEP, YieldContext& y) noexcept override {
                    Connect = true;
                    ConnectId = connection_id;
                    Destination = destinationEP;
                    return true;
                }
                virtual bool                                                OnConnectOK(const ITransmissionPtr& transmission, int connection_id, Byte error_code, YieldContext& y) noexcept override {
                    ConnectOK = true;
                    ErrorCode = error_code;
                    ConnectId = connection_id;
                    return true;
                }
                virtual bool                                                OnMuxON(const ITransmissionPtr& transmission, uint16_t vlan, uint32_t seq, uint32_t ack, YieldContext& y) noexcept override {
                    MuxON = true;
                    Vlan = vlan;
                    Sequence = seq;
                    Acknowledge = ack;
                    return true;
                }

            private:
                VirtualEthernetTcpipConnection* const                       connection_;
            };

            VirtualEthernetTcpipConnection::VirtualEthernetTcpipConnection(
                const AppConfigurationPtr&                              configuration,
                const ContextPtr&                                       context,
                const StrandPtr&                                        strand,
                const Int128&                                           id,
                const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket) noexcept
                : disposed_(false)
                , connected_(false)
                , configuration_(configuration)
                , context_(context)
                , strand_(strand)
                , id_(id)
                , socket_(socket) {

                if (NULLPTR != socket) {
#if defined(_WIN32)
                    if (ppp::net::Socket::IsDefaultFlashTypeOfService()) {
                        if (socket->is_open()) {
                            qoss_ = ppp::net::QoSS::New(socket->native_handle());
                        }
                    }
#endif
                    ppp::net::Socket::SetWindowSizeIfNotZero(socket->native_handle(), configuration->tcp.cwnd, configuration->tcp.rwnd);
                }
            }

            bool VirtualEthernetTcpipConnection::Connect(
                YieldContext&       y, 
                ITransmissionPtr&   transmission, 
                const ppp::string&  host, 
                int                 port) noexcept {
                    
                return MuxOrConnect(y, transmission, host, port, 0, 0, 0, false);
            }

            bool VirtualEthernetTcpipConnection::ConnectMux(
                YieldContext&       y, 
                ITransmissionPtr&   transmission, 
                uint32_t            vlan, 
                uint32_t            seq, 
                uint32_t            ack) noexcept {

                ppp::string default_host;
                int default_port = ppp::net::IPEndPoint::MinPort;

                return MuxOrConnect(y, transmission, default_host, default_port, vlan, seq, ack, true);
            }

            bool VirtualEthernetTcpipConnection::MuxOrConnect(
                YieldContext&       y, 
                ITransmissionPtr&   transmission, 
                const ppp::string&  host, 
                int                 port, 
                uint32_t            vlan, 
                uint32_t            seq, 
                uint32_t            ack, 
                bool                mux_or_connect) noexcept {

                typedef VirtualEthernetLinklayer::ERROR_CODES ERROR_CODES;

                if (NULLPTR == transmission) {
                    LOG_DEBUG("VETcpip::MuxOrConnect: transmission is null");
                    return false;
                }

                if (disposed_) {
                    LOG_DEBUG("VETcpip::MuxOrConnect: disposed");
                    return false;
                }

                if (connected_) {
                    LOG_DEBUG("VETcpip::MuxOrConnect: already connected");
                    return false;
                }

                if (!mux_or_connect) {
                    if (!socket_) {
                        LOG_DEBUG("VETcpip::MuxOrConnect: socket is null for connect mode");
                        return false;
                    }
                }

                Update();

                auto connector = make_shared_object<STATIC_VIRTUAL_ETHERNET_TCPIP_CONNECTOR_NEST>(this, configuration_, context_, id_);
                if (NULLPTR == connector) {
                    LOG_DEBUG("VETcpip::MuxOrConnect: failed to create connector");
                    return false;
                }
                else {
                    bool connector_dook = false;
                    if (mux_or_connect) {
                        connector_dook = connector->DoMuxON(transmission, vlan, seq, ack, y);
                    }
                    else {
                        connector_dook = connector->DoConnect(transmission, RandomNext(1, INT_MAX), host, port, y);
                    }

                    if (!connector_dook) {
                        LOG_DEBUG("VETcpip::MuxOrConnect: connector operation failed, mux_or_connect=%d, host=%s, port=%d",
                            mux_or_connect, host.data(), port);
                        return false;
                    }
                }

                int packet_size = 0;
                std::shared_ptr<Byte> packet = transmission->Read(y, packet_size);
                if (NULLPTR == packet || packet_size < 1) {
                    LOG_DEBUG("VETcpip::MuxOrConnect: read failed after connector, mux_or_connect=%d", mux_or_connect);
                    return false;
                }

                if (!connector->PacketInput(transmission, packet.get(), packet_size, y)) {
                    LOG_DEBUG("VETcpip::MuxOrConnect: PacketInput failed");
                    return false;
                }

                if (mux_or_connect) {
                    if (!connector->MuxON) {
                        LOG_DEBUG("VETcpip::MuxOrConnect: MuxON flag not set");
                        return false;
                    }

                    if (connector->Vlan != vlan || connector->Sequence != seq || connector->Acknowledge != ack) {
                        LOG_DEBUG("VETcpip::MuxOrConnect: mux params mismatch, vlan=%u/%u, seq=%u/%u, ack=%u/%u",
                            connector->Vlan, vlan, connector->Sequence, seq, connector->Acknowledge, ack);
                        return false;
                    }
                }
                else {
                    if (!connector->ConnectOK) {
                        LOG_DEBUG("VETcpip::MuxOrConnect: ConnectOK not set");
                        return false;
                    }

                    ERROR_CODES err = (ERROR_CODES)connector->ErrorCode;
                    if (err != ERROR_CODES::ERRORS_SUCCESS) {
                        LOG_DEBUG("VETcpip::MuxOrConnect: connect error=%d", (int)err);
                        return false;
                    }
                }

                connected_ = true;
                transmission_ = transmission;

                LOG_DEBUG("VETcpip::MuxOrConnect: success, mux_or_connect=%d, host=%s, port=%d",
                    mux_or_connect, host.data(), port);
                Update();
                return true;
            }

            bool VirtualEthernetTcpipConnection::Accept(
                YieldContext&                                           y, 
                ITransmissionPtr&                                       transmission, 
                const VirtualEthernetLoggerPtr&                         logger,
                const AcceptMuxAsynchronousCallback&                    mux) noexcept {

                return MuxOrAccept(y, transmission, logger, mux, false);
            }

            bool VirtualEthernetTcpipConnection::AcceptMux(
                YieldContext&                           y, 
                ITransmissionPtr&                       transmission, 
                const AcceptMuxAsynchronousCallback&    ac) noexcept {

                if (NULLPTR == ac) {
                    return false;
                }

                return MuxOrAccept(y, transmission, NULLPTR, ac, true);
            }

            bool VirtualEthernetTcpipConnection::MuxOrAccept(
                YieldContext&                                           y, 
                ITransmissionPtr&                                       transmission, 
                const VirtualEthernetLoggerPtr&                         logger,
                const AcceptMuxAsynchronousCallback&                    accept_mux_ac, 
                bool                                                    mux_or_connect) noexcept {

                typedef VirtualEthernetLinklayer::ERROR_CODES ERROR_CODES;

                if (NULLPTR == transmission) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: transmission is null");
                    return false;
                }

                if (disposed_) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: disposed");
                    return false;
                }

                if (connected_) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: already connected");
                    return false;
                }

                if (!socket_) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: socket is null");
                    return false;
                }

                Update();

                int packet_size = -1;
                std::shared_ptr<Byte> packet = transmission->Read(y, packet_size);
                if (NULLPTR == packet || packet_size < 1) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: read failed");
                    return false;
                }

                auto connector = make_shared_object<STATIC_VIRTUAL_ETHERNET_TCPIP_CONNECTOR_NEST>(this, configuration_, context_, id_);
                if (NULLPTR == connector) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: failed to create connector");
                    return false;
                }

                if (!connector->PacketInput(transmission, packet.get(), packet_size, y)) {
                    LOG_DEBUG("VETcpip::MuxOrAccept: PacketInput failed");
                    return false;
                }

                if (mux_or_connect) {
                LABEL_MUXON:
                    if (!connector->MuxON) {
                        LOG_DEBUG("VETcpip::MuxOrAccept: MuxON not set");
                        return false;
                    }

                    connected_ = true;
                    transmission_ = transmission;
                    Update();

                    bool ok = accept_mux_ac(connector->Vlan, connector->Sequence, connector->Acknowledge);
                    if (!ok) {
                        LOG_DEBUG("VETcpip::MuxOrAccept: accept_mux_ac callback failed");
                        return false;
                    }

                    LOG_DEBUG("VETcpip::MuxOrAccept: mux accept success, vlan=%u, seq=%u, ack=%u",
                        connector->Vlan, connector->Sequence, connector->Acknowledge);
                }
                else {
                    boost::asio::ip::tcp::endpoint& destinationEP = connector->Destination;
                    if (!connector->Connect) {
                        if (NULLPTR != accept_mux_ac) {
                            goto LABEL_MUXON;
                        }

                        return false;
                    }

                    boost::system::error_code ec;
                    socket_->open(destinationEP.protocol(), ec);
                    if (ec) {
                        return false;
                    }

                    boost::asio::ip::address destinationIP = destinationEP.address();
#if defined(_WIN32)
                    if (ppp::net::Socket::IsDefaultFlashTypeOfService()) {
                        int destinationPort = destinationEP.port();
                        qoss_ = ppp::net::QoSS::New(socket_->native_handle(), destinationIP, destinationPort);
                    }
#elif defined(_LINUX)
                    // If IPV4 is not a loop IP address, it needs to be linked to a physical network adapter. 
                    // IPV6 does not need to be linked, because VPN is IPV4, 
                    // And IPV6 does not affect the physical layer network communication of the VPN.
                    if (!destinationIP.is_loopback()) {
                        auto protector_network = ProtectorNetwork;
                        if (NULLPTR != protector_network) {
                            if (!protector_network->Protect(socket_->native_handle(), y)) {
                                return false;
                            }
                        }
                    }
#endif

                    std::shared_ptr<ppp::configurations::AppConfiguration> configuration = GetConfiguration();
                    ppp::net::Socket::SetWindowSizeIfNotZero(socket_->native_handle(), configuration->tcp.cwnd, configuration->tcp.rwnd);
                    ppp::net::Socket::AdjustSocketOptional(*socket_, destinationIP.is_v4(), configuration->tcp.fast_open, configuration->tcp.turbo);

                    bool ok = ppp::coroutines::asio::async_connect(*socket_, destinationEP, y);
                    if (NULLPTR != logger) {
                        logger->Connect(GetId(), transmission, socket_->local_endpoint(ec), destinationEP, connector->Host);
                    }

                    if (disposed_) {
                        connector->DoConnectOK(transmission, connector->ConnectId, ERROR_CODES::ERRORS_CONNECT_CANCEL, y);
                        return false;
                    }

                    if (ok) {
                        ok = connector->DoConnectOK(transmission, connector->ConnectId, ERROR_CODES::ERRORS_SUCCESS, y);
                        if (!ok) {
                            return false;
                        }
                    }
                    else {
                        connector->DoConnectOK(transmission, connector->ConnectId, ERROR_CODES::ERRORS_CONNECT_TO_DESTINATION, y);
                        return false;
                    }

                    connected_ = true;
                    transmission_ = transmission;
                    Update();

                    LOG_DEBUG("VETcpip::MuxOrAccept: connect accept success, host=%s, port=%d",
                        connector->Host.data(), connector->Destination.port());
                }

                return true;
            }

            void VirtualEthernetTcpipConnection::Finalize() noexcept {
                ITransmissionPtr transmission = std::move(transmission_); 
                if (NULLPTR != transmission) {
                    transmission->Dispose();
                }

#if defined(_WIN32)
                qoss_.reset();
#endif

                disposed_ = true;
                connected_ = false;

                ppp::net::Socket::Closesocket(socket_);
            }

            void VirtualEthernetTcpipConnection::Clear() noexcept {
                connected_ = false;

#if defined(_WIN32)
                qoss_.reset();
#endif

                socket_.reset();
                transmission_.reset();
            }

            VirtualEthernetTcpipConnection::~VirtualEthernetTcpipConnection() noexcept {
                Finalize();
            }

            void VirtualEthernetTcpipConnection::Dispose() noexcept {
                auto self = shared_from_this();
                ppp::threading::Executors::ContextPtr context = context_;
                ppp::threading::Executors::StrandPtr strand = strand_;

                ppp::threading::Executors::Post(context, strand,
                    [self, this, context, strand]() noexcept {
                        Finalize();
                    });
            }

            bool VirtualEthernetTcpipConnection::Run(YieldContext& y) noexcept {
                if (!ReceiveTransmissionToSocket()) {
                    LOG_DEBUG("VETcpip::Run: ReceiveTransmissionToSocket failed");
                    return false;
                }

                Update();
                LOG_DEBUG("VETcpip::Run: starting ForwardTransmissionToSocket");
                return ForwardTransmissionToSocket(y);
            }

            bool VirtualEthernetTcpipConnection::SendBufferToPeer(YieldContext& y, const void* packet, int packet_length) noexcept {
                if (NULLPTR == packet || packet_length < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                if (!connected_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                return transmission->Write(y, packet, packet_length);
            }

            bool VirtualEthernetTcpipConnection::ForwardSocketToTransmission(const std::shared_ptr<Byte>& buffer, int buffer_size, int bytes_transferred) noexcept {
                if (NULLPTR == buffer || buffer_size < 1 || bytes_transferred < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                if (!connected_) {
                    return false;
                }

                ITransmissionPtr transmission = transmission_;
                if (NULLPTR == transmission) {
                    return false;
                }

                auto self = shared_from_this();
                return transmission->Write(buffer.get(), bytes_transferred, 
                    [self, this, buffer, buffer_size](bool ok) noexcept {
                        ForwardSocketToTransmissionOK(ok, buffer, buffer_size);
                    });
            }

            bool VirtualEthernetTcpipConnection::ReceiveSocketToTransmission(const std::shared_ptr<Byte>& buffer, int buffer_size) noexcept {
                if (NULLPTR == buffer || buffer_size < 1) {
                    return false;
                }

                if (disposed_) {
                    return false;
                }

                if (!connected_) {
                    return false;
                }

                auto self = shared_from_this();
                boost::asio::post(socket_->get_executor(),
                    [self, this, buffer, buffer_size]() noexcept {
                        int bytes_transferred = BufferSkateboarding(configuration_->key.sb, buffer_size, PPP_BUFFER_SIZE);
                        socket_->async_read_some(boost::asio::buffer(buffer.get(), bytes_transferred),
                            [self, this, buffer, buffer_size](const boost::system::error_code& ec, std::size_t sz) noexcept {
                                int bytes_transferred = std::max<int>(ec ? -1 : static_cast<int>(sz), -1);
                                if (bytes_transferred < 1) {
                                    Dispose();
                                }
                                elif(ForwardSocketToTransmission(buffer, buffer_size, bytes_transferred)) {
                                    Update();
                                }
                                else {
                                    Dispose();
                                }
                            });
                    });
                return true;
            }

            bool VirtualEthernetTcpipConnection::ReceiveTransmissionToSocket() noexcept {
                if (disposed_) {
                    return false;
                }

                if (!connected_) {
                    return false;
                }

                auto allocator = configuration_->GetBufferAllocator();
                auto buffer = ppp::threading::BufferswapAllocator::MakeByteArray(allocator, PPP_BUFFER_SIZE);
                if (NULLPTR == buffer) {
                    return false;
                }

                return ReceiveSocketToTransmission(buffer, PPP_BUFFER_SIZE);
            }

            bool VirtualEthernetTcpipConnection::ForwardTransmissionToSocket(YieldContext& y) noexcept {
                if (!connected_) {
                    LOG_DEBUG("VETcpip::ForwardTransmissionToSocket: not connected");
                    return false;
                }

                if (disposed_) {
                    LOG_DEBUG("VETcpip::ForwardTransmissionToSocket: disposed");
                    return false;
                }

                bool any = false;
                while (!disposed_) {
                    ITransmissionPtr transmission = transmission_;
                    if (NULLPTR == transmission) {
                        LOG_DEBUG("VETcpip::ForwardTransmissionToSocket: transmission is null");
                        break;
                    }

                    int packet_length = 0;
                    std::shared_ptr<Byte> packet = transmission->Read(y, packet_length);
                    if (NULLPTR == packet || packet_length < 1) {
                        LOG_DEBUG("VETcpip::ForwardTransmissionToSocket: read returned empty, packet_length=%d", packet_length);
                        break;
                    }

                    any = true;
                    Update();

                    bool ok = ppp::coroutines::asio::async_write(*socket_, boost::asio::buffer(packet.get(), packet_length), y);
                    if (ok) {
                        Update();
                    }
                    else {
                        LOG_DEBUG("VETcpip::ForwardTransmissionToSocket: async_write failed");
                        break;
                    }
                }

                LOG_DEBUG("VETcpip::ForwardTransmissionToSocket: exiting, any=%d, disposed=%d", any, disposed_);
                Dispose();
                return any;
            }
        }
    }
}