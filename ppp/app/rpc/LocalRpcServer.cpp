// LocalRpcServer.cpp
#include <ppp/app/rpc/LocalRpcServer.h>

#include <ppp/threading/BufferswapAllocator.h>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <deque>

namespace ppp {
    namespace app {
        namespace rpc {

            namespace {
                static constexpr int  RPC_MAX_FRAME_SIZE    = 4 * 1024 * 1024; // 4 MiB body cap
                static constexpr int  RPC_PROTOCOL_SCHEMA   = 1;
                static constexpr int  RPC_HANDSHAKE_TIMEOUT = 10000;            // ms
            }

            class LocalRpcServer::Session final : public std::enable_shared_from_this<Session> {
            public:
                Session(const std::shared_ptr<LocalRpcServer>& server, const AsioTcpSocketPtr& socket) noexcept
                    : server_(server), socket_(socket), strand_(socket->get_executor()) { }

            public:
                void Start() noexcept {
                    LOG_DEBUG("LocalRpcServer::Session::Start: session started, remote=%s",
                        socket_->remote_endpoint().address().to_string().data());
                    boost::asio::post(strand_,
                        [self = shared_from_this()]() noexcept
                        {
                            self->ReadFrameLength();
                        });
                }

                void Dispose() noexcept {
                    LOG_DEBUG("LocalRpcServer::Session::Dispose: closing session, remote=%s",
                        socket_->is_open() ? socket_->remote_endpoint().address().to_string().data() : "closed");
                    boost::system::error_code ec;
                    socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                    socket_->close(ec);
                }

                // Write one frame to the peer (responses and notifications).
                // All writes are serialized through the strand: concurrent
                // async_write calls on one socket interleave bytes, which
                // corrupts the length-prefixed framing for the peer.
                void SendFrame(const Json::Value& frame) noexcept {
                    boost::asio::post(strand_,
                        [self = shared_from_this(), frame]() noexcept
                        {
                            self->EnqueueWrite(frame);
                        });
                }

            private:
                struct FrameBuffer {
                    std::shared_ptr<Byte>                                   data;
                    int                                                     size = 0;
                };

                void EnqueueWrite(const Json::Value& frame) noexcept {
                    ppp::string json = ppp::auxiliary::JsonAuxiliary::ToString(frame);
                    if (json.size() > RPC_MAX_FRAME_SIZE) return;

                    // NOTE: make_shared_object<Byte>(N) would allocate only
                    // sizeof(Byte) and corrupt the heap on the memcpy below;
                    // MakeByteArray allocates the full N bytes.
                    std::shared_ptr<Byte> buffer =
                        ppp::threading::BufferswapAllocator::MakeByteArray(NULLPTR, 4 + (int)json.size());
                    if (NULLPTR == buffer) return;

                    uint32_t length = (uint32_t)json.size();
                    buffer.get()[0] = (Byte)(length >> 24);
                    buffer.get()[1] = (Byte)(length >> 16);
                    buffer.get()[2] = (Byte)(length >> 8);
                    buffer.get()[3] = (Byte)length;
                    std::memcpy(buffer.get() + 4, json.data(), json.size());

                    write_queue_.push_back(FrameBuffer{ buffer, 4 + (int)json.size() });
                    LOG_DEBUG("LocalRpcServer::Session::EnqueueWrite: queued, size=%d, queue=%zu, writing=%d",
                        (int)json.size(), write_queue_.size(), (int)writing_);
                    if (!writing_)
                    {
                        WriteNext();
                    }
                }

                void WriteNext() noexcept {
                    if (write_queue_.empty())
                    {
                        writing_ = false;
                        return;
                    }
                    writing_ = true;

                    FrameBuffer frame = write_queue_.front();
                    LOG_DEBUG("LocalRpcServer::Session::WriteNext: begin, size=%d, queue=%zu",
                        frame.size, write_queue_.size());
                    boost::asio::async_write(*socket_,
                        boost::asio::buffer(frame.data.get(), frame.size),
                        boost::asio::bind_executor(strand_,
                            [self = shared_from_this(), frame](const boost::system::error_code& ec, std::size_t written) noexcept
                            {
                                LOG_DEBUG("LocalRpcServer::Session::WriteNext: done, written=%zu, ec=%s, queue=%zu",
                                    written, ec.message().data(), self->write_queue_.size());
                                self->write_queue_.pop_front();
                                if (ec)
                                {
                                    LOG_DEBUG("LocalRpcServer::Session::WriteNext: write failed, ec=%s", ec.message().data());
                                    self->Dispose();
                                    return;
                                }
                                self->WriteNext();
                            }));
                }

