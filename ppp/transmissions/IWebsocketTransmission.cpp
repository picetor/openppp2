#include <ppp/transmissions/IWebsocketTransmission.h>
#include <ppp/net/Socket.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/coroutines/asio/asio.h>
#include <ppp/coroutines/YieldContext.h>

namespace ppp {
    namespace transmissions {
        template <typename R>
        static inline bool DecoratorWebsocketAllHeaders(ppp::map<ppp::string, ppp::string>& headers, R& r) noexcept {
            if (headers.empty()) {
                return false;
            }

            for (auto&& [k, v] : headers) {
                boost::beast::string_view vsv(v.data(), v.size());
                boost::beast::string_view ksv(k.data(), k.size());
                r.set(ksv, vsv);
            }

            return true;
        }

        static inline bool DecoratorWebsocketResponseToWebclient(const ITransmission::AppConfigurationPtr& configuration, boost::beast::websocket::response_type& res) noexcept {
            if (NULLPTR == configuration) {
                return false;
            }

            int status_code = res.result_int();
            bool ok = DecoratorWebsocketAllHeaders(configuration->websocket.http.response, res);
            if (status_code == 404) {
                std::string& response_body = res.body();
                response_body = configuration->websocket.http.error;
            }

            return ok;
        }

        IWebsocketTransmission::IWebsocketTransmission(
            const ContextPtr&                                       context,
            const StrandPtr&                                        strand,
            const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket,
            const AppConfigurationPtr&                              configuration) noexcept
            : WebSocket(context, strand, socket, configuration) {

        }

        bool IWebsocketTransmission::HandshakeWebsocket(
            const AppConfigurationPtr&                              configuration,
            const std::shared_ptr<ppp::net::asio::websocket>&       socket,
            HandshakeType                                           handshake_type,
            YieldContext&                                           y) noexcept {

            ppp::string host = std::move(this->Host);
            ppp::string path = std::move(this->Path);

            auto& client_cfg = configuration->client.websocket;

            ppp::string ws_host;
            ppp::string ws_path;
            bool ok;
            if (host.size() > 0 && path.size() > 0) {
                ws_host = client_cfg.host.size() > 0 ? client_cfg.host : host;
                ws_path = path;
                ok = socket->Run(handshake_type, ws_host, ws_path, y);
            }
            else {
                auto& cfg = configuration->websocket;
                ws_host = client_cfg.host.size() > 0 ? client_cfg.host : cfg.host;
                ws_path = cfg.path;
                ok = socket->Run(handshake_type, ws_host, ws_path, y);
            }

            LOG_DEBUG("IWebsocketTransmission::HandshakeWebsocket: %s, entry_ip=%s, host=%s, path=%s, type=%s",
                ok ? "success" : "failed", host.data(), ws_host.data(), ws_path.data(),
                handshake_type == ppp::net::asio::websocket::HandshakeType_Client ? "client" : "server");
            return ok;
        }

        bool IWebsocketTransmission::Decorator(boost::beast::websocket::request_type& req) noexcept {
            auto configuration = GetConfiguration();
            if (NULLPTR == configuration) {
                return false;
            }

            return DecoratorWebsocketAllHeaders(configuration->websocket.http.request, req);
        }
        
        bool IWebsocketTransmission::Decorator(boost::beast::websocket::response_type& res) noexcept {
            return DecoratorWebsocketResponseToWebclient(GetConfiguration(), res);
        }

        ISslWebsocketTransmission::ISslWebsocketTransmission(
            const ContextPtr&                                       context,
            const StrandPtr&                                        strand,
            const std::shared_ptr<boost::asio::ip::tcp::socket>&    socket,
            const AppConfigurationPtr&                              configuration) noexcept
            : WebSocket(context, strand, socket, configuration) {

        }

        bool ISslWebsocketTransmission::HandshakeWebsocket(
            const AppConfigurationPtr&                              configuration,
            const std::shared_ptr<ppp::net::asio::sslwebsocket>&    socket,
            HandshakeType                                           handshake_type,
            YieldContext&                                           y) noexcept {

            ppp::string host = std::move(this->Host);
            ppp::string path = std::move(this->Path);

            auto& cfg = configuration->websocket;
            auto& client_cfg = configuration->client.websocket;

            ppp::string ws_host;
            ppp::string sni;
            ppp::string ws_path;
            bool ok;
            if (host.size() > 0 && path.size() > 0) {
                ws_host = client_cfg.host.size() > 0 ? client_cfg.host : host;
                sni = client_cfg.sni.size() > 0 ? client_cfg.sni : ws_host;
                ws_path = path;
                ok = socket->Run(handshake_type,
                    ws_host,
                    sni,
                    ws_path,
                    cfg.ssl.verify_peer,
                    cfg.ssl.certificate_file,
                    cfg.ssl.certificate_key_file,
                    cfg.ssl.certificate_chain_file,
                    cfg.ssl.certificate_key_password,
                    cfg.ssl.ciphersuites,
                    y);
            }
            else {
                ws_host = client_cfg.host.size() > 0 ? client_cfg.host : cfg.host;
                sni = client_cfg.sni.size() > 0 ? client_cfg.sni : ws_host;
                ws_path = cfg.path;
                ok = socket->Run(handshake_type,
                    ws_host,
                    sni,
                    ws_path,
                    cfg.ssl.verify_peer,
                    cfg.ssl.certificate_file,
                    cfg.ssl.certificate_key_file,
                    cfg.ssl.certificate_chain_file,
                    cfg.ssl.certificate_key_password,
                    cfg.ssl.ciphersuites,
                    y);
            }

            LOG_DEBUG("ISslWebsocketTransmission::HandshakeWebsocket: %s, entry_ip=%s, host=%s, sni=%s, path=%s, type=%s",
                ok ? "success" : "failed", host.data(), ws_host.data(), sni.data(), ws_path.data(),
                handshake_type == ppp::net::asio::websocket::HandshakeType_Client ? "client" : "server");
            return ok;
        }

        bool ISslWebsocketTransmission::Decorator(boost::beast::websocket::request_type& req) noexcept {
            auto configuration = GetConfiguration();
            if (NULLPTR == configuration) {
                return false;
            }

            return DecoratorWebsocketAllHeaders(configuration->websocket.http.request, req);
        }

        bool ISslWebsocketTransmission::Decorator(boost::beast::websocket::response_type& res) noexcept {
            return DecoratorWebsocketResponseToWebclient(GetConfiguration(), res);
        }
    }
}