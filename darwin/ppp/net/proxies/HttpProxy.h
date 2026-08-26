#pragma once

#include <ppp/stdafx.h>

namespace ppp {
    namespace net {
        namespace proxies {
            class HttpProxy {
            public:
                static bool RefreshSystemProxy() noexcept;
                static bool SetSystemProxy(const ppp::string& server) noexcept;
                static bool SetSystemProxy(const ppp::string& server, const ppp::string& bypass) noexcept;
                static bool SetSystemProxy(const ppp::string& server, const ppp::string& pac, bool enable) noexcept;
            };
        }
    }
}
