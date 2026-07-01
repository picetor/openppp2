#include <ppp/app/protocol/VirtualEthernetTcpipConnection.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/client/VEthernetNetworkTcpipConnection.h>
#include <ppp/app/client/proxys/VEthernetSocksProxySwitcher.h>
#include <ppp/app/client/proxys/VEthernetSocksProxyConnection.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/Socket.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/diagnostics/Error.h>

namespace ppp {
    namespace app {
        namespace client {
            namespace proxys {
                static constexpr int SOCKS_VER                  = 5;
                static constexpr int SOCKS_METHOD_NONE          = 0;
                static constexpr int SOCKS_METHOD_AUTH          = 2;
                static constexpr int SOCKS_METHOD_RSVD          = 255;
                static constexpr int SOCKS_ERR_ER               = -1;
                static constexpr int SOCKS_ERR_OK               = 0;
                static constexpr int SOCKS_ERR_NO               = 1;
                static constexpr int SOCKS_ERR_CMD              = 7;
                static constexpr int SOCKS_ERR_ATYPE            = 8;
                static constexpr int SOCKS_ERR_FF               = 255;
                static constexpr int SOCKS_PROTO_AUTH           = 1;
                static constexpr int SOCKS_ATYPE_IPV4           = 1;
                static constexpr int SOCKS_ATYPE_IPV6           = 4;
                static constexpr int SOCKS_ATYPE_DOMAIN         = 3;
                static constexpr int SOCKS_CMD_CONNECT          = 1;
                static constexpr int SOCKS_CMD_UDP              = 3;
                static constexpr int SOCKS_ERSV                = 0;
                static constexpr int SOCKS_ERR_RSV             = 2;
                static constexpr int SOCKS_UDP_MIN_PACKET_SIZE  = 10;

                static int PublishSocketReadFailure(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept {
                    if (NULLPTR == socket) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SessionTransportMissing);
                    }
                    elif(socket->is_open()) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketReadFailed);
                    }

                    return SOCKS_ERR_ER;
                }