            private:
                void ReadFrameLength() noexcept {
                    if (server_->disposed_.load()) return;

                    std::shared_ptr<Byte> length_buffer =
                        ppp::threading::BufferswapAllocator::MakeByteArray(NULLPTR, 4);
                    if (NULLPTR == length_buffer) { Dispose(); return; }

                    boost::asio::async_read(*socket_, boost::asio::buffer(length_buffer.get(), 4),
                        boost::asio::bind_executor(strand_,
                            [self = shared_from_this(), length_buffer](const boost::system::error_code& ec, std::size_t) noexcept
                            {
                                if (ec || self->server_->disposed_.load())
                                {
                                    LOG_DEBUG("LocalRpcServer::Session::ReadFrameLength: read failed, ec=%s", ec.message().data());
                                    self->Dispose();
                                    return;
                                }

                                uint32_t length = 0;
                                length |= (uint32_t)(Byte)length_buffer.get()[0] << 24;
                                length |= (uint32_t)(Byte)length_buffer.get()[1] << 16;
                                length |= (uint32_t)(Byte)length_buffer.get()[2] << 8;
                                length |= (uint32_t)(Byte)length_buffer.get()[3];
                                if (length == 0 || length > RPC_MAX_FRAME_SIZE) { self->Dispose(); return; }

                                self->ReadFrameBody(length);
                            }));
                }

                void ReadFrameBody(uint32_t length) noexcept {
                    std::shared_ptr<Byte> body =
                        ppp::threading::BufferswapAllocator::MakeByteArray(NULLPTR, (int)length);
                    if (NULLPTR == body) { Dispose(); return; }

                    boost::asio::async_read(*socket_, boost::asio::buffer(body.get(), length),
                        boost::asio::bind_executor(strand_,
                            [self = shared_from_this(), body, length](const boost::system::error_code& ec, std::size_t) noexcept
                            {
                                if (ec || self->server_->disposed_.load()) { self->Dispose(); return; }
                                self->HandleFrame(body.get(), (int)length);
                                self->ReadFrameLength();
                            }));
                }

                void HandleFrame(Byte* body, int length) noexcept {
                    Json::Value frame = ppp::auxiliary::JsonAuxiliary::FromString((const char*)body, length);
                    if (!frame.isObject())
                    {
                        LOG_DEBUG("LocalRpcServer::Session::HandleFrame: parse error, length=%d", length);
                        SendError(Json::Value(), -32700, "parse error");
                        return;
                    }

                    Json::Value id = frame.get("id", Json::Value());
                    ppp::string method = ppp::auxiliary::JsonAuxiliary::AsString(frame.get("method", Json::Value()));
                    Json::Value params = frame.get("params", Json::Value());
                    LOG_DEBUG("LocalRpcServer::Session::HandleFrame: method=%s, authenticated=%d",
                        method.data(), (int)authenticated_);

                    if (method == "hello")
                    {
                        HandleHello(id, params);
                        return;
                    }

                    if (!authenticated_)
                    {
                        SendError(id, 401, "unauthenticated");
                        return;
                    }

                    Json::Value result;
                    ppp::string error;
                    if (server_->handler_ && server_->handler_(method, params, result, error))
                    {
                        Json::Value response;
                        response["id"] = id;
                        response["ok"] = true;
                        response["result"] = result;
                        SendFrame(response);
                    }
                    else
                    {
                        SendError(id, error.empty() ? 404 : 500, error.empty() ? "unknown method" : error);
                    }
                }

                void HandleHello(const Json::Value& id, const Json::Value& params) noexcept {
                    ppp::string token = ppp::auxiliary::JsonAuxiliary::AsString(params.get("token", Json::Value()));
                    if (server_->token_.size() == 0 || token != server_->token_)
                    {
                        LOG_DEBUG("LocalRpcServer::Session::HandleHello: token mismatch, got='%s'", token.data());
                        SendError(id, 403, "invalid token");
                        Dispose();
                        return;
                    }

                    authenticated_ = true;
                    LOG_DEBUG("LocalRpcServer::Session::HandleHello: authenticated");

                    Json::Value result;
                    result["server_version"] = PPP_APPLICATION_VERSION;
                    result["schema_version"] = RPC_PROTOCOL_SCHEMA;
                    result["session_id"] = "openppp2-rpc";
                    result["max_clients"] = server_->max_clients_;

                    Json::Value response;
                    response["id"] = id;
                    response["ok"] = true;
                    response["result"] = result;
                    SendFrame(response);
                }

                void SendError(const Json::Value& id, int code, const ppp::string& message) noexcept {
                    Json::Value error;
                    error["code"] = code;
                    error["message"] = message;

                    Json::Value response;
                    response["id"] = id;
                    response["ok"] = false;
                    response["error"] = error;
                    SendFrame(response);
                }

            private:
                std::shared_ptr<LocalRpcServer>                                 server_;
                AsioTcpSocketPtr                                                socket_;
                boost::asio::strand<AsioTcpSocket::executor_type>               strand_;
                bool                                                            authenticated_ = false;
                std::deque<FrameBuffer>                                         write_queue_;
                bool                                                            writing_ = false;
            };

            LocalRpcServer::LocalRpcServer(const ContextPtr& context, const ppp::string& token, int max_clients, const RequestHandler& handler) noexcept
                : context_(context), token_(token), handler_(handler)
            {
                max_clients_ = std::max<int>(1, std::min<int>(64, max_clients));
            }

            LocalRpcServer::~LocalRpcServer() noexcept
            {
                Dispose();
            }

