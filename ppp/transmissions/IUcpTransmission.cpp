// IUcpTransmission.cpp
#include <ppp/transmissions/IUcpTransmission.h>

#include <cstring>

#include <ppp/threading/Executors.h>
#include <ppp/threading/BufferswapAllocator.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>

namespace ppp {
    namespace transmissions {
        namespace {
            // Single receive-chunk size.  UCP segments application data into
            // Mss-sized packets on the wire; a 64 KiB chunk keeps the number of
            // queued chunks small even under heavy load.
            static constexpr int IUCP_RECEIVE_BUFFER_SIZE = 65536;
        }

        IUcpTransmission::IUcpTransmission(
            const ContextPtr&                                   context,
            const StrandPtr&                                    strand,
            const AppConfigurationPtr&                          configuration,
            const UcpNetworkPtr&                                network,
            const UcpConnectionPtr&                             connection,
            const boost::asio::ip::tcp::endpoint&               remoteEP) noexcept
            : ITransmission(context, strand, configuration)
            , disposed_(false)
            , closed_(false)
            , driving_(false)
            , on_disconnected_registered_(false)
            , connection_(connection ? connection.get() : NULLPTR)
            , connection_owner_(connection)
            , network_(network)
            , remoteEP_(remoteEP) {
            StartDriving();
        }

        IUcpTransmission::IUcpTransmission(
            const ContextPtr&                                   context,
            const StrandPtr&                                    strand,
            const AppConfigurationPtr&                          configuration,
            ucp::UcpConnection*                                 connection) noexcept
            : ITransmission(context, strand, configuration)
            , disposed_(false)
            , closed_(false)
            , driving_(false)
            , on_disconnected_registered_(false)
            , connection_(connection) {
            // Server mode: resolve the remote endpoint from the accepted connection.
            if (NULLPTR != connection_) {
                ucp::string remote = connection_->GetRemoteEndpoint();
                ucp::Endpoint endpoint = ucp::Endpoint::Parse(remote);
                boost::system::error_code ec;
                boost::asio::ip::address address = ppp::StringToAddress(endpoint.address.c_str(), ec);
                if (!ec) {
                    remoteEP_ = ppp::net::Ipep::V6ToV4(boost::asio::ip::tcp::endpoint(address, endpoint.port));
                }
            }
        }

        IUcpTransmission::~IUcpTransmission() noexcept {
            Finalize();
        }

        void IUcpTransmission::Finalize() noexcept {
            disposed_ = true;

            // Stop the DoEvents driving thread (client mode).
            driving_ = false;
            if (driving_thread_.joinable() && std::this_thread::get_id() != driving_thread_.get_id()) {
                driving_thread_.join();
            }

            // Synchronously close the UCP connection.  In server mode the
            // UcpServer owns the object; Close() tears down the PCB and lets
            // the server reclaim it.  In client mode the connection is owned
            // by connection_owner_ below.
            ucp::UcpConnection* connection = connection_;
            connection_ = NULLPTR;
            if (NULLPTR != connection) {
                LOG_DEBUG("IUcpTransmission::Finalize: closing ucp connection, this=%p", (void*)this);
                connection->Close();
            }

            // Release client-mode ownership.
            connection_owner_.reset();
            network_.reset();

            // Wake every suspended reader so it observes the EOF state.
            WakeupAllReaders();
        }

        void IUcpTransmission::Dispose() noexcept {
            if (disposed_) {
                LOG_DEBUG("IUcpTransmission::Dispose: already disposed, skipping, this=%p", (void*)this);
                return;
            }
            disposed_ = true;

            auto self = std::static_pointer_cast<IUcpTransmission>(shared_from_this());
            ppp::threading::Executors::ContextPtr context = GetContext();
            ppp::threading::Executors::StrandPtr strand = GetStrand();

            ppp::threading::Executors::Post(context, strand,
                [self, this]() noexcept {
                    Finalize();
                });
            ITransmission::Dispose();
        }

        bool IUcpTransmission::ShiftToScheduler() noexcept {
            // UCP runs its own worker threads; there is no asio socket to move.
            return true;
        }

        boost::asio::ip::tcp::endpoint IUcpTransmission::GetRemoteEndPoint() noexcept {
            return remoteEP_;
        }

