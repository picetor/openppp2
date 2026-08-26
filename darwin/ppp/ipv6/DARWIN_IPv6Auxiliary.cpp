#include <darwin/ppp/ipv6/IPv6Auxiliary.h>
#include <ppp/ipv6/IPv6Packet.h>
#include <ppp/diagnostics/Error.h>

#include <common/unix/UnixAfx.h>

#include <sys/sysctl.h>

namespace ppp {
    namespace darwin {
        namespace ipv6 {
            namespace auxiliary {
                namespace {
                    static int  server_original_forwarding = -1;
                    static bool server_forwarding_changed = false;
                    static bool server_pf_nat66_applied = false;
                    static ppp::string server_pf_nat66_anchor = "openppp2/nat66";
                    static ppp::string server_pf_nat66_interface;

                    static bool IsSafeShellToken(const ppp::string& value) noexcept {
                        if (value.empty()) {
                            return false;
                        }

                        for (char ch : value) {
                            bool ok =
                                (ch >= 'a' && ch <= 'z') ||
                                (ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9') ||
                                ch == ':' || ch == '.' || ch == '_' || ch == '-' || ch == '%' || ch == '/';
                            if (!ok) {
                                return false;
                            }
                        }

                        return true;
                    }

                    static bool IsPfEnabled() noexcept {
                        FILE* pipe = popen("pfctl -s info 2>/dev/null", "r");
                        if (NULLPTR == pipe) {
                            return false;
                        }

                        bool enabled = false;
                        char buffer[512];
                        while (fgets(buffer, sizeof(buffer), pipe) != NULLPTR) {
                            ppp::string line = buffer;
                            if (line.find("Status: Enabled") != ppp::string::npos) {
                                enabled = true;
                                break;
                            }
                        }
                        pclose(pipe);
                        return enabled;
                    }

                    static bool IsPfAnchorMounted(const ppp::string& anchor) noexcept {
                        if (!IsSafeShellToken(anchor)) {
                            return false;
                        }

                        FILE* pipe = popen("pfctl -sr 2>/dev/null", "r");
                        if (NULLPTR == pipe) {
                            return false;
                        }

                        ppp::string needle = "anchor \"" + anchor + "\"";
                        bool mounted = false;
                        char buffer[1024];
                        while (fgets(buffer, sizeof(buffer), pipe) != NULLPTR) {
                            if (ppp::string(buffer).find(needle) != ppp::string::npos) {
                                mounted = true;
                                break;
                            }
                        }
                        pclose(pipe);
                        return mounted;
                    }

                    static bool RunPfctlRules(const ppp::string& anchor, const ppp::string& rules) noexcept {
                        if (!IsSafeShellToken(anchor) || rules.empty()) {
                            return false;
                        }

                        ppp::string command = "pfctl -a " + anchor + " -f - 2>/dev/null";
                        FILE* pipe = popen(command.data(), "w");
                        if (NULLPTR == pipe) {
                            return false;
                        }

                        size_t written = fwrite(rules.data(), 1, rules.size(), pipe);
                        int result = pclose(pipe);
                        return written == rules.size() && result == 0;
                    }

                    static bool FlushPfctlAnchor(const ppp::string& anchor) noexcept {
                        if (!IsSafeShellToken(anchor)) {
                            return false;
                        }

                        char command[512];
                        snprintf(command, sizeof(command), "pfctl -a %s -F all >/dev/null 2>&1", anchor.data());
                        return system(command) == 0;
                    }
                }

                void ReadPrimaryDefaultRoute(ppp::string& interface_name, ppp::string& gateway) noexcept;

                bool PrepareServerEnvironment(const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration, const ppp::string& preferred_nic, const ppp::string& transit_ifname) noexcept {
                    if (NULLPTR == configuration) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6AuxiliaryPrepareServerEnvironmentNullConfig);
                    }
                    const auto mode = configuration->server.ipv6.mode;
                    if (mode != ppp::configurations::AppConfiguration::IPv6Mode_Gua && mode != ppp::configurations::AppConfiguration::IPv6Mode_Nat66) {
                        return true;
                    }