            bool LocalRpcServer::Open(const ppp::string& listen) noexcept
            {
                if (NULLPTR == context_ || disposed_.load() || listen.empty()) return false;

                // Parse "ip:port" or "[ipv6]:port".  Only loopback addresses are
                // accepted; everything else is rejected.
                boost::asio::ip::address address;
                uint16_t port = 0;
                ppp::string ip_text;
                ppp::string port_text;

                if (listen.size() > 0 && listen.front() == '[')
                {
                    std::size_t close = listen.find(']');
                    if (close == ppp::string::npos || close + 1 >= listen.size() || listen[close + 1] != ':') return false;
                    ip_text = listen.substr(1, close - 1);
                    port_text = listen.substr(close + 2);
                }
                else
                {
                    std::size_t colon = listen.rfind(':');
                    if (colon == ppp::string::npos || colon == 0 || colon + 1 >= listen.size()) return false;
                    ip_text = listen.substr(0, colon);
                    port_text = listen.substr(colon + 1);
                }

                boost::system::error_code ec;
                address = boost::asio::ip::make_address(ip_text, ec);
                if (ec) return false;
                if (!address.is_loopback()) return false;

                long parsed_port = strtol(port_text.data(), NULLPTR, 10);
                if (parsed_port < ppp::net::IPEndPoint::MinPort || parsed_port > ppp::net::IPEndPoint::MaxPort) return false;
                port = (uint16_t)parsed_port;

                acceptor_ = ppp::make_shared_object<AsioTcpAcceptor>(*context_);
                if (NULLPTR == acceptor_) return false;

                acceptor_->open(address.is_v4() ? boost::asio::ip::tcp::v4() : boost::asio::ip::tcp::v6(), ec);
                if (ec) { acceptor_.reset(); return false; }
                if (address.is_v6())
                {
                    acceptor_->set_option(boost::asio::ip::v6_only(true), ec);
                }
                acceptor_->set_option(boost::asio::socket_base::reuse_address(true), ec);
                acceptor_->bind(boost::asio::ip::tcp::endpoint(address, port), ec);
                if (ec) { acceptor_.reset(); return false; }
                acceptor_->listen(boost::asio::socket_base::max_listen_connections, ec);
                if (ec) { acceptor_.reset(); return false; }

                local_endpoint_ = acceptor_->local_endpoint(ec);
                if (ec) { acceptor_.reset(); return false; }

                LOG_INFO("LocalRpcServer: listening on %s:%d, token=%s",
                    local_endpoint_.address().to_string().data(),
                    (int)local_endpoint_.port(),
                    token_.empty() ? "<empty>" : "***");

                AcceptLoop();
                return true;
            }

            void LocalRpcServer::Dispose() noexcept
            {
                bool expected = false;
                if (!disposed_.compare_exchange_strong(expected, true)) return;

                AsioTcpAcceptorPtr acceptor = std::move(acceptor_);
                if (NULLPTR != acceptor)
                {
                    boost::system::error_code ec;
                    acceptor->cancel(ec);
                    acceptor->close(ec);
                }

                SessionTable sessions;
                {
                    std::lock_guard<std::mutex> scope(syncobj_);
                    sessions.swap(sessions_);
                }
                for (const SessionPtr& session : sessions)
                {
                    session->Dispose();
                }
                sessions.clear();
                client_count_.store(0);
            }

            void LocalRpcServer::Broadcast(const Json::Value& frame) noexcept
            {
                if (disposed_.load() || !frame.isObject()) return;

                SessionTable sessions;
                {
                    std::lock_guard<std::mutex> scope(syncobj_);
                    sessions = sessions_;
                }
                if (sessions.empty()) return;

                // The frame has no "id"; strip any stale id just in case.
                Json::Value notification = frame;
                notification.removeMember("id");

                for (const SessionPtr& session : sessions)
                {
                    session->SendFrame(notification);
                }
            }

            void LocalRpcServer::AcceptLoop() noexcept
            {
                if (disposed_.load() || NULLPTR == acceptor_) return;

                AsioTcpSocketPtr socket = ppp::make_shared_object<AsioTcpSocket>(*context_);
                if (NULLPTR == socket) return;

                acceptor_->async_accept(*socket,
                    [self = shared_from_this(), socket](const boost::system::error_code& ec) noexcept
                    {
                        if (ec || self->disposed_.load()) return;

                        if (self->client_count_.load() >= self->max_clients_)
                        {
                            boost::system::error_code ignore;
                            socket->close(ignore);
                            self->AcceptLoop();
                            return;
                        }

                        std::shared_ptr<Session> session =
                            ppp::make_shared_object<Session>(self, socket);
                        if (NULLPTR == session)
                        {
                            boost::system::error_code ignore;
                            socket->close(ignore);
                            self->AcceptLoop();
                            return;
                        }

                        {
                            std::lock_guard<std::mutex> scope(self->syncobj_);
                            if (self->disposed_.load())
                            {
                                boost::system::error_code ignore;
                                socket->close(ignore);
                                return;
                            }
                            self->sessions_.insert(session);
                        }
                        self->client_count_.fetch_add(1);
                        session->Start();
                        self->AcceptLoop();
                    });
            }

        }
    }
}
