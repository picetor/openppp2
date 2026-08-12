#include <ppp/app/client/ConnectivityProbe.h>

#include <ppp/coroutines/asio/asio.h>
#include <ppp/diagnostics/Stopwatch.h>
#include <ppp/ssl/SSL.h>
#include <ppp/threading/Timer.h>

#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/ssl.h>

namespace ppp {
    namespace app {
        namespace client {
            typedef ppp::coroutines::YieldContext                                       YieldContext;
            typedef ppp::net::IPEndPoint                                                IPEndPoint;

            typedef boost::asio::ip::tcp::socket                                                 TCPSocket;
            typedef boost::asio::deadline_timer                                                 DeadlineTimer;
            typedef std::shared_ptr<DeadlineTimer>                                              DeadlineTimerPtr;

            namespace {
                /**
                 * @brief Arms a deadline timer that cancels the socket when it expires.
                 * @return True when the timer was armed (or no timeout was requested).
                 */
                bool ProbeArmTimeout(const std::shared_ptr<TCPSocket>& socket, int timeout_ms, DeadlineTimerPtr& timer) noexcept {
                    if (timeout_ms < 1) {
                        return true;
                    }
                    if (!socket) {
                        return false;
                    }

                    timer = make_shared_object<DeadlineTimer>(socket->get_executor());
                    if (!timer) {
                        return false;
                    }

                    std::weak_ptr<TCPSocket> weak_socket = socket;
                    timer->expires_from_now(boost::posix_time::milliseconds(timeout_ms));
                    timer->async_wait([weak_socket](const boost::system::error_code& ec) noexcept {
                        if (ec != boost::system::errc::success) {
                            return;
                        }
                        std::shared_ptr<TCPSocket> socket = weak_socket.lock();
                        if (socket) {
                            boost::system::error_code ignored;
                            socket->cancel(ignored);
                        }
                    });
                    return true;
                }
            }

            bool ConnectivityProbe::ProbeTcp(const TCPEndPoint& remoteEP, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect) noexcept {
                rtt_ms = 0;
                if (!y) {
                    return false;
                }
                if (IPEndPoint::IsInvalid(remoteEP.address())) {
                    return false;
                }
                if (remoteEP.port() <= IPEndPoint::MinPort || remoteEP.port() > IPEndPoint::MaxPort) {
                    return false;
                }

                boost::asio::io_context& context = y.GetContext();
                std::shared_ptr<TCPSocket> socket = make_shared_object<TCPSocket>(context);
                if (!socket) {
                    return false;
                }

                boost::system::error_code open_ec;
                if (!ppp::coroutines::asio::async_open(y, *socket, remoteEP.protocol(), &open_ec)) {
                    return false;
                }

                if (protect) {
                    if (!protect(socket->native_handle())) {
                        boost::system::error_code ignored;
                        socket->close(ignored);
                        return false;
                    }
                }

                ppp::diagnostics::Stopwatch stopwatch;
                stopwatch.Restart();

                bool ok = false;
                DeadlineTimerPtr timer;
                if (!ProbeArmTimeout(socket, timeout_ms, timer)) {
                    return false;
                }

                boost::asio::post(socket->get_executor(),
                    [socket, remoteEP, &y, &ok]() noexcept {
                        socket->async_connect(remoteEP,
                            [&y, &ok](const boost::system::error_code& ec) noexcept {
                                ok = ec == boost::system::errc::success;
                                y.R();
                            });
                    });
                y.Suspend();

                if (timer) {
                    boost::system::error_code ignored;
                    timer->cancel(ignored);
                }
                if (ok) {
                    rtt_ms = (int)stopwatch.ElapsedMilliseconds();
                }
                else {
                    boost::system::error_code ignored;
                    socket->close(ignored);
                }
                return ok;
            }

            namespace {
                template <typename TWebSocket>
                bool ProbeWebSocketUpgrade(TWebSocket& websocket, const ppp::string& host, const ppp::string& path, YieldContext& y) noexcept {
                    if (host.empty() || path.empty()) {
                        return false;
                    }

                    bool ok = false;
                    boost::asio::post(websocket.get_executor(),
                        [&websocket, &host, &path, &y, &ok]() noexcept {
                            websocket.async_handshake(host.data(), path.data(),
                                [&y, &ok](const boost::system::error_code& ec) noexcept {
                                    ok = ec == boost::system::errc::success;
                                    y.R();
                                });
                        });
                    y.Suspend();
                    return ok;
                }
            }