                static bool PublishSocketWriteFailure(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) noexcept {
                    if (NULLPTR == socket) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::SessionTransportMissing);
                    }
                    elif(socket->is_open()) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::SocketWriteFailed);
                    }

                    return false;
                }

                VEthernetSocksProxyConnection::VEthernetSocksProxyConnection(
                    const VEthernetSocksProxySwitcherPtr&                           proxy,
                    const VEthernetExchangerPtr&                                    exchanger, 
                    const std::shared_ptr<boost::asio::io_context>&                 context,
                    const ppp::threading::Executors::StrandPtr&                     strand,
                    const std::shared_ptr<boost::asio::ip::tcp::socket>&            socket) noexcept 
                    : VEthernetLocalProxyConnection(proxy, exchanger, context, strand, socket) {
                        
                }

                static bool SendSocksRequestReply(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket, Byte rep, ppp::coroutines::YieldContext& y) noexcept {
                    if (NULLPTR == socket) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::SessionTransportMissing);
                    }

                    if (!socket->is_open()) {
                        return false;
                    }

                    Byte data[32];
                    int packet_length = 0;
                    data[packet_length++] = SOCKS_VER;
                    data[packet_length++] = rep;
                    data[packet_length++] = 0;

                    boost::system::error_code ec;
                    boost::asio::ip::tcp::endpoint local_endpoint = socket->local_endpoint(ec);
                    if (ec) {
                        local_endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), 0);
                    }
                    else {
                        local_endpoint = ppp::net::Ipep::V6ToV4(local_endpoint);
                    }

                    boost::asio::ip::address local_ip = local_endpoint.address();
                    if (local_ip.is_v4()) {
                        data[packet_length++] = SOCKS_ATYPE_IPV4;
                        auto bytes = local_ip.to_v4().to_bytes();
                        memcpy(data + packet_length, bytes.data(), bytes.size());
                        packet_length += bytes.size();
                    }
                    elif(local_ip.is_v6()) {
                        data[packet_length++] = SOCKS_ATYPE_IPV6;
                        auto bytes = local_ip.to_v6().to_bytes();
                        memcpy(data + packet_length, bytes.data(), bytes.size());
                        packet_length += bytes.size();
                    }
                    else {
                        data[packet_length++] = SOCKS_ATYPE_IPV4;
                        memset(data + packet_length, 0, 4);
                        packet_length += 4;
                    }

                    int local_port = local_endpoint.port();
                    data[packet_length++] = (Byte)(local_port >> 8);
                    data[packet_length++] = (Byte)(local_port);

                    bool ok = ppp::coroutines::asio::async_write(*socket, boost::asio::buffer(data, packet_length), y);
                    return ok ? true : PublishSocketWriteFailure(socket);
                }

                VEthernetSocksProxyConnection::~VEthernetSocksProxyConnection() noexcept {
                    VEthernetExchangerPtr exchanger = GetExchanger();
                    if (NULLPTR != exchanger && udp_client_ep_.port() > ppp::net::IPEndPoint::MinPort) {
                        exchanger->ReleaseDatagramHandler(udp_client_ep_);
                    }

                    ppp::net::Socket::Closesocket(udp_socket_);
                }

                static bool SendSocksRequestReply(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket, Byte rep, const boost::asio::ip::udp::endpoint& bind_endpoint, ppp::coroutines::YieldContext& y) noexcept {
                    if (NULLPTR == socket) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::SessionTransportMissing);
                    }

                    if (!socket->is_open()) {
                        return false;
                    }

                    Byte data[32];
                    int packet_length = 0;
                    data[packet_length++] = SOCKS_VER;
                    data[packet_length++] = rep;
                    data[packet_length++] = 0;

                    boost::asio::ip::address local_ip = bind_endpoint.address();
                    if (local_ip.is_v4()) {
                        data[packet_length++] = SOCKS_ATYPE_IPV4;
                        auto bytes = local_ip.to_v4().to_bytes();
                        memcpy(data + packet_length, bytes.data(), bytes.size());
                        packet_length += bytes.size();
                    }
                    elif(local_ip.is_v6()) {
                        data[packet_length++] = SOCKS_ATYPE_IPV6;
                        auto bytes = local_ip.to_v6().to_bytes();
                        memcpy(data + packet_length, bytes.data(), bytes.size());
                        packet_length += bytes.size();
                    }
                    else {
                        data[packet_length++] = SOCKS_ATYPE_IPV4;
                        memset(data + packet_length, 0, 4);
                        packet_length += 4;
                    }

                    int local_port = bind_endpoint.port();
                    data[packet_length++] = (Byte)(local_port >> 8);
                    data[packet_length++] = (Byte)(local_port);

                    bool ok = ppp::coroutines::asio::async_write(*socket, boost::asio::buffer(data, packet_length), y);
                    return ok ? true : PublishSocketWriteFailure(socket);
                }
                
                bool VEthernetSocksProxyConnection::Handshake(YieldContext& y) noexcept {
                    int method = SOCKS_METHOD_NONE;
                    int status = SelectMethod(y, method); 
                    if (status <= SOCKS_ERR_ER) {
                        return false;
                    }
                    elif(status >= SOCKS_ERR_NO) {
                        Replay(y, SOCKS_VER, SOCKS_METHOD_RSVD);
                        return false;
                    }
                    elif(!Replay(y, SOCKS_VER, method)) {
                        return false;
                    }
                    elif(method == SOCKS_METHOD_AUTH) {
                        status = Authentication(y);
                        if (status <= SOCKS_ERR_ER) {
                            return false;
                        }
                        elif(status >= SOCKS_ERR_NO) {
                            Replay(y, SOCKS_PROTO_AUTH, SOCKS_ERR_FF);
                            return false;
                        }
                        elif(!Replay(y, SOCKS_PROTO_AUTH, SOCKS_ERR_OK)) {
                            return false;
                        }
                    }

                    int port = ppp::net::IPEndPoint::MinPort;
                    int command = SOCKS_CMD_CONNECT;
                    ppp::string host;
                    ppp::app::protocol::AddressType address_type = ppp::app::protocol::AddressType::Domain;

                    int command_status = Requirement(y, host, port, address_type, command);
                    if (command_status != SOCKS_ERR_OK) {
                        if (command_status == SOCKS_ERR_ATYPE) {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksAddressTypeUnsupported);
                        }
                        elif(command_status == SOCKS_ERR_CMD) {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksCommandUnsupported);
                        }
                        elif(command_status > SOCKS_ERR_OK) {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketAddressInvalid);
                        }

                        SendSocksRequestReply(GetSocket(), (Byte)command_status, y);
                        return false;
                    }

                    if (command == SOCKS_CMD_UDP) {
                        if (!OpenUdpAssociate(y)) {
                            SendSocksRequestReply(GetSocket(), SOCKS_ERR_NO, y);
                            return false;
                        }

                        boost::system::error_code ec;
                        boost::asio::ip::udp::endpoint local_endpoint = udp_socket_->local_endpoint(ec);
                        if (ec) {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketAddressInvalid);
                            SendSocksRequestReply(GetSocket(), SOCKS_ERR_NO, y);
                            return false;
                        }

                        return SendSocksRequestReply(GetSocket(), SOCKS_ERR_OK, local_endpoint, y);
                    }

                    std::shared_ptr<ppp::app::protocol::AddressEndPoint> address_endpoint = make_shared_object<ppp::app::protocol::AddressEndPoint>();
                    if (NULLPTR == address_endpoint) {
                        return false;
                    }

                    address_endpoint->Type = address_type;
                    address_endpoint->Host = host;
                    address_endpoint->Port = port;

                    if (!ConnectBridgeToPeer(address_endpoint, y)) {
                        return false;
                    }

                    return SendSocksRequestReply(GetSocket(), SOCKS_ERR_OK, y);
                }

                int VEthernetSocksProxyConnection::Authentication(YieldContext& y) noexcept {
                    std::shared_ptr<boost::asio::ip::tcp::socket>& socket = GetSocket();
                    if (NULLPTR == socket || !socket->is_open()) {
                        return SOCKS_ERR_ER;
                    }

                    if (IsDisposed()) {
                        return SOCKS_ERR_ER;
                    }

                    AppConfigurationPtr& configuration = GetConfiguration();
                    auto& socks_proxy = configuration->client.socks_proxy;

                    Byte data[256];
                    if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 1), y)) {
                        return PublishSocketReadFailure(socket);
                    }

                    if (data[0] != SOCKS_PROTO_AUTH) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AuthChallengeFailed);
                        return SOCKS_ERR_NO;
                    }

                    ppp::string strings[2];
                    for (int i = 0; i < arraysizeof(strings); i++) {
                        if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 1), y)) {
                            return PublishSocketReadFailure(socket);
                        }

                        int string_size = data[0];
                        if (string_size > 0) {
                            if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, string_size), y)) {
                                return PublishSocketReadFailure(socket);
                            }

                            data[string_size] = '\x0';
                            strings[i] = reinterpret_cast<char*>(data);
                        }
                    }

                    if (socks_proxy.username != strings[0] || socks_proxy.password != strings[1]) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::AuthCredentialInvalid);
                        return SOCKS_ERR_NO;
                    }

                    return SOCKS_ERR_OK;
                }

                bool VEthernetSocksProxyConnection::Replay(YieldContext& y, int k, int v) noexcept {
                    std::shared_ptr<boost::asio::ip::tcp::socket>& socket = GetSocket();
                    if (NULLPTR == socket || !socket->is_open()) {
                        return false;
                    }

                    if (IsDisposed()) {
                        return false;
                    }

                    Byte data[2] = { (Byte)k, (Byte)v };
                    bool ok = ppp::coroutines::asio::async_write(*socket, boost::asio::buffer(data, sizeof(data)), y);
                    return ok ? true : PublishSocketWriteFailure(socket);
                }

                int VEthernetSocksProxyConnection::SelectMethod(YieldContext& y, int& method) noexcept {
                    std::shared_ptr<boost::asio::ip::tcp::socket>& socket = GetSocket();
                    if (NULLPTR == socket || !socket->is_open()) {
                        return SOCKS_ERR_ER;
                    }

                    if (IsDisposed()) {
                        return SOCKS_ERR_ER;
                    }

                    Byte data[256];
                    if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 2), y)) {
                        return PublishSocketReadFailure(socket);
                    }

                    if (data[0] != SOCKS_VER) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksVersionInvalid);
                        return SOCKS_ERR_NO;
                    }

                    int n_methods = data[1];
                    if (n_methods < 1) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksMethodInvalid);
                        return SOCKS_ERR_NO;
                    }

                    if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, n_methods), y)) {
                        return PublishSocketReadFailure(socket);
                    }

                    method = SOCKS_METHOD_RSVD;
                    for (int i = 0; i < n_methods; i++) {
                        if (data[i] == SOCKS_METHOD_AUTH) {
                            method = SOCKS_METHOD_AUTH;
                            break;
                        }
                        elif(data[i] == SOCKS_METHOD_NONE) {
                            method = SOCKS_METHOD_NONE;
                        }
                    }

                    if (method == SOCKS_METHOD_RSVD) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksMethodInvalid);
                    }

                    return SOCKS_ERR_OK;
                }
            
                int VEthernetSocksProxyConnection::Requirement(YieldContext& y, ppp::string& address, int& port, ppp::app::protocol::AddressType& address_type, int& command) noexcept {
                    std::shared_ptr<boost::asio::ip::tcp::socket>& socket = GetSocket();
                    address.clear();

                    port = ppp::net::IPEndPoint::MinPort;
                    address_type = ppp::app::protocol::AddressType::Domain;
                    command = SOCKS_CMD_CONNECT;

                    if (NULLPTR == socket || !socket->is_open()) {
                        return SOCKS_ERR_ER;
                    }

                    if (IsDisposed()) {
                        return SOCKS_ERR_ER;
                    }
                    
                    Byte cmd = SOCKS_ERR_CMD;
                    Byte data[256];

                    for (;;) {
                        if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 4), y)) {
                            return PublishSocketReadFailure(socket);
                        }

                        int nver = data[0];
                        if (nver != SOCKS_VER) {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksVersionInvalid);
                            return SOCKS_ERR_NO;
                        }

                        cmd = data[1];
                        if (cmd == SOCKS_CMD_CONNECT || cmd == SOCKS_CMD_UDP) {
                            command = cmd;
                        }
                        else {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksCommandUnsupported);
                            return SOCKS_ERR_CMD;
                        }

                        if (data[2] != SOCKS_ERSV) {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksReservedFieldInvalid);
                            return SOCKS_ERR_RSV;
                        }

                        Byte atype = data[3];
                        if (atype == SOCKS_ATYPE_IPV4) {
                            if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 6), y)) {
                                return PublishSocketReadFailure(socket);
                            }

                            port = boost::asio::detail::socket_ops::network_to_host_short(*(short*)(data + 4));
                            boost::asio::ip::address_v4::bytes_type ip = *(boost::asio::ip::address_v4::bytes_type*)data;
                            address = boost::asio::ip::address_v4(ip).to_string();
                            address_type = ppp::app::protocol::AddressType::IPv4;
                            return SOCKS_ERR_OK;
                        }
                        elif(atype == SOCKS_ATYPE_IPV6) {
                            if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 18), y)) {
                                return PublishSocketReadFailure(socket);
                            }

                            port = boost::asio::detail::socket_ops::network_to_host_short(*(short*)(data + 16));
                            boost::asio::ip::address_v6::bytes_type ip = *(boost::asio::ip::address_v6::bytes_type*)data;
                            address = boost::asio::ip::address_v6(ip).to_string();
                            address_type = ppp::app::protocol::AddressType::IPv6;
                            return SOCKS_ERR_OK;
                        }
                        elif(atype == SOCKS_ATYPE_DOMAIN) {
                            if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, 1), y)) {
                                return PublishSocketReadFailure(socket);
                            }

                            int address_length = data[0];
                            if (address_length < 1) {
                                ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocketAddressInvalid);
                                return SOCKS_ERR_NO;
                            }

                            if (!ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(data, address_length + 2), y)) {
                                return PublishSocketReadFailure(socket);
                            }

                            port = boost::asio::detail::socket_ops::network_to_host_short(*(short*)(data + address_length));
                            data[address_length] = '\x0';
                            address = (char*)data;
                            address_type = ppp::app::protocol::AddressType::Domain;
                            return SOCKS_ERR_OK;
                        }
                        else {
                            ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::SocksAddressTypeUnsupported);
                            return SOCKS_ERR_ATYPE;
                        }
                    }
                }

                bool VEthernetSocksProxyConnection::OpenUdpAssociate(YieldContext& y) noexcept {
                    udp_client_ep_ = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::any(), ppp::net::IPEndPoint::MinPort);

                    boost::system::error_code ec;
                    udp_socket_ = make_shared_object<boost::asio::ip::udp::socket>(*GetContext());
                    if (NULLPTR == udp_socket_) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::UdpOpenFailed);
                        return false;
                    }

                    udp_socket_->open(boost::asio::ip::udp::v4(), ec);
                    if (ec) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::UdpOpenFailed);
                        return false;
                    }

                    udp_socket_->bind(udp_client_ep_, ec);
                    if (ec) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::UdpOpenFailed);
                        return false;
                    }

                    udp_client_ep_ = udp_socket_->local_endpoint(ec);
                    if (ec) {
                        udp_client_ep_ = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::any(), ppp::net::IPEndPoint::MinPort);
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::UdpOpenFailed);
                        return false;
                    }

                    ppp::net::Socket::AdjustSocketOptional(*udp_socket_, true);

                    VEthernetExchangerPtr exchanger = GetExchanger();
                    if (NULLPTR != exchanger) {
                        exchanger->RegisterDatagramHandler(udp_client_ep_, 
                            std::bind(&VEthernetSocksProxyConnection::ForwardUdpAssociatePacket,
                                std::static_pointer_cast<VEthernetSocksProxyConnection>(shared_from_this()),
                                std::placeholders::_1,
                                std::placeholders::_2,
                                std::placeholders::_3,
                                std::placeholders::_4));
                    }

                    udp_buffer_ = Executors::GetCachedBuffer(GetContext());
                    if (NULLPTR == udp_buffer_) {
                        return true;
                    }

                    return UdpAssociateLoopback(y);
                }

                bool VEthernetSocksProxyConnection::UdpAssociateLoopback(YieldContext& y) noexcept {
                    if (NULLPTR == udp_socket_ || !udp_socket_->is_open()) {
                        return false;
                    }

                    if (NULLPTR == udp_buffer_) {
                        return false;
                    }

                    while (udp_socket_->is_open()) {
                        boost::system::error_code ec;
                        int datagram_length = 0;
                        boost::asio::post(udp_socket_->get_executor(),
                            [this, &y, &ec, &datagram_length]() noexcept {
                                udp_socket_->async_receive_from(
                                    boost::asio::buffer(udp_buffer_.get(), PPP_BUFFER_SIZE),
                                    udp_remote_ep_,
                                    [&y, &ec, &datagram_length](const boost::system::error_code& e, std::size_t sz) noexcept {
                                        ec = e;
                                        datagram_length = std::max<int>(e ? -1 : static_cast<int>(sz), -1);
                                        y.R();
                                    });
                            });

                        y.Suspend();
                        if (ec) {
                            return false;
                        }

                        if (datagram_length < SOCKS_UDP_MIN_PACKET_SIZE) {
                            continue;
                        }

                        Byte* p = udp_buffer_.get();
                        // p[0..1] = RSV, p[2] = FRAG
                        Byte atype = p[3];

                        boost::asio::ip::udp::endpoint destination_ep;
                        int header_length = 4;

                        if (atype == SOCKS_ATYPE_IPV4) {
                            if (datagram_length < header_length + 4 + 2) {
                                continue;
                            }
                            boost::asio::ip::address_v4::bytes_type ip;
                            memcpy(ip.data(), p + header_length, 4);
                            header_length += 4;
                            destination_ep.address(boost::asio::ip::address_v4(ip));
                        }
                        elif(atype == SOCKS_ATYPE_IPV6) {
                            if (datagram_length < header_length + 16 + 2) {
                                continue;
                            }
                            boost::asio::ip::address_v6::bytes_type ip;
                            memcpy(ip.data(), p + header_length, 16);
                            header_length += 16;
                            destination_ep.address(boost::asio::ip::address_v6(ip));
                        }
                        elif(atype == SOCKS_ATYPE_DOMAIN) {
                            int domain_length = p[header_length];
                            header_length++;
                            if (datagram_length < header_length + domain_length + 2) {
                                continue;
                            }
                            p[header_length + domain_length] = '\x0';
                            destination_ep.address(
                                boost::asio::ip::make_address((char*)(p + header_length), ec));
                            if (ec) {
                                continue;
                            }
                            header_length += domain_length;
                        }
                        else {
                            continue;
                        }

                        int dst_port = (p[header_length] << 8) | p[header_length + 1];
                        header_length += 2;
                        destination_ep.port(dst_port);

                        VEthernetExchangerPtr exchanger = GetExchanger();
                        if (NULLPTR == exchanger) {
                            continue;
                        }

                        exchanger->SendTo(ppp::net::Ipep::V6ToV4(udp_client_ep_),
                            ppp::net::Ipep::V6ToV4(destination_ep),
                            udp_buffer_.get() + header_length,
                            datagram_length - header_length);
                    }

                    return true;
                }

                bool VEthernetSocksProxyConnection::ForwardUdpAssociatePacket(
                    const boost::asio::ip::udp::endpoint& source_ep, 
                    const boost::asio::ip::udp::endpoint& destination_ep, 
                    void* packet, int packet_length) noexcept {
                    if (NULLPTR == packet || packet_length < 1) {
                        return false;
                    }

                    if (udp_client_ep_.port() != source_ep.port()) {
                        return false;
                    }

                    return SendUdpAssociatePacketToClient(destination_ep, packet, packet_length);
                }

                bool VEthernetSocksProxyConnection::SendUdpAssociatePacketToClient(
                    const boost::asio::ip::udp::endpoint& source_endpoint,
                    void* packet,
                    int packet_length) noexcept {
                    if (NULLPTR == packet || packet_length < 1) {
                        return false;
                    }

                    if (NULLPTR == udp_socket_ || !udp_socket_->is_open()) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::SessionTransportMissing);
                    }

                    boost::asio::ip::udp::endpoint source_ep = ppp::net::Ipep::V6ToV4(source_endpoint);
                    boost::asio::ip::address source_ip = source_ep.address();

                    Byte buffer[2048];
                    Byte* p = buffer;

                    *p++ = 0; // RSV
                    *p++ = 0; // RSV
                    *p++ = 0; // FRAG

                    if (source_ip.is_v4()) {
                        *p++ = SOCKS_ATYPE_IPV4;
                        auto bytes = source_ip.to_v4().to_bytes();
                        memcpy(p, bytes.data(), bytes.size());
                        p += bytes.size();
                    }
                    elif(source_ip.is_v6()) {
                        *p++ = SOCKS_ATYPE_IPV6;
                        auto bytes = source_ip.to_v6().to_bytes();
                        memcpy(p, bytes.data(), bytes.size());
                        p += bytes.size();
                    }
                    else {
                        *p++ = SOCKS_ATYPE_IPV4;
                        memset(p, 0, 4);
                        p += 4;
                    }

                    *p++ = (Byte)(source_ep.port() >> 8);
                    *p++ = (Byte)(source_ep.port());

                    int header_length = (int)(p - buffer);
                    int total_length = header_length + packet_length;
                    if (total_length > (int)sizeof(buffer)) {
                        return false;
                    }

                    memcpy(p, packet, packet_length);

                    boost::system::error_code ec;
                    udp_socket_->send_to(boost::asio::buffer(buffer, total_length), udp_remote_ep_, 0, ec);
                    return !ec;
                }

                bool VEthernetSocksProxyConnection::RunAfterHandshakeWithoutBridge(YieldContext& y) noexcept {
                    if (NULLPTR == udp_socket_ || !udp_socket_->is_open()) {
                        return false;
                    }

                    return UdpAssociateLoopback(y);
                }
            }
        }
    }
}