                    if (mode == ppp::configurations::AppConfiguration::IPv6Mode_Nat66) {
                        if (!IsPfEnabled() || !IsPfAnchorMounted(server_pf_nat66_anchor)) {
                            return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6Nat66Unavailable);
                        }

                        ppp::string uplink = preferred_nic;
                        if (uplink.empty()) {
                            ppp::string default_gateway;
                            ReadPrimaryDefaultRoute(uplink, default_gateway);
                        }
                        server_pf_nat66_interface = uplink;

                        ppp::string prefix = configuration->server.ipv6.cidr;
                        std::size_t slash = prefix.find('/');
                        int prefix_length = configuration->server.ipv6.prefix_length;
                        if (slash != ppp::string::npos) {
                            ppp::string length = prefix.substr(slash + 1);
                            prefix = prefix.substr(0, slash);
                            prefix_length = atoi(length.data());
                        }
                        prefix_length = std::max<int>(0, std::min<int>(128, prefix_length));
                        if (server_pf_nat66_interface.empty() || transit_ifname.empty() || prefix.empty() || !IsSafeShellToken(server_pf_nat66_interface) || !IsSafeShellToken(transit_ifname) || !IsSafeShellToken(prefix)) {
                            return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6Nat66Unavailable);
                        }

                        char rules[4096];
                        snprintf(rules, sizeof(rules),
                            "nat on %s inet6 from %s/%d to any -> (%s)\n"
                            "pass quick on %s inet6 from %s/%d to any keep state\n",
                            server_pf_nat66_interface.data(), prefix.data(), prefix_length, server_pf_nat66_interface.data(),
                            transit_ifname.data(), prefix.data(), prefix_length);

                        if (!RunPfctlRules(server_pf_nat66_anchor, rules)) {
                            server_pf_nat66_applied = false;
                            return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6Nat66Unavailable);
                        }
                        server_pf_nat66_applied = true;
                    }