            bool ConnectivityProbe::ProbeWebSocket(const TCPEndPoint& remoteEP, const ppp::string& host, const ppp::string& path, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect) noexcept {
                rtt_ms = 0;
                if (!y) {
                    return false;
                }
                if (IPEndPoint::IsInvalid(remoteEP.address()) || host.empty() || path.empty()) {
                    return false;
                }

                boost::asio::io_context& context = y.GetContext();
                std::shared_ptr<TCPSocket> socket = make_shared_object<TCPSocket>(context);
                if (!socket) {
                    return false;
                }

                boost::system::error_code open_ec;
                if (!ppp::coroutines::asio::async_open(y, *socket, remoteEP.protocol(), &open_ec)) {
                    return false;
                }

                if (protect) {
                    if (!protect(socket->native_handle())) {
                        boost::system::error_code ignored;
                        socket->close(ignored);
                        return false;
                    }
                }

                ppp::diagnostics::Stopwatch stopwatch;
                stopwatch.Restart();

                bool ok = false;
                DeadlineTimerPtr timer;
                if (!ProbeArmTimeout(socket, timeout_ms, timer)) {
                    return false;
                }

                boost::asio::post(socket->get_executor(),
                    [socket, remoteEP, &y, &ok]() noexcept {
                        socket->async_connect(remoteEP,
                            [&y, &ok](const boost::system::error_code& ec) noexcept {
                                ok = ec == boost::system::errc::success;
                                y.R();
                            });
                    });
                y.Suspend();

                if (ok) {
                    typedef boost::beast::websocket::stream<TCPSocket&>                  WebSocket;
                    WebSocket websocket(*socket);
                    ok = ProbeWebSocketUpgrade(websocket, host, path, y);
                }

                if (timer) {
                    boost::system::error_code ignored;
                    timer->cancel(ignored);
                }
                if (ok) {
                    rtt_ms = (int)stopwatch.ElapsedMilliseconds();
                }
                else {
                    boost::system::error_code ignored;
                    socket->close(ignored);
                }
                return ok;
            }

            bool ConnectivityProbe::ProbeWebSocketSSL(const TCPEndPoint& remoteEP, const ppp::string& host, const ppp::string& sni, const ppp::string& path, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect) noexcept {
                rtt_ms = 0;
                if (!y) {
                    return false;
                }
                if (IPEndPoint::IsInvalid(remoteEP.address()) || host.empty() || path.empty()) {
                    return false;
                }

                boost::asio::io_context& context = y.GetContext();
                std::shared_ptr<TCPSocket> socket = make_shared_object<TCPSocket>(context);
                if (!socket) {
                    return false;
                }

                boost::system::error_code open_ec;
                if (!ppp::coroutines::asio::async_open(y, *socket, remoteEP.protocol(), &open_ec)) {
                    return false;
                }

                if (protect) {
                    if (!protect(socket->native_handle())) {
                        boost::system::error_code ignored;
                        socket->close(ignored);
                        return false;
                    }
                }

                ppp::diagnostics::Stopwatch stopwatch;
                stopwatch.Restart();

                bool ok = false;
                DeadlineTimerPtr timer;
                if (!ProbeArmTimeout(socket, timeout_ms, timer)) {
                    return false;
                }

                boost::asio::post(socket->get_executor(),
                    [socket, remoteEP, &y, &ok]() noexcept {
                        socket->async_connect(remoteEP,
                            [&y, &ok](const boost::system::error_code& ec) noexcept {
                                ok = ec == boost::system::errc::success;
                                y.R();
                            });
                    });
                y.Suspend();

                if (ok) {
                    std::shared_ptr<boost::asio::ssl::context> ssl_context =
                        ppp::ssl::SSL::CreateClientSslContext(ppp::ssl::SSL::SSL_METHOD::ssl, false, "");
                    if (ssl_context) {
                        typedef boost::asio::ssl::stream<TCPSocket&>                     SslStream;
                        typedef boost::beast::websocket::stream<SslStream&>              WebSocket;

                        SslStream ssl_stream(*socket, *ssl_context);
                        SSL* ssl_handle = ssl_stream.native_handle();
                        if (ssl_handle && !sni.empty()) {
                            SSL_set_tlsext_host_name(ssl_handle, sni.data());
                        }

                        bool ssl_ok = false;
                        boost::asio::post(socket->get_executor(),
                            [&ssl_stream, &y, &ssl_ok]() noexcept {
                                ssl_stream.async_handshake(boost::asio::ssl::stream_base::client,
                                    [&y, &ssl_ok](const boost::system::error_code& ec) noexcept {
                                        ssl_ok = ec == boost::system::errc::success;
                                        y.R();
                                    });
                            });
                        y.Suspend();

                        if (ssl_ok) {
                            WebSocket websocket(ssl_stream);
                            ok = ProbeWebSocketUpgrade(websocket, host, path, y);
                        }
                    }
                }

                if (timer) {
                    boost::system::error_code ignored;
                    timer->cancel(ignored);
                }
                if (ok) {
                    rtt_ms = (int)stopwatch.ElapsedMilliseconds();
                }
                else {
                    boost::system::error_code ignored;
                    socket->close(ignored);
                }
                return ok;
            }

