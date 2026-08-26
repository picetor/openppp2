#include <darwin/ppp/net/proxies/HttpProxy.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace ppp {
    namespace net {
        namespace proxies {
            namespace {
                struct ProxyState final {
                    std::string service;
                    bool web_enabled = false;
                    std::string web_server;
                    int web_port = 0;
                    bool secure_enabled = false;
                    std::string secure_server;
                    int secure_port = 0;
                    bool auto_enabled = false;
                    std::string auto_url;
                    std::vector<std::string> bypass;
                };

                static std::vector<ProxyState> g_states;
                static bool g_captured = false;

                static std::string Trim(const std::string& value) noexcept {
                    std::size_t begin = 0;
                    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
                        ++begin;
                    }
                    std::size_t end = value.size();
                    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
                        --end;
                    }
                    return value.substr(begin, end - begin);
                }

                static std::string ShellQuote(const std::string& value) noexcept {
                    std::string result("'");
                    for (char ch : value) {
                        if (ch == '\'') {
                            result += "'\\''";
                        }
                        else {
                            result += ch;
                        }
                    }
                    result += '\'';
                    return result;
                }

                static bool Run(const std::string& command, std::string& output) noexcept {
                    output.clear();
                    FILE* pipe = popen(command.c_str(), "r");
                    if (NULLPTR == pipe) {
                        return false;
                    }

                    char buffer[512];
                    while (fgets(buffer, sizeof(buffer), pipe) != NULLPTR) {
                        output.append(buffer);
                    }
                    return pclose(pipe) == 0;
                }

                static bool Run(const std::string& command) noexcept {
                    std::string ignored;
                    return Run(command, ignored);
                }

                static std::string Field(const std::string& output, const char* name) noexcept {
                    std::istringstream stream(output);
                    std::string line;
                    const std::string prefix = std::string(name) + ":";
                    while (std::getline(stream, line)) {
                        line = Trim(line);
                        if (line.compare(0, prefix.size(), prefix) == 0) {
                            return Trim(line.substr(prefix.size()));
                        }
                    }
                    return std::string();
                }

                static bool Yes(const std::string& value) noexcept {
                    std::string lower = value;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    return lower == "yes" || lower == "on" || lower == "1";
                }

                static std::vector<std::string> Split(const std::string& value) noexcept {
                    std::vector<std::string> result;
                    std::string item;
                    for (char ch : value) {
                        if (ch == ';' || ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
                            item = Trim(item);
                            if (!item.empty()) {
                                result.emplace_back(std::move(item));
                            }
                            item.clear();
                        }
                        else {
                            item += ch;
                        }
                    }
                    item = Trim(item);
                    if (!item.empty()) {
                        result.emplace_back(std::move(item));
                    }
                    return result;
                }

                static bool ReadProxy(const std::string& service, const char* kind,
                    bool& enabled, std::string& server, int& port) noexcept {
                    std::string output;
                    std::string command = "/usr/sbin/networksetup -get" + std::string(kind) + "proxy " + ShellQuote(service);
                    if (!Run(command, output)) {
                        return false;
                    }
                    enabled = Yes(Field(output, "Enabled"));
                    server = Field(output, "Server");
                    port = atoi(Field(output, "Port").c_str());
                    return true;
                }

                static bool ReadAutoProxy(const std::string& service, bool& enabled, std::string& url) noexcept {
                    std::string state;
                    if (!Run("/usr/sbin/networksetup -getautoproxystate " + ShellQuote(service), state)) {
                        return false;
                    }
                    enabled = Yes(Field(state, "Enabled"));

                    std::string output;
                    if (!Run("/usr/sbin/networksetup -getautoproxyurl " + ShellQuote(service), output)) {
                        return false;
                    }
                    url = Field(output, "URL");
                    return true;
                }

                static std::vector<std::string> ReadBypass(const std::string& service) noexcept {
                    std::string output;
                    std::vector<std::string> result;
                    if (!Run("/usr/sbin/networksetup -getproxybypassdomains " + ShellQuote(service), output)) {
                        return result;
                    }
                    std::istringstream stream(output);
                    std::string line;
                    while (std::getline(stream, line)) {
                        line = Trim(line);
                        if (!line.empty() && line.find("aren't any") == std::string::npos) {
                            result.emplace_back(std::move(line));
                        }
                    }
                    return result;
                }

                static bool Capture() noexcept {
                    if (g_captured) {
                        return true;
                    }

                    std::string output;
                    if (!Run("/usr/sbin/networksetup -listallnetworkservices", output)) {
                        return false;
                    }

                    std::istringstream stream(output);
                    std::string service;
                    bool first = true;
                    while (std::getline(stream, service)) {
                        service = Trim(service);
                        if (first) {
                            first = false;
                            continue;
                        }
                        if (service.empty() || service[0] == '*') {
                            continue;
                        }

                        std::string enabled_output;
                        if (!Run("/usr/sbin/networksetup -getnetworkserviceenabled " + ShellQuote(service), enabled_output) ||
                            Field(enabled_output, "Enabled") != "Yes") {
                            continue;
                        }

                        ProxyState state;
                        state.service = service;
                        if (!ReadProxy(service, "web", state.web_enabled, state.web_server, state.web_port) ||
                            !ReadProxy(service, "secureweb", state.secure_enabled, state.secure_server, state.secure_port) ||
                            !ReadAutoProxy(service, state.auto_enabled, state.auto_url)) {
                            continue;
                        }
                        state.bypass = ReadBypass(service);
                        g_states.emplace_back(std::move(state));
                    }

                    g_captured = !g_states.empty();
                    return g_captured;
                }

                static bool SetProxy(const std::string& service, const char* kind,
                    const std::string& host, int port, bool enabled) noexcept {
                    if (host.empty() || port <= 0) {
                        return Run("/usr/sbin/networksetup -set" + std::string(kind) + "proxystate " +
                            ShellQuote(service) + " off");
                    }
                    return Run("/usr/sbin/networksetup -set" + std::string(kind) + "proxy " +
                        ShellQuote(service) + " " + ShellQuote(host) + " " + std::to_string(port)) &&
                        Run("/usr/sbin/networksetup -set" + std::string(kind) + "proxystate " +
                            ShellQuote(service) + " " + (enabled ? "on" : "off"));
                }

                static bool SetBypass(const std::string& service, const std::vector<std::string>& bypass) noexcept {
                    std::string command = "/usr/sbin/networksetup -setproxybypassdomains " + ShellQuote(service);
                    for (const std::string& item : bypass) {
                        command += " " + ShellQuote(item);
                    }
                    return Run(command);
                }

                static bool SetAutoProxy(const ProxyState& state, bool enabled, const std::string& url) noexcept {
                    if (enabled && !url.empty() &&
                        !Run("/usr/sbin/networksetup -setautoproxyurl " + ShellQuote(state.service) + " " + ShellQuote(url))) {
                        return false;
                    }
                    return Run("/usr/sbin/networksetup -setautoproxystate " + ShellQuote(state.service) + " " +
                        (enabled ? "on" : "off"));
                }

                static bool Apply(const std::string& server, const std::vector<std::string>& bypass) noexcept {
                    std::size_t separator = server.rfind(':');
                    if (separator == std::string::npos) {
                        return false;
                    }
                    std::string host = server.substr(0, separator);
                    if (host.size() > 1 && host.front() == '[' && host.back() == ']') {
                        host = host.substr(1, host.size() - 2);
                    }
                    int port = atoi(server.substr(separator + 1).c_str());
                    if (host.empty() || port <= 0 || port > 65535 || !Capture()) {
                        return false;
                    }

                    bool ok = true;
                    for (const ProxyState& state : g_states) {
                        ok = SetProxy(state.service, "web", host, port, true) && ok;
                        ok = SetProxy(state.service, "secureweb", host, port, true) && ok;
                        ok = SetAutoProxy(state, false, std::string()) && ok;
                        ok = SetBypass(state.service, bypass) && ok;
                    }
                    return ok;
                }

                static bool Restore() noexcept {
                    if (!g_captured) {
                        return true;
                    }
                    bool ok = true;
                    for (const ProxyState& state : g_states) {
                        ok = SetProxy(state.service, "web", state.web_server, state.web_port, state.web_enabled) && ok;
                        ok = SetProxy(state.service, "secureweb", state.secure_server, state.secure_port, state.secure_enabled) && ok;
                        ok = SetAutoProxy(state, state.auto_enabled, state.auto_url) && ok;
                        ok = SetBypass(state.service, state.bypass) && ok;
                    }
                    g_states.clear();
                    g_captured = false;
                    return ok;
                }

                static const std::vector<std::string>& DefaultBypass() noexcept {
                    static const std::vector<std::string> bypass = {
                        "localhost", "127.*", "::1"
                    };
                    return bypass;
                }
            }

            bool HttpProxy::RefreshSystemProxy() noexcept {
                // networksetup writes the preferences immediately.  Keeping
                // this method as a no-op mirrors the WinInet API used on
                // Windows and keeps the caller platform-neutral.
                return true;
            }

            bool HttpProxy::SetSystemProxy(const ppp::string& server) noexcept {
                return SetSystemProxy(server, "localhost;127.*;::1");
            }

            bool HttpProxy::SetSystemProxy(const ppp::string& server, const ppp::string& bypass) noexcept {
                if (server.empty()) {
                    return Restore();
                }
                return Apply(server, Split(bypass));
            }

            bool HttpProxy::SetSystemProxy(const ppp::string& server, const ppp::string& pac, bool enable) noexcept {
                (void)pac;
                if (!enable) {
                    return Restore();
                }
                return Apply(server, DefaultBypass());
            }
        }
    }
}