        std::shared_ptr<Byte> IUcpTransmission::DoReadBytes(YieldContext& y, int length) noexcept {
            if (disposed_) {
                LOG_DEBUG("IUcpTransmission::DoReadBytes: disposed, length=%d", length);
                return NULLPTR;
            }

            auto self = std::static_pointer_cast<IUcpTransmission>(shared_from_this());
            return ITransmissionQoS::DoReadBytes(y, length, self, *this, this->QoS);
        }

        std::shared_ptr<Byte> IUcpTransmission::ReadBytes(YieldContext& y, int length) noexcept {
            if (length < 1) {
                return NULLPTR;
            }

            std::shared_ptr<BufferswapAllocator> allocator = this->BufferAllocator;
            std::shared_ptr<Byte> packet = BufferswapAllocator::MakeByteArray(allocator, length);
            if (NULLPTR == packet) {
                return NULLPTR;
            }

            while (true) {
                if (disposed_) {
                    LOG_DEBUG("IUcpTransmission::ReadBytes: disposed, length=%d", length);
                    return NULLPTR;
                }

                {
                    std::lock_guard<std::mutex> scope(syncobj_);
                    if (rx_count_ >= length) {
                        int copied = 0;
                        while (copied < length) {
                            if (rx_queue_.empty()) {
                                break;
                            }

                            RxChunk& chunk = rx_queue_.front();
                            int n = std::min<int>(length - copied, chunk.second - rx_offset_);
                            std::memcpy(packet.get() + copied, chunk.first.get() + rx_offset_, n);
                            copied += n;
                            rx_offset_ += n;
                            rx_count_ -= n;

                            if (rx_offset_ >= chunk.second) {
                                rx_queue_.pop_front();
                                rx_offset_ = 0;
                            }
                        }

                        if (copied == length) {
                            return packet;
                        }
                    }

                    if (closed_) {
                        LOG_DEBUG("IUcpTransmission::ReadBytes: connection closed, length=%d, buffered=%d", length, rx_count_);
                        return NULLPTR;
                    }
                }

                // Not enough buffered data yet; suspend until the receive pump
                // pushes more bytes or the connection closes.
                if (!BeginRead(y)) {
                    return NULLPTR;
                }
            }
        }

        bool IUcpTransmission::BeginRead(YieldContext& y) noexcept {
            auto status = ppp::make_shared_object<std::atomic<int>>(-1);
            if (NULLPTR == status) {
                return false;
            }

            {
                std::lock_guard<std::mutex> scope(syncobj_);
                if (disposed_ || closed_) {
                    return false;
                }
                readers_.push_back(std::make_pair(status, &y));
            }

            y.Suspend();
            return status->load() > 0;
        }

        void IUcpTransmission::WakeupReaders() noexcept {
            ppp::list<std::pair<std::shared_ptr<std::atomic<int>>, YieldContext*>> readers;
            {
                std::lock_guard<std::mutex> scope(syncobj_);
                readers.swap(readers_);
            }

            if (readers.empty()) {
                return;
            }

            auto context = GetContext();
            if (NULLPTR == context) {
                return;
            }

            for (auto& r : readers) {
                std::shared_ptr<std::atomic<int>> status = r.first;
                YieldContext* y = r.second;
                boost::asio::post(*context,
                    [status, y]() noexcept {
                        ppp::coroutines::asio::R(*y, *status, true);
                    });
            }
        }

        void IUcpTransmission::WakeupAllReaders() noexcept {
            WakeupReaders();
        }

        void IUcpTransmission::OnConnectionClosed() noexcept {
            {
                std::lock_guard<std::mutex> scope(syncobj_);
                if (closed_) {
                    return;
                }
                closed_ = true;
            }

            // Buffered bytes are still readable; ReadBytes drains them first
            // and then observes closed_ and returns EOF.
            WakeupReaders();
        }

        void IUcpTransmission::StartDriving() noexcept {
            if (NULLPTR == network_) {
                return;
            }

            driving_ = true;
            auto self = std::static_pointer_cast<IUcpTransmission>(shared_from_this());
            driving_thread_ = std::thread(
                [self]() noexcept {
                    self->DriveLoop();
                });
        }