                    int value = 0;
                    size_t value_size = sizeof(value);
                    if (::sysctlbyname("net.inet6.ip6.forwarding", &value, &value_size, NULLPTR, 0) != 0) {
                        if (server_pf_nat66_applied) {
                            FlushPfctlAnchor(server_pf_nat66_anchor);
                            server_pf_nat66_applied = false;
                        }
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6ForwardingEnableFailed);
                    }

                    server_original_forwarding = value;
                    server_forwarding_changed = false;
                    if (value == 0) {
                        int enabled = 1;
                        if (::sysctlbyname("net.inet6.ip6.forwarding", NULLPTR, NULLPTR, &enabled, sizeof(enabled)) != 0) {
                            if (server_pf_nat66_applied) {
                                FlushPfctlAnchor(server_pf_nat66_anchor);
                                server_pf_nat66_applied = false;
                            }
                            return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6ForwardingEnableFailed);
                        }
                        server_forwarding_changed = true;
                    }
                    return true;
                }

                void FinalizeServerEnvironment(const std::shared_ptr<ppp::configurations::AppConfiguration>& configuration, const ppp::string& preferred_nic, const ppp::string& transit_ifname) noexcept {
                    (void)configuration;
                    (void)preferred_nic;
                    (void)transit_ifname;
                    if (server_forwarding_changed && server_original_forwarding >= 0) {
                        int original = server_original_forwarding;
                        ::sysctlbyname("net.inet6.ip6.forwarding", NULLPTR, NULLPTR, &original, sizeof(original));
                    }
                    if (server_pf_nat66_applied) {
                        FlushPfctlAnchor(server_pf_nat66_anchor);
                    }
                    server_original_forwarding = -1;
                    server_forwarding_changed = false;
                    server_pf_nat66_applied = false;
                    server_pf_nat66_interface.clear();
                }

                ppp::string ComputeNetworkAddress(const boost::asio::ip::address_v6& address, int prefix_length) noexcept {
                    boost::asio::ip::address_v6::bytes_type bytes = address.to_bytes();
                    prefix_length = std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length));

                    int full_bytes = prefix_length / 8;
                    int remainder_bits = prefix_length % 8;
                    if (full_bytes < 16) {
                        if (remainder_bits != 0) {
                            unsigned char mask = static_cast<unsigned char>(0xff << (8 - remainder_bits));
                            bytes[full_bytes] &= mask;
                            full_bytes++;
                        }

                        for (int i = full_bytes; i < 16; ++i) {
                            bytes[i] = 0;
                        }
                    }

                    return stl::transform<ppp::string>(boost::asio::ip::address_v6(bytes).to_string());
                }

                void ReadPrimaryDefaultRoute(ppp::string& interface_name, ppp::string& gateway) noexcept {
                    interface_name.clear();
                    gateway.clear();

                    FILE* pipe = popen("route -n get -inet6 default", "r");
                    if (NULLPTR == pipe) {
                        return;
                    }

                    char buffer[1024];
                    while (fgets(buffer, sizeof(buffer), pipe) != NULLPTR) {
                        ppp::string line = buffer;
                        if (auto position = line.find("interface:"); position != ppp::string::npos) {
                            ppp::string value = ATrim(line.substr(position + 10));
                            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
                                value.pop_back();
                            }

                            interface_name = value;
                        }

                        if (auto position = line.find("gateway:"); position != ppp::string::npos) {
                            ppp::string value = ATrim(line.substr(position + 8));
                            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
                                value.pop_back();
                            }

                            if (value != "default") {
                                gateway = value;
                            }
                        }
                    }

                    pclose(pipe);
                }

                static ppp::vector<ppp::string> ReadDefaultRoutes() noexcept {
                    ppp::vector<ppp::string> routes;

                    FILE* pipe = popen("netstat -rn -f inet6", "r");
                    if (NULLPTR == pipe) {
                        return routes;
                    }

                    char buffer[1024];
                    while (fgets(buffer, sizeof(buffer), pipe) != NULLPTR) {
                        ppp::string line = ATrim<ppp::string>(buffer);
                        if (line.empty()) {
                            continue;
                        }

                        if (line.find("default") != 0) {
                            continue;
                        }

                        if (line.find("Internet6") != ppp::string::npos || line.find("Destination") != ppp::string::npos) {
                            continue;
                        }

                        std::istringstream stream(std::string(line.data(), line.size()));
                        ppp::vector<ppp::string> tokens;
                        std::string token;
                        while (stream >> token) {
                            tokens.emplace_back(token.data(), token.size());
                        }

                        if (tokens.size() < 2 || tokens[0] != "default") {
                            continue;
                        }

                        ppp::string gateway = tokens[1];
                        if (!gateway.empty() && gateway.find("link#") == 0) {
                            gateway.clear();
                        }

                        ppp::string interface_name;
                        for (std::size_t i = 2; i < tokens.size(); ++i) {
                            if (tokens[i].find("utun") == 0 || tokens[i].find("en") == 0 || tokens[i].find("bridge") == 0 || tokens[i].find("pdp_ip") == 0) {
                                interface_name = tokens[i];
                            }
                        }

                        if (interface_name.empty()) {
                            continue;
                        }

                        routes.emplace_back("if=" + interface_name + ";gw=" + gateway);
                    }

                    pclose(pipe);
                    return routes;
                }

                static bool ApplyDefaultRouteSnapshot(const ppp::string& route) noexcept {
                    if (route.empty()) {
                        return false;
                    }

                    ppp::vector<ppp::string> segments;
                    if (ppp::Tokenize<ppp::string>(route, segments, ";") < 2) {
                        return false;
                    }

                    ppp::string gateway;
                    ppp::string interface_name;

                    for (const ppp::string& segment : segments) {
                        std::size_t pos = segment.find('=');
                        if (pos == ppp::string::npos) {
                            continue;
                        }

                        ppp::string key = segment.substr(0, pos);
                        ppp::string value = segment.substr(pos + 1);
                        if (key == "if") {
                            interface_name = value;
                        }
                        elif (key == "gw") {
                            gateway = value;
                        }
                    }

                    if (interface_name.empty()) {
                        ppp::string current_interface;
                        ppp::string current_gateway;
                        ReadPrimaryDefaultRoute(current_interface, current_gateway);
                        interface_name = current_interface;
                    }

                    if (interface_name.empty()) {
                        return false;
                    }

                    return SetRoute(interface_name, "::", 0, gateway);
                }

                bool IsCurrentDefaultRoute(const ppp::string& interface_name, const ppp::string& gateway) noexcept {
                    ppp::string current_interface;
                    ppp::string current_gateway;
                    ReadPrimaryDefaultRoute(current_interface, current_gateway);

                    if (!gateway.empty()) {
                        return gateway == current_gateway;
                    }

                    return !interface_name.empty() && interface_name == current_interface;
                }

                bool SetRoute(const ppp::string& ifrName, const ppp::string& addressIP, int prefix_length, const ppp::string& gw) noexcept {
                    if (!IsSafeShellToken(ifrName) || !IsSafeShellToken(addressIP) || (!gw.empty() && !IsSafeShellToken(gw))) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::DarwinIPv6RouteCommandUnsafeToken);
                        return false;
                    }

                    char add_cmd[1200];
                    char change_cmd[1200];
                    if (addressIP == "::" && prefix_length == 0) {
                        if (gw.empty()) {
                            snprintf(add_cmd, sizeof(add_cmd), "route -n add -inet6 default -interface %s > /dev/null 2>&1", ifrName.data());
                        }
                        else {
                            snprintf(add_cmd, sizeof(add_cmd), "route -n add -inet6 default %s > /dev/null 2>&1", gw.data());
                        }

                        if (system(add_cmd) == 0) {
                            return true;
                        }

                        if (IsCurrentDefaultRoute(ifrName, gw)) {
                            return true;
                        }

                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RouteAddFailed);
                        return false;
                    }

                    elif (gw.empty()) {
                        snprintf(add_cmd, sizeof(add_cmd), "route -n add -inet6 %s/%d -interface %s > /dev/null 2>&1", addressIP.data(), std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length)), ifrName.data());
                        snprintf(change_cmd, sizeof(change_cmd), "route -n change -inet6 %s/%d -interface %s > /dev/null 2>&1", addressIP.data(), std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length)), ifrName.data());
                    }
                    else {
                        snprintf(add_cmd, sizeof(add_cmd), "route -n add -inet6 %s/%d %s > /dev/null 2>&1", addressIP.data(), std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length)), gw.data());
                        snprintf(change_cmd, sizeof(change_cmd), "route -n change -inet6 %s/%d %s > /dev/null 2>&1", addressIP.data(), std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length)), gw.data());
                    }

                    if (system(add_cmd) == 0) {
                        return true;
                    }

                    if (system(change_cmd) == 0) {
                        return true;
                    }

                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RouteReplaceFailed);
                    return false;
                }

                bool DeleteRoute(const ppp::string& ifrName, const ppp::string& addressIP, int prefix_length, const ppp::string& gw) noexcept {
                    if (!IsSafeShellToken(ifrName) || !IsSafeShellToken(addressIP) || (!gw.empty() && !IsSafeShellToken(gw))) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::DarwinIPv6RouteCommandUnsafeToken);
                        return false;
                    }

                    char cmd[1200];
                    if (addressIP == "::" && prefix_length == 0) {
                        if (gw.empty()) {
                            snprintf(cmd, sizeof(cmd), "route -n delete -inet6 default -interface %s > /dev/null 2>&1", ifrName.data());
                        }
                        else {
                            snprintf(cmd, sizeof(cmd), "route -n delete -inet6 default %s > /dev/null 2>&1", gw.data());
                        }
                    }
                    elif (gw.empty()) {
                        snprintf(cmd, sizeof(cmd), "route -n delete -inet6 %s/%d -interface %s > /dev/null 2>&1", addressIP.data(), std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length)), ifrName.data());
                    }
                    else {
                        snprintf(cmd, sizeof(cmd), "route -n delete -inet6 %s/%d %s > /dev/null 2>&1", addressIP.data(), std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length)), gw.data());
                    }
                    if (system(cmd) == 0) {
                        return true;
                    }

                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::RouteDeleteFailed);
                    return false;
                }

                void CaptureClientOriginalState(const ::ppp::ipv6::auxiliary::ClientContext& context, bool nat_mode, ::ppp::ipv6::auxiliary::ClientState& state) noexcept {
                    state.OriginalDnsConfiguration = ppp::unix__::UnixAfx::GetDnsResolveConfiguration();
                    state.OriginalDefaultRoutes = ReadDefaultRoutes();

                    ReadPrimaryDefaultRoute(state.OriginalDefaultRouteInterface, state.OriginalDefaultRoute);
                    state.DefaultRouteWasPresent = !state.OriginalDefaultRouteInterface.empty();
                }

                bool ApplyClientAddress(const ::ppp::ipv6::auxiliary::ClientContext& context, const boost::asio::ip::address& address, int prefix_length, bool gua_mode, ::ppp::ipv6::auxiliary::ClientState& state) noexcept {
                    if (NULLPTR == context.Tap || context.InterfaceIndex < 0 || !IsSafeShellToken(context.InterfaceName) || !address.is_v6()) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::NetworkInterfaceConfigureFailed);
                    }

                    boost::asio::ip::address_v6 addr_v6 = address.to_v6();
                    if (addr_v6.is_unspecified() || addr_v6.is_multicast() || addr_v6.is_loopback() || addr_v6.is_link_local()) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6AddressUnsafe);
                    }

                    prefix_length = std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length));
                    if (prefix_length < ppp::ipv6::IPv6_MAX_PREFIX_LENGTH && addr_v6 == ppp::ipv6::ComputeNetworkAddress(addr_v6, prefix_length)) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6AddressUnsafe);
                    }

                    ppp::string addr_str = stl::transform<ppp::string>(address.to_string());
                    char cmd[600];
                    snprintf(cmd, sizeof(cmd), "ifconfig %s inet6 %s prefixlen %d alias > /dev/null 2>&1", context.InterfaceName.data(), addr_str.data(), prefix_length);

                    if (system(cmd) != 0) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6ClientAddressApplyFailed);
                    }

                    state.AddressApplied = true;
                    state.Address = addr_str;
                    return true;
                }

                bool ApplyClientDefaultRoute(const ::ppp::ipv6::auxiliary::ClientContext& context, const boost::asio::ip::address& gateway, bool nat_mode, ::ppp::ipv6::auxiliary::ClientState& state) noexcept {
                    if (context.InterfaceName.empty()) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::NetworkInterfaceConfigureFailed);
                        return false;
                    }

                    ppp::string gateway_string;
                    if (gateway.is_v6()) {
                        gateway_string = stl::transform<ppp::string>(gateway.to_string());
                    }
                    elif (!nat_mode) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6GatewayMissing);
                    }

                    // Split ::/0 into ::/1 and 8000::/1 (same approach as IPv4's
                    // 0.0.0.0/1 + 128.0.0.0/1) to avoid overwriting any existing
                    // default route on the physical NIC. The server's /128 pin route
                    // will naturally take priority over the less specific /1 routes.
                    if (!SetRoute(context.InterfaceName, "::", 1, gateway_string)) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::IPv6ClientRouteApplyFailed);
                        return false;
                    }
                    if (!SetRoute(context.InterfaceName, "8000::", 1, gateway_string)) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::IPv6ClientRouteApplyFailed);
                        return false;
                    }

                    state.DefaultRouteApplied = true;
                    state.DefaultRouteGateway = gateway_string;
                    return true;
                }

                bool ApplyClientSubnetRoute(const ::ppp::ipv6::auxiliary::ClientContext& context, const boost::asio::ip::address& prefix, int prefix_length, const boost::asio::ip::address& gateway, bool nat_mode, ::ppp::ipv6::auxiliary::ClientState& state) noexcept {
                    if (!nat_mode) {
                        return true;
                    }

                    if (context.InterfaceName.empty() || !prefix.is_v6()) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::NetworkInterfaceConfigureFailed);
                        return false;
                    }

                    ppp::string gateway_string;
                    if (gateway.is_v6()) {
                        gateway_string = stl::transform<ppp::string>(gateway.to_string());
                    }
                    elif (!nat_mode) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6GatewayMissing);
                    }

                    ppp::string prefix_string = stl::transform<ppp::string>(prefix.to_string());
                    prefix_length = std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length));
                    if (!SetRoute(context.InterfaceName, prefix_string, prefix_length, gateway_string)) {
                        ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::IPv6ClientRouteApplyFailed);
                        return false;
                    }

                    state.SubnetRouteApplied = true;
                    state.SubnetRoutePrefix = prefix_string;
                    state.SubnetRoutePrefixLength = prefix_length;
                    state.SubnetRouteGateway = gateway_string;
                    return true;
                }

                bool ApplyClientDns(const ::ppp::ipv6::auxiliary::ClientContext& context, const ppp::vector<ppp::string>& dns_servers, ::ppp::ipv6::auxiliary::ClientState& state) noexcept {
                    if (dns_servers.empty()) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6ClientDnsApplyFailed);
                    }

                    ppp::vector<boost::asio::ip::address> dns_addrs;
                    ppp::vector<boost::asio::ip::address> current_addrs;
                    ppp::unix__::UnixAfx::GetDnsAddresses(current_addrs);
                    for (auto& s : dns_servers) {
                        boost::system::error_code ec;
                        auto addr = StringToAddress(s, ec);
                        if (!ec && addr.is_v6()) {
                            dns_addrs.emplace_back(addr);
                        }
                    }

                    if (dns_addrs.empty() || !ppp::unix__::UnixAfx::MergeDnsAddresses(dns_addrs, current_addrs)) {
                        return ppp::diagnostics::SetLastError(ppp::diagnostics::ErrorCode::IPv6ClientDnsApplyFailed);
                    }

                    state.DnsApplied = true;
                    state.DnsServers = dns_servers;
                    return true;
                }

                void RestoreClientConfiguration(const ::ppp::ipv6::auxiliary::ClientContext& context, const boost::asio::ip::address& address, int prefix_length, bool nat_mode, ::ppp::ipv6::auxiliary::ClientState& state) noexcept {
                    if (context.InterfaceName.empty()) {
                        return;
                    }

                    if (state.SubnetRouteApplied && !state.SubnetRoutePrefix.empty()) {
                        DeleteRoute(context.InterfaceName, state.SubnetRoutePrefix, state.SubnetRoutePrefixLength, state.SubnetRouteGateway);
                    }

                    if (state.DefaultRouteApplied) {
                        // Match the ::/1 + 8000::/1 split added in ApplyClientDefaultRoute
                        DeleteRoute(context.InterfaceName, "::", 1, state.DefaultRouteGateway);
                        DeleteRoute(context.InterfaceName, "8000::", 1, state.DefaultRouteGateway);
                    }

                    if (state.AddressApplied && address.is_v6() && !state.Address.empty()) {
                        char cmd[600];
                        int delete_prefix = std::max<int>(ppp::ipv6::IPv6_MIN_PREFIX_LENGTH, std::min<int>(ppp::ipv6::IPv6_MAX_PREFIX_LENGTH, prefix_length));
                        snprintf(cmd, sizeof(cmd), "ifconfig %s inet6 %s/%d delete > /dev/null 2>&1", context.InterfaceName.data(), state.Address.data(), delete_prefix);

                        system(cmd);
                    }

                    if (state.DnsApplied) {
                        ppp::unix__::UnixAfx::SetDnsResolveConfiguration(state.OriginalDnsConfiguration);
                    }

                    if (state.DefaultRouteApplied && state.DefaultRouteWasPresent) {
                        bool restored = false;
                        for (const ppp::string& route : state.OriginalDefaultRoutes) {
                            bool ok = ApplyDefaultRouteSnapshot(route);
                            restored |= ok;
                        }

                        if (!restored && !state.OriginalDefaultRouteInterface.empty()) {
                            SetRoute(state.OriginalDefaultRouteInterface, "::", 0, state.OriginalDefaultRoute);
                        }
                    }
                }
            }
        }
    }
}
