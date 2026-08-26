#pragma once

/**
 * @file LocalRpcServer.h
 * @brief Minimal local JSON-RPC-over-TCP server for the headless core.
 *
 * @details Serves a length-prefixed JSON frame protocol on a loopback TCP
 *          socket (see docs/RUST_TUI_DESIGN_CN.md §4).  The server owns
 *          transport only: it parses frames, authenticates the `hello`
 *          handshake with a token, and forwards every other request to a
 *          caller-provided handler callback.
 *
 *          Frame format: 4-byte big-endian length + JSON UTF-8 body.
 *          Message model: request/response with `id`, plus server-push
 *          notifications without `id` (reserved for future log events).
 *
 * @license GPL-3.0
 */

#include <ppp/stdafx.h>
#include <ppp/auxiliary/JsonAuxiliary.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/Socket.h>
#include <ppp/threading/Executors.h>

#include <boost/asio/ip/tcp.hpp>

namespace ppp {
    namespace app {
        namespace rpc {

            class LocalRpcServer final : public std::enable_shared_from_this<LocalRpcServer> {
            public:
                typedef std::shared_ptr<boost::asio::io_context>                    ContextPtr;
                typedef boost::asio::ip::tcp::socket                                AsioTcpSocket;
                typedef std::shared_ptr<AsioTcpSocket>                              AsioTcpSocketPtr;
                typedef boost::asio::ip::tcp::acceptor                              AsioTcpAcceptor;
                typedef std::shared_ptr<AsioTcpAcceptor>                            AsioTcpAcceptorPtr;
                typedef ppp::function<bool(const ppp::string& method, const Json::Value& params, Json::Value& result, ppp::string& error)> RequestHandler;

            public:
                LocalRpcServer(const ContextPtr& context, const ppp::string& token, int max_clients, const RequestHandler& handler) noexcept;
                ~LocalRpcServer() noexcept;

            public:
                // Bind/listen/accept. `listen` is "ip:port" or "[ipv6]:port";
                // port 0 picks a random free port (see GetLocalEndPoint).
                bool                                                            Open(const ppp::string& listen) noexcept;
                void                                                            Dispose() noexcept;
                bool                                                            IsDisposed() const noexcept { return disposed_.load(); }
                // Actual bound endpoint (valid after Open; random port resolved).
                boost::asio::ip::tcp::endpoint                                  GetLocalEndPoint() const noexcept { return local_endpoint_; }
                int                                                             GetClientCount() const noexcept { return client_count_.load(); }
                // Push a notification frame (no "id") to every authenticated
                // session.  Thread-safe; safe to call from log sink threads.
                void                                                            Broadcast(const Json::Value& frame) noexcept;

            private:
                class Session;
                typedef std::shared_ptr<Session>                                SessionPtr;
                typedef ppp::unordered_set<SessionPtr>                          SessionTable;

                void                                                            AcceptLoop() noexcept;
                void                                                            HandleAccept(const boost::system::error_code& ec, const AsioTcpSocketPtr& socket) noexcept;

            private:
                ContextPtr                                                      context_;
                ppp::string                                                     token_;
                int                                                             max_clients_ = 1;
                RequestHandler                                                  handler_;
                AsioTcpAcceptorPtr                                              acceptor_;
                boost::asio::ip::tcp::endpoint                                  local_endpoint_;
                SessionTable                                                    sessions_;
                std::mutex                                                      syncobj_;
                std::atomic<bool>                                               disposed_ = false;
                std::atomic<int>                                                client_count_ = 0;
            };

        }
    }
}
