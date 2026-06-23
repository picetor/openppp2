#pragma once

#include <ppp/stdafx.h>
#include <ppp/net/IPEndPoint.h>

namespace ppp {
    namespace net {
        namespace asio {
            namespace internal {
                template <class TProtocol, class TIterator>
                boost::asio::ip::basic_endpoint<TProtocol>                      GetAddressByHostName(const TIterator& i, const TIterator& l, int port) noexcept {
                    TIterator tail = i;
                    TIterator endl = l;
                    for (; tail != endl; ++tail) {
                        boost::asio::ip::basic_endpoint<TProtocol> localEP = *tail;
                        boost::asio::ip::address localIP = localEP.address();
                        if (localIP.is_v4()) {
                            return localEP;
                        }
                    }

                    tail = i;
                    endl = l;
                    for (; tail != endl; ++tail) {
                        boost::asio::ip::basic_endpoint<TProtocol> localEP = *tail;
                        boost::asio::ip::address localIP = localEP.address();
                        if (localIP.is_v6()) {
                            return localEP;
                        }
                    }

                    return ppp::net::IPEndPoint::AnyAddressV4<TProtocol>(ppp::net::IPEndPoint::MinPort);
                }

                template <class TProtocol, class TResult>
                boost::asio::ip::basic_endpoint<TProtocol>                      GetAddressByHostName(const TResult& results, int port) noexcept {
                    if (results.empty()) {
                        return ppp::net::IPEndPoint::AnyAddressV4<TProtocol>(ppp::net::IPEndPoint::MinPort);
                    }

                    auto i = results.begin();
                    auto l = results.end();
                    return GetAddressByHostName<TProtocol>(i, l, port);
                }

                template <class TProtocol, class ResolveCall>
                boost::asio::ip::basic_endpoint<TProtocol>                      GetAddressByHostName(boost::asio::ip::basic_resolver<TProtocol>& resolver, const char* hostname, int port, ResolveCall&& resolver_resolve) noexcept {
                    typedef boost::asio::ip::basic_resolver<TProtocol> protocol_resolver;

                    if (NULLPTR == hostname || *hostname == '\x0') {
                        return ppp::net::IPEndPoint::AnyAddressV4<TProtocol>(ppp::net::IPEndPoint::MinPort);
                    }

                    boost::system::error_code ec;
                    ppp::string port_string = stl::to_string<ppp::string>(port);

                    typename protocol_resolver::results_type results;
                    try {
                        results = resolver_resolve(resolver, hostname, port_string.data(), ec);
                        if (ec) {
                            return ppp::net::IPEndPoint::AnyAddressV4<TProtocol>(ppp::net::IPEndPoint::MinPort);
                        }
                    }
                    catch (const std::exception&) {
                        return ppp::net::IPEndPoint::AnyAddressV4<TProtocol>(ppp::net::IPEndPoint::MinPort);
                    }

                    return GetAddressByHostName<TProtocol>(results, port);
                }
            }

            template <typename AsyncWriteStream, typename MutableBufferSequence>
            bool                                                                async_read(AsyncWriteStream& stream, const MutableBufferSequence& buffers, const boost::asio::yield_context& y) noexcept {
                if (!buffers.data() || !buffers.size()) {
                    return false;
                }

                boost::system::error_code ec;
                try {
                    std::size_t bytes_transferred = boost::asio::async_read(stream, constantof(buffers), y[ec]);
                    if (ec) {
                        return false;
                    }

                    return bytes_transferred == buffers.size();
                }
                catch (const std::exception&) {
                    return false;
                }
            }

            template <typename AsyncWriteStream, typename MutableBufferSequence>
            bool                                                                async_read_some(AsyncWriteStream& stream, const MutableBufferSequence& buffers, const boost::asio::yield_context& y) noexcept {
                if (!buffers.data() || !buffers.size()) {
                    return false;
                }

                boost::system::error_code ec;
                try {
                    std::size_t bytes_transferred = stream.async_read_some(constantof(buffers), y[ec]);
                    if (ec) {
                        return false;
                    }

                    return bytes_transferred > 0;
                }
                catch (const std::exception&) {
                    return false;
                }
            }

            template <typename AsyncWriteStream, typename ConstBufferSequence>
            bool                                                                async_write(AsyncWriteStream& stream, const ConstBufferSequence& buffers, const boost::asio::yield_context& y) noexcept {
                if (!buffers.data() || !buffers.size()) {
                    return false;
                }

                boost::system::error_code ec;
                try {
                    std::size_t bytes_transferred = boost::asio::async_write(stream, constantof(buffers), y[ec]);
                    if (ec) {
                        return false;
                    }
                    
                    return bytes_transferred == buffers.size();
                }
                catch (const std::exception&) {
                    return false;
                }
            }

            inline bool                                                         async_connect(boost::asio::ip::tcp::socket& socket, const boost::asio::ip::tcp::endpoint& remoteEP, const boost::asio::yield_context& y) noexcept {
                boost::asio::ip::address address = remoteEP.address();
                if (IPEndPoint::IsInvalid(address)) {
                    return false;
                }

                int port = remoteEP.port();
                if (port <= ppp::net::IPEndPoint::MinPort || port > ppp::net::IPEndPoint::MaxPort) {
                    return false;
                }

                boost::system::error_code ec;
                socket.async_connect(remoteEP, y[ec]);

                return ec == boost::system::errc::success; /* b is boost::system::errc::success. */
            }

            template <class TProtocol>
            boost::asio::ip::basic_endpoint<TProtocol>                          GetAddressByHostName(boost::asio::ip::basic_resolver<TProtocol>& resolver, const char* hostname, int port) noexcept {
                typedef boost::asio::ip::basic_resolver<TProtocol> protocol_resolver;

                return ppp::net::asio::internal::GetAddressByHostName(resolver, hostname, port,
                    [](protocol_resolver& resolver, const char* hostname, const char* port_string, boost::system::error_code& ec) noexcept {
                        return resolver.resolve(hostname, port_string, ec);
                    });
            }

            template <class TProtocol>
            boost::asio::ip::basic_endpoint<TProtocol>                          GetAddressByHostName(boost::asio::ip::basic_resolver<TProtocol>& resolver, const char* hostname, int port, const boost::asio::yield_context& y) noexcept {
                typedef boost::asio::ip::basic_resolver<TProtocol> protocol_resolver;

                return ppp::net::asio::internal::GetAddressByHostName(resolver, hostname, port,
                    [&y](protocol_resolver& resolver, const char* hostname, const char* port_string, boost::system::error_code& ec) noexcept {
                        return resolver.async_resolve(hostname, port_string, y[ec]);
                    });
            }
        }
    }
}