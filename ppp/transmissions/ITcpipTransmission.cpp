#include <ppp/transmissions/ITcpipTransmission.h>
#include <ppp/net/Socket.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>

#include <ppp/threading/Executors.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>

using ppp::net::Socket;
using ppp::net::IPEndPoint;

namespace ppp {
    namespace transmissions {
        ITcpipTransmission::ITcpipTransmission(
            const ContextPtr&                                       context, 
            const StrandPtr&                                        strand,
            const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket, 
            const AppConfigurationPtr&                              configuration) noexcept 
            : ITransmission(context, strand, configuration)
            , disposed_(false)
            , socket_(socket) {
            boost::system::error_code ec;
            remoteEP_ = ppp::net::Ipep::V6ToV4(socket->remote_endpoint(ec));

#if defined(_WIN32)
            if (ppp::net::Socket::IsDefaultFlashTypeOfService()) {
                qoss_ = ppp::net::QoSS::New(socket->native_handle());
            }
#endif
        }

        ITcpipTransmission::~ITcpipTransmission() noexcept {
            Finalize();
        }
 
        void ITcpipTransmission::Finalize() noexcept {
            std::shared_ptr<boost::asio::ip::tcp::socket> socket = std::move(socket_);
            disposed_ = true;

            if (socket) {
                boost::system::error_code ec;
                boost::asio::ip::tcp::endpoint ep = socket->remote_endpoint(ec);
                bool is_open = socket->is_open();
                LOG_DEBUG("ITcpipTransmission::Finalize: closing socket, is_open=%d, remote=%s, native=%p",
                    (int)is_open,
                    ec ? "n/a" : ep.address().to_string().c_str(),
                    (void*)(uintptr_t)socket->native_handle());
                Socket::Closesocket(socket);
            }
            else {
                LOG_DEBUG("ITcpipTransmission::Finalize: socket already null, disposed=%d", (int)disposed_);
            }

#if defined(_WIN32)
            qoss_.reset();
#endif
        }

        void ITcpipTransmission::Dispose() noexcept {
            auto self = shared_from_this();
            ppp::threading::Executors::ContextPtr context = GetContext();
            ppp::threading::Executors::StrandPtr strand = GetStrand();

            ppp::threading::Executors::Post(context, strand,
                [self, this, context, strand]() noexcept {
                    Finalize();
                });
            ITransmission::Dispose();
        }

        boost::asio::ip::tcp::endpoint ITcpipTransmission::GetRemoteEndPoint() noexcept {
            return remoteEP_;
        }

        std::shared_ptr<Byte> ITcpipTransmission::DoReadBytes(YieldContext& y, int length) noexcept {
            if (disposed_) {
                LOG_DEBUG("ITcpipTransmission::DoReadBytes: disposed, length=%d", length);
                return NULLPTR;
            }

            auto self = shared_from_this();
            auto result = ITransmissionQoS::DoReadBytes(y, length, self, *this, this->QoS);
            if (NULLPTR == result) {
#if defined(PPP_LOG_VERBOSE)
                std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
                LOG_DEBUG("ITcpipTransmission::DoReadBytes: failed, length=%d, socket_open=%d, disposed=%d, remote=%s",
                    length,
                    socket ? (int)socket->is_open() : -1,
                    (int)disposed_,
                    socket && socket->is_open() ? socket->remote_endpoint().address().to_string().c_str() : "n/a");
#else
                LOG_DEBUG("ITcpipTransmission::DoReadBytes: failed, length=%d", length);
#endif
            }
            return result;
        }

        bool ITcpipTransmission::ShiftToScheduler() noexcept {
            std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
            if (!socket || !socket->is_open()) {
                return false;
            }

            if (disposed_) {
                return false;
            }

            std::shared_ptr<boost::asio::ip::tcp::socket> socket_new;
            ContextPtr scheduler;
            StrandPtr strand;

            bool ok = ppp::threading::Executors::ShiftToScheduler(*socket, socket_new, scheduler, strand);
            if (ok) {
                socket_ = socket_new;
                GetStrand() = strand;
                GetContext() = scheduler;
            }

            return ok;
        }

        std::shared_ptr<Byte> ITcpipTransmission::ReadBytes(YieldContext& y, int length) noexcept {
            std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
            if (!socket || !socket->is_open()) {
                return NULLPTR;
            }

            if (disposed_) {
                return NULLPTR;
            }

            if (length < 1) {
                return NULLPTR;
            }

            std::shared_ptr<BufferswapAllocator> allocator = this->BufferAllocator;
            std::shared_ptr<Byte> packet = BufferswapAllocator::MakeByteArray(allocator, length);
            if (NULLPTR == packet) {
                return NULLPTR;
            }

#if defined(PPP_LOG_VERBOSE)
            boost::system::error_code ec;
            bool ok = ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(packet.get(), length), y, ec);
            if (!ok) {
                LOG_DEBUG("ITcpipTransmission::ReadBytes: async_read failed, length=%d, ec=%d, msg=%s",
                    length, ec.value(), ec.message().c_str());
                Dispose();
                return NULLPTR;
            }
#else
            bool ok = ppp::coroutines::asio::async_read(*socket, boost::asio::buffer(packet.get(), length), y);
            if (!ok) {
                Dispose();
                return NULLPTR;
            }
#endif

            std::shared_ptr<ITransmissionStatistics> statistics = this->Statistics;
            if (statistics) {
                statistics->AddIncomingTraffic(length);
            }

            return packet;
        }

        bool ITcpipTransmission::DoWriteBytes(std::shared_ptr<Byte> packet, int offset, int packet_length, const AsynchronousWriteBytesCallback& cb) noexcept {
            std::shared_ptr<boost::asio::ip::tcp::socket> socket = socket_;
            if (!socket || !socket->is_open()) {
                LOG_DEBUG("ITcpipTransmission::DoWriteBytes: socket invalid, packet_length=%d", packet_length);
                return false;
            }

            if (disposed_) {
                LOG_DEBUG("ITcpipTransmission::DoWriteBytes: disposed, packet_length=%d", packet_length);
                return false;
            }

            std::shared_ptr<IAsynchronousWriteIoQueue> self = shared_from_this();
            auto context = GetContext();
            auto strand = GetStrand();

            auto complete_do_write_bytes_async_callback = [self, this, socket, context, strand, packet, offset, packet_length, cb]() noexcept {
                if (!socket->is_open()) {
                    LOG_DEBUG("ITcpipTransmission::DoWriteBytes: socket closed before async_write, packet_length=%d, disposed=%d",
                        packet_length, (int)disposed_);
                    if (cb) cb(false);
                    return;
                }
                boost::asio::async_write(*socket, boost::asio::buffer((Byte*)packet.get() + offset, packet_length),
                    [self, this, context, strand, packet, packet_length, cb, socket](const boost::system::error_code& ec, std::size_t sz) noexcept {
                        bool ok = ec == boost::system::errc::success;
                        if (ok) {
                            std::shared_ptr<ITransmissionStatistics> statistics = this->Statistics;
                            if (statistics) {
                                statistics->AddOutgoingTraffic(packet_length);
                            }
                        }
                        else {
                            LOG_DEBUG("ITcpipTransmission::DoWriteBytes: async_write failed, ec=%s, ecv=%d, packet_length=%d, disposed=%d, socket_open=%d",
                                ec.message().data(), ec.value(), packet_length, (int)disposed_, socket ? (int)socket->is_open() : -1);
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