        void IUcpTransmission::DriveLoop() noexcept {
            UcpNetworkPtr network = network_;
            while (driving_ && network) {
                network->DoEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        bool IUcpTransmission::StartReceive() noexcept {
            ucp::UcpConnection* connection = connection_;
            if (NULLPTR == connection || disposed_ || closed_) {
                return false;
            }

            std::shared_ptr<BufferswapAllocator> allocator = this->BufferAllocator;
            std::shared_ptr<Byte> buffer = BufferswapAllocator::MakeByteArray(allocator, IUCP_RECEIVE_BUFFER_SIZE);
            if (NULLPTR == buffer) {
                return false;
            }

            ReceiveMore(buffer);
            return true;
        }

        void IUcpTransmission::ReceiveMore(const std::shared_ptr<Byte>& buffer) noexcept {
            ucp::UcpConnection* connection = connection_;
            if (NULLPTR == connection || disposed_ || closed_) {
                return;
            }

            // Register the disconnect notification exactly once.  The weak
            // reference keeps the callback safe if the transmission is torn
            // down while a UCP worker thread is still dispatching events.
            if (!on_disconnected_registered_) {
                on_disconnected_registered_ = true;
                std::weak_ptr<IUcpTransmission> weak = std::static_pointer_cast<IUcpTransmission>(shared_from_this());
                connection->SetOnDisconnected(
                    [weak]() noexcept {
                        std::shared_ptr<IUcpTransmission> self = weak.lock();
                        if (NULLPTR != self) {
                            self->OnConnectionClosed();
                        }
                    });
            }

            auto self = std::static_pointer_cast<IUcpTransmission>(shared_from_this());
            connection->ReceiveAsync(buffer.get(), 0, IUCP_RECEIVE_BUFFER_SIZE,
                [self, this, buffer](ucp::UcpError error, int32_t n) noexcept {
                    if (error != ucp::UcpError::None || n <= 0) {
                        LOG_DEBUG("IUcpTransmission::ReceiveMore: receive ended, error=%d, n=%d, this=%p",
                            (int)error, (int)n, (void*)this);
                        OnConnectionClosed();
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> scope(syncobj_);
                        rx_queue_.push_back(RxChunk(buffer, (int)n));
                        rx_count_ += (int)n;
                    }

                    WakeupReaders();
                    ReceiveMore(buffer);
                });
        }

        bool IUcpTransmission::DoWriteBytes(std::shared_ptr<Byte> packet, int offset, int packet_length, const AsynchronousWriteBytesCallback& cb) noexcept {
            ucp::UcpConnection* connection = connection_;
            if (NULLPTR == connection || disposed_ || closed_) {
                LOG_DEBUG("IUcpTransmission::DoWriteBytes: connection invalid, packet_length=%d, disposed=%d, closed=%d",
                    packet_length, (int)disposed_, (int)closed_);
                if (cb) {
                    cb(false);
                }
                return false;
            }

            auto self = std::static_pointer_cast<IUcpTransmission>(shared_from_this());
            auto context = GetContext();
            auto strand = GetStrand();

            auto complete_do_write_bytes_async_callback = [self, this, context, strand, connection, packet, offset, packet_length, cb]() noexcept {
                // WriteAsync requires the source buffer to stay valid until the
                // completion callback fires; `packet` is captured by value so it
                // stays alive for the whole operation.
                connection->WriteAsync((const uint8_t*)packet.get() + offset, 0, packet_length,
                    [self, this, context, strand, packet, packet_length, cb](ucp::UcpError error, bool success) noexcept {
                        bool ok = (error == ucp::UcpError::None) && success;
                        if (ok) {
                            std::shared_ptr<ITransmissionStatistics> statistics = this->Statistics;
                            if (statistics) {
                                statistics->AddOutgoingTraffic(packet_length);
                            }
                        }
                        else {
                            LOG_DEBUG("IUcpTransmission::DoWriteBytes: WriteAsync failed, error=%d, success=%d, packet_length=%d, disposed=%d",
                                (int)error, (int)success, packet_length, (int)disposed_);
                            if (!disposed_) {
                                Dispose();
                            }
                        }

                        if (cb) {
                            cb(ok);
                        }
                    });
                };

            return ppp::threading::Executors::Post(context, strand, complete_do_write_bytes_async_callback);
        }
    }
}
