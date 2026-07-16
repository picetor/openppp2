#include <ppp/net/SocketAcceptor.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/Socket.h>
#include <ppp/threading/Executors.h>
#include <common/unix/net/UnixSocketAcceptor.h>

#if defined(_MACOS)
#include <cerrno>
#endif

namespace ppp
{
    namespace net
    {
        static bool ShouldLogSocketAcceptorError() noexcept
        {
#if defined(PPP_LOG_VERBOSE)
            static std::atomic<uint64_t> next_log_time = 0;
            uint64_t now = ppp::threading::Executors::GetTickCount();
            uint64_t expected = next_log_time.load();
            return now >= expected && next_log_time.compare_exchange_strong(expected, now + 1000);
#else
            return false;
#endif
        }

        UnixSocketAcceptor::UnixSocketAcceptor() noexcept
            : server_(NULLPTR)
            , context_(ppp::threading::Executors::GetDefault())
            , in_(false)
        {

        }

        UnixSocketAcceptor::~UnixSocketAcceptor() noexcept
        {
            Finalize();
        }

        bool UnixSocketAcceptor::IsOpen() noexcept
        {
            std::shared_ptr<boost::asio::io_context> context = context_;
            if (NULLPTR == context)
            {
                return false;
            }

            std::shared_ptr<boost::asio::ip::tcp::acceptor> server = server_;
            if (NULLPTR == server)
            {
                return false;
            }

            return server->is_open();
        }

        int UnixSocketAcceptor::GetHandle() noexcept
        {
            std::shared_ptr<boost::asio::ip::tcp::acceptor> server = server_;
            if (NULLPTR == server)
            {
                return -1;
            }

            return server->native_handle();
        }

        bool UnixSocketAcceptor::Open(const char* localIP, int localPort, int backlog) noexcept
        {
            if (localPort < IPEndPoint::MinPort || localPort > IPEndPoint::MaxPort)
            {
                return false;
            }

            if (NULLPTR == localIP || *localIP == '\x0')
            {
                return false;
            }

            std::shared_ptr<boost::asio::io_context> context = context_;
            if (NULLPTR == context)
            {
                return false;
            }

            if (NULLPTR != server_)
            {
                return false;
            }

            if (backlog < 1)
            {
                backlog = PPP_LISTEN_BACKLOG;
            }

            boost::system::error_code ec;
            boost::asio::ip::address address = StringToAddress(localIP, ec);
            if (ec)
            {
                return false;
            }

            server_ = make_shared_object<boost::asio::ip::tcp::acceptor>(*context);
            if (NULLPTR == server_)
            {
                return false;
            }
            
            bool any = false;
            boost::asio::ip::address bind_ips[] = { address, boost::asio::ip::address_v4::any(), boost::asio::ip::address_v6::any() };
            for (boost::asio::ip::address& bind_ip : bind_ips) 
            {
                any = Socket::OpenAcceptor(*server_, bind_ip, localPort, backlog, false, false);
                if (any)
                {
                    in_ = bind_ip.is_v4();
                    break;
                }

                server_->close(ec);
                if (ec)
                {
                    return false;
                }
            }

            any = any && Next();
            return any;
        }

        void UnixSocketAcceptor::Dispose() noexcept
        {
            std::shared_ptr<boost::asio::io_context> context = context_;
            if (NULLPTR != context)
            {
                auto self = shared_from_this();
                boost::asio::post(*context, 
                    [self, this, context]() noexcept
                    {
                        Finalize();
                    });
            }
        }

        bool UnixSocketAcceptor::Next() noexcept
        {
            std::shared_ptr<boost::asio::ip::tcp::acceptor> server = server_;
            if (NULLPTR == server)
            {
                return false;
            }

            std::shared_ptr<boost::asio::io_context> context = context_;
            if (NULLPTR == context)
            {
                return false;
            }

            std::shared_ptr<boost::asio::ip::tcp::socket> socket = make_shared_object<boost::asio::ip::tcp::socket>(*context);
            if (NULLPTR == socket)
            {
                return false;
            }

            std::shared_ptr<SocketAcceptor> self = shared_from_this();
            server->async_accept(*socket, 
                [self, this, server, socket, context](boost::system::error_code ec) noexcept
                {
                    if (ec == boost::system::errc::operation_canceled) /* WSAWaitForMultipleEvents */
                    {
                        return;
                    }

                    if (ec && ShouldLogSocketAcceptorError()) {
                        LOG_DEBUG("UnixSocketAcceptor::Next: async_accept failed, native=%lld, ec=%d, category=%s, message=%s",
                            (long long)server->native_handle(), ec.value(), ec.category().name(), ec.message().data());
                    }

#if defined(_MACOS)
                    if (ec.value() == EMFILE || ec.value() == ENFILE)
                    {
                        // Retrying accept immediately while the descriptor table is full creates
                        // a tight error loop that can starve the whole io_context. Pause briefly
                        // so existing connections have time to close and release descriptors.
                        Socket::Closesocket(socket);
                        std::shared_ptr<boost::asio::deadline_timer> retry =
                            make_shared_object<boost::asio::deadline_timer>(*context);
                        if (retry)
                        {
                            retry->expires_from_now(boost::posix_time::milliseconds(250));
                            retry->async_wait(
                                [self, this, retry](const boost::system::error_code& timer_ec) noexcept
                                {
                                    if (!timer_ec)
                                    {
                                        Next();
                                    }
                                });
                        }
                        return;
                    }
#endif

                    /* This function always fails with operation_not_supported when used on Windows versions prior to Windows 8.1. */
#if defined(_WIN32)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
                    int sockfd = socket->release(ec); // os < microsoft windows 8.1 is not supported.

#if defined(_WIN32)
#pragma warning(pop)
#endif
                    if (ec)
                    {
                        if (ShouldLogSocketAcceptorError()) {
                            LOG_DEBUG("UnixSocketAcceptor::Next: socket release failed, ec=%d, category=%s, message=%s",
                                ec.value(), ec.category().name(), ec.message().data());
                        }
                        sockfd = -1;
                    }
                    else
                    {
                        Socket::AdjustDefaultSocketOptional(sockfd, in_);
                        Socket::SetTypeOfService(sockfd);
                        Socket::SetSignalPipeline(sockfd, false);
                        Socket::ReuseSocketAddress(sockfd, true);
                    }

                    Socket::Closesocket(socket);
                    if (sockfd != -1)
                    {
                        AcceptSocketEventArgs e = { sockfd };
                        OnAcceptSocket(e);
                    }

                    Next();
                });
            return true;
        }

        void UnixSocketAcceptor::Finalize() noexcept
        {
            std::shared_ptr<boost::asio::ip::tcp::acceptor> server = std::move(server_);
            if (NULLPTR != server)
            {
                Socket::Closesocket(server);
            }

            server_ = NULLPTR;
            context_ = NULLPTR;
        }
    }
}