            bool ConnectivityProbe::ProbeUdp(const UDPEndPoint& remoteEP, int timeout_ms, YieldContext& y, int& rtt_ms, const ProtectSocketHandler& protect) noexcept {
                // The static-UDP channel uses an application-level echo protocol
                // (session id + ciphertext) that a standalone probe cannot speak.
                // A UDP "probe" is therefore send-only: reachable means the OS
                // accepted the datagram (no immediate ICMP error observed).
                rtt_ms = 0;
                if (!y) {
                    return false;
                }
                if (IPEndPoint::IsInvalid(remoteEP.address())) {
                    return false;
                }
                if (remoteEP.port() <= IPEndPoint::MinPort || remoteEP.port() > IPEndPoint::MaxPort) {
                    return false;
                }

                boost::asio::io_context& context = y.GetContext();
                std::shared_ptr<boost::asio::ip::udp::socket> socket = make_shared_object<boost::asio::ip::udp::socket>(context);
                if (!socket) {
                    return false;
                }

                boost::system::error_code open_ec;
                if (!ppp::coroutines::asio::async_open(y, *socket, remoteEP.protocol(), &open_ec)) {
                    return false;
                }

                if (protect) {
                    if (!protect(socket->native_handle())) {
                        boost::system::error_code ignored;
                        socket->close(ignored);
                        return false;
                    }
                }

                ppp::diagnostics::Stopwatch stopwatch;
                stopwatch.Restart();

                char datagram = 0;
                boost::system::error_code send_ec;
                socket->send_to(boost::asio::buffer(&datagram, 1), remoteEP, 0, send_ec);
                if (send_ec) {
                    boost::system::error_code ignored;
                    socket->close(ignored);
                    return false;
                }

                rtt_ms = (int)stopwatch.ElapsedMilliseconds();
                return true;
            }

            int ConnectivityProbe::ComputeJitter(const ppp::vector<int>& rtt_samples) noexcept {
                if (rtt_samples.size() < 2) {
                    return 0;
                }

                // Peak-to-peak fluctuation: max(samples) - min(samples).
                int min_rtt = INT_MAX;
                int max_rtt = INT_MIN;
                for (int sample : rtt_samples) {
                    min_rtt = std::min<int>(min_rtt, sample);
                    max_rtt = std::max<int>(max_rtt, sample);
                }
                const int jitter_pp = max_rtt - min_rtt;

                // Median absolute deviation (MAD): median(|x - median|), robust to spikes.
                ppp::vector<int> sorted(rtt_samples.begin(), rtt_samples.end());
                std::stable_sort(sorted.begin(), sorted.end());
                const std::size_t n = sorted.size();
                const int median = (n % 2 == 1) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
                ppp::vector<int> deviation;
                deviation.reserve(n);
                for (int sample : sorted) {
                    deviation.emplace_back(sample > median ? sample - median : median - sample);
                }
                std::stable_sort(deviation.begin(), deviation.end());
                const int jitter_mad = (n % 2 == 1) ? deviation[n / 2] : (deviation[n / 2 - 1] + deviation[n / 2]) / 2;

                // Fixed metric: the larger of the two (sensitive yet stable).
                return std::max<int>(jitter_pp, jitter_mad);
            }
        }
    }
}
