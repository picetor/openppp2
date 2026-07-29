#pragma once

#include <ppp/stdafx.h>

namespace ppp {
    namespace net {
        namespace http {
            class HttpClient final {
            public:
                typedef ppp::map<ppp::string, ppp::string> Headers;

            public:
                HttpClient(const ppp::string& host, const ppp::string& cacert_path) noexcept;

            public:
                std::string                             Get(const ppp::string& api, int& status) noexcept { 
                    return this->HttpGetOrPostImpl(false, api, NULLPTR, 0, status, Headers(), "application/json; charset=UTF-8");
                }
                std::string                             Get(const ppp::string& api, const Headers& headers, int& status) noexcept {
                    return this->HttpGetOrPostImpl(false, api, NULLPTR, 0, status, headers, "application/json; charset=UTF-8");
                }
                std::string                             Post(const ppp::string& api, const char* data, size_t size, int& status) noexcept { 
                    return this->HttpGetOrPostImpl(true, api, data, size, status, Headers(), "application/x-www-form-urlencoded; charset=UTF-8");
                }
                std::string                             Post(const ppp::string& api, const char* data, size_t size, const Headers& headers, int& status, const ppp::string& content_type = "application/json; charset=UTF-8") noexcept {
                    return this->HttpGetOrPostImpl(true, api, data, size, status, headers, content_type);
                }
                static bool                             VerifyUri(const ppp::string& url, ppp::string* host, int* port, ppp::string* path, bool* https) noexcept;

            private:
                std::string                             HttpGetOrPostImpl(bool post, const ppp::string& api, const char* data, size_t size, int& status, const Headers& headers, const ppp::string& content_type) noexcept;

            private:        
                ppp::string                             _host;
                ppp::string                             _cacert_path;
                bool                                    _cacert_exist = false;
            };
        }
    }
}
