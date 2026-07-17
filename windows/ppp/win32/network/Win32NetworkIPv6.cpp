#include <windows/ppp/win32/network/NetworkInterface.h>

#include <ppp/stdafx.h>
#include <ppp/net/IPEndPoint.h>

#include <Windows.h>
#include <ws2tcpip.h>
#include <Iphlpapi.h>
#include <netioapi.h>
#include <cstdio>
#include <sstream>
#include <string>

#ifndef PPP_POPEN
#if defined(_MSC_VER)
#define PPP_POPEN  ::_popen
#define PPP_PCLOSE ::_pclose
#else
#define PPP_POPEN  ::popen
#define PPP_PCLOSE ::pclose
#endif
#endif

#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Netapi32.lib")

namespace ppp
{
    namespace win32
    {
        namespace network
        {
            static bool ExecuteNetshCommand(const ppp::string& command) noexcept
            {
                if (command.empty())
                {
                    return false;
                }

                ppp::string cmd = "netsh " + command + " > nul 2>&1";
                int result = ::system(cmd.c_str());
                return result == 0;
            }

            bool SetIPv6DefaultRoute(int interface_index, int metric) noexcept
            {
                if (interface_index < 0)
                {
                    return false;
                }

                // Add an IPv6 default route via the specified interface.
                // Using netsh: netsh interface ipv6 add route ::/0 <interface_index> metric=<metric>
                char command[512];
                if (metric > 0)
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 add route ::/0 %d metric=%d",
                        interface_index, metric);
                }
                else
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 add route ::/0 %d",
                        interface_index);
                }

                return ExecuteNetshCommand(command);
            }

            bool SetIPv6DefaultGateway(int interface_index, const ppp::string& gateway, int metric) noexcept
            {
                if (interface_index < 0 || gateway.empty())
                {
                    return false;
                }

                // Add an IPv6 default route (gateway) via the specified interface.
                // Using netsh: netsh interface ipv6 add route ::/0 <interface_index> <gateway> metric=<metric>
                char command[512];
                if (metric > 0)
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 add route ::/0 %d %s metric=%d",
                        interface_index, gateway.c_str(), metric);
                }
                else
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 add route ::/0 %d %s",
                        interface_index, gateway.c_str());
                }

                return ExecuteNetshCommand(command);
            }

            bool SetIPv6Address(int interface_index, const ppp::string& address, int prefix_length) noexcept
            {
                if (interface_index < 0 || address.empty() || prefix_length < 0)
                {
                    return false;
                }

                // Add an IPv6 address to the specified interface.
                // Using netsh: netsh interface ipv6 add address <interface_index> <address>/<prefix_length>
                char command[512];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 add address %d %s/%d",
                    interface_index, address.c_str(), prefix_length);

                return ExecuteNetshCommand(command);
            }

            bool SetIPv6AddressSkipAsSource(int interface_index, const ppp::string& address) noexcept
            {
                if (interface_index < 0 || address.empty())
                {
                    return false;
                }

                // Use Win32 Iphlpapi to set SkipAsSource on the IPv6 address.
                // netsh interface ipv6 set address does NOT support skipassource.
                // We use GetUnicastIpAddressEntry / SetUnicastIpAddressEntry from netioapi.h.
                MIB_UNICASTIPADDRESS_ROW row;
                InitializeUnicastIpAddressEntry(&row);
                row.InterfaceIndex = static_cast<NET_IFINDEX>(interface_index);
                row.Address.si_family = AF_INET6;

                // Parse the IPv6 address string into the sockaddr_in6.
                if (::inet_pton(AF_INET6, address.c_str(), &row.Address.Ipv6.sin6_addr) != 1) {
                    return false;
                }

                // On-link prefix length must be non-zero for GetUnicastIpAddressEntry to work.
                // Use /64 as default since the auto-generated fd00:: address uses /64.
                row.OnLinkPrefixLength = 64;

                ULONG result = GetUnicastIpAddressEntry(&row);
                if (result != NO_ERROR) {
                    return false;
                }

                row.SkipAsSource = TRUE;
                result = SetUnicastIpAddressEntry(&row);
                return result == NO_ERROR;
            }

            bool AddIPv6Neighbor(int interface_index, const ppp::string& address, const ppp::string& mac) noexcept
            {
                if (interface_index < 0 || address.empty() || mac.empty())
                {
                    return false;
                }

                // Add a static IPv6 neighbor entry (NDP cache). Keep it active-only;
                // a persistent entry can survive an abnormal VPN termination.
                char command[512];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 add neighbors %d %s %s store=active",
                    interface_index, address.c_str(), mac.c_str());

                return ExecuteNetshCommand(command);
            }

            bool DeleteIPv6Neighbor(int interface_index, const ppp::string& address) noexcept
            {
                if (interface_index < 0 || address.empty())
                {
                    return false;
                }

                char command[512];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 delete neighbors %d %s",
                    interface_index, address.c_str());

                return ExecuteNetshCommand(command);
            }

            bool AddIPv6Route(int interface_index, const ppp::string& prefix, int prefix_length, const ppp::string& gateway, int metric) noexcept
            {
                if (interface_index < 0 || prefix.empty() || prefix_length < 0)
                {
                    return false;
                }

                // Add an IPv6 route.
                // Using netsh: netsh interface ipv6 add route <prefix>/<length> <interface_index> <gateway> metric=<metric>
                char command[1024];
                if (!gateway.empty())
                {
                    if (metric > 0)
                    {
                        ::snprintf(command, sizeof(command),
                            "interface ipv6 add route %s/%d %d %s metric=%d",
                            prefix.c_str(), prefix_length, interface_index, gateway.c_str(), metric);
                    }
                    else
                    {
                        ::snprintf(command, sizeof(command),
                            "interface ipv6 add route %s/%d %d %s",
                            prefix.c_str(), prefix_length, interface_index, gateway.c_str());
                    }
                }
                else
                {
                    if (metric > 0)
                    {
                        ::snprintf(command, sizeof(command),
                            "interface ipv6 add route %s/%d %d metric=%d",
                            prefix.c_str(), prefix_length, interface_index, metric);
                    }
                    else
                    {
                        ::snprintf(command, sizeof(command),
                            "interface ipv6 add route %s/%d %d",
                            prefix.c_str(), prefix_length, interface_index);
                    }
                }

                return ExecuteNetshCommand(command);
            }

            bool DeleteIPv6DefaultGateway(int interface_index, const ppp::string& gateway) noexcept
            {
                if (interface_index < 0)
                {
                    return false;
                }

                // Delete the IPv6 default route on the specified interface.
                // Using netsh: netsh interface ipv6 delete route ::/0 <interface_index> [<gateway>]
                char command[512];
                if (!gateway.empty())
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 delete route ::/0 %d %s",
                        interface_index, gateway.c_str());
                }
                else
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 delete route ::/0 %d",
                        interface_index);
                }

                return ExecuteNetshCommand(command);
            }

            bool DeleteIPv6Route(int interface_index, const ppp::string& prefix, int prefix_length, const ppp::string& gateway) noexcept
            {
                if (interface_index < 0 || prefix.empty() || prefix_length < 0)
                {
                    return false;
                }

                // Delete an IPv6 route.
                // Using netsh: netsh interface ipv6 delete route <prefix>/<length> <interface_index> [<gateway>]
                char command[1024];
                if (!gateway.empty())
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 delete route %s/%d %d %s",
                        prefix.c_str(), prefix_length, interface_index, gateway.c_str());
                }
                else
                {
                    ::snprintf(command, sizeof(command),
                        "interface ipv6 delete route %s/%d %d",
                        prefix.c_str(), prefix_length, interface_index);
                }

                return ExecuteNetshCommand(command);
            }

            bool DeleteIPv6Address(int interface_index, const ppp::string& address) noexcept
            {
                if (interface_index < 0 || address.empty())
                {
                    return false;
                }

                // Delete an IPv6 address from the specified interface.
                // Using netsh: netsh interface ipv6 delete address <interface_index> <address>
                char command[512];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 delete address %d %s",
                    interface_index, address.c_str());

                return ExecuteNetshCommand(command);
            }

            bool SetDnsAddressesV6(int interface_index, const ppp::vector<ppp::string>& servers) noexcept
            {
                if (interface_index < 0 || servers.empty())
                {
                    return false;
                }

                // Set IPv6 DNS servers for the specified interface.
                // Using netsh: netsh interface ipv6 set dnsservers <interface_index> static <address> primary
                //              netsh interface ipv6 add dnsservers <interface_index> <address> [index=2|3|...]
                bool success = false;
                for (size_t i = 0; i < servers.size(); i++)
                {
                    if (servers[i].empty())
                    {
                        continue;
                    }

                    char command[1024];
                    if (i == 0)
                    {
                        ::snprintf(command, sizeof(command),
                            "interface ipv6 set dnsservers %d static %s primary validate=no",
                            interface_index, servers[i].c_str());
                    }
                    else
                    {
                        ::snprintf(command, sizeof(command),
                            "interface ipv6 add dnsservers %d %s index=%zu validate=no",
                            interface_index, servers[i].c_str(), i + 1);
                    }

                    if (ExecuteNetshCommand(command))
                    {
                        success = true;
                    }
                }

                return success;
            }

            bool ClearDnsAddressesV6(int interface_index) noexcept
            {
                if (interface_index < 0)
                {
                    return false;
                }

                char command[256];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 delete dnsservers %d all",
                    interface_index);

                return ExecuteNetshCommand(command);
            }

            int GetAllNicsDnsAddressesV6(ppp::unordered_map<int, ppp::vector<ppp::string>>& dns_map) noexcept
            {
                dns_map.clear();

                ULONG bufLen = 15000;
                ppp::vector<BYTE> buffer(bufLen);
                PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
                DWORD ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, NULLPTR, pAddresses, &bufLen);
                if (ret == ERROR_BUFFER_OVERFLOW)
                {
                    buffer.resize(bufLen);
                    pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                    ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, NULLPTR, pAddresses, &bufLen);
                }

                if (ret != NO_ERROR)
                {
                    return 0;
                }

                int count = 0;
                for (PIP_ADAPTER_ADDRESSES p = pAddresses; p != NULLPTR; p = p->Next)
                {
                    if (p->OperStatus != IfOperStatusUp)
                    {
                        continue;
                    }

                    ppp::vector<ppp::string> dns_v6_list;
                    for (PIP_ADAPTER_DNS_SERVER_ADDRESS dns = p->FirstDnsServerAddress; dns != NULLPTR; dns = dns->Next)
                    {
                        if (dns->Address.lpSockaddr->sa_family == AF_INET6)
                        {
                            SOCKADDR_IN6* addr6 = reinterpret_cast<SOCKADDR_IN6*>(dns->Address.lpSockaddr);
                            // Skip link-local and loopback addresses used by TAP DHCPv6 or similar.
                            if (addr6->sin6_addr.u.Byte[0] == 0xFE && (addr6->sin6_addr.u.Byte[1] & 0xC0) == 0x80)
                            {
                                continue;
                            }
                            char buf[INET6_ADDRSTRLEN];
                            if (NULLPTR != ::inet_ntop(AF_INET6, &addr6->sin6_addr, buf, sizeof(buf)))
                            {
                                dns_v6_list.emplace_back(ppp::string(buf));
                            }
                        }
                    }

                    if (!dns_v6_list.empty())
                    {
                        dns_map[(int)p->IfIndex] = std::move(dns_v6_list);
                        count++;
                    }
                }
                return count;
            }

            int SetAllNicsDnsAddressesV6(ppp::unordered_map<int, ppp::vector<ppp::string>>& dns_map) noexcept
            {
                int events = 0;
                for (auto&& [interface_index, servers] : dns_map)
                {
                    if (!servers.empty())
                    {
                        if (SetDnsAddressesV6(interface_index, servers))
                        {
                            events++;
                        }
                    }
                }
                dns_map.clear();
                return events;
            }

            bool SetIPv6PrefixPolicy(const ppp::string& prefix, int precedence, int label) noexcept
            {
                if (prefix.empty())
                {
                    return false;
                }

                // Configure IPv6 prefix policy to adjust source address selection.
                // Try "add" first (normal case: entry doesn't exist yet).
                // If add fails (e.g. entry already exists from a crashed previous run),
                // fall back to "set" to update the existing entry in-place.
                // Using netsh: netsh interface ipv6 add|set prefixpolicy <prefix> <precedence> <label>
                char command[512];

                // 1) Try add (normal first-run path)
                ::snprintf(command, sizeof(command),
                    "interface ipv6 add prefixpolicy %s %d %d",
                    prefix.c_str(), precedence, label);

                if (ExecuteNetshCommand(command))
                {
                    return true;
                }

                // 2) add failed — entry likely already exists, try set as fallback
                ::snprintf(command, sizeof(command),
                    "interface ipv6 set prefixpolicy %s %d %d",
                    prefix.c_str(), precedence, label);

                return ExecuteNetshCommand(command);
            }

            static int QueryGlobalUnicastLabel() noexcept
            {
                // Dynamically query the label of ::/0 from the current prefix policy table.
                // This is needed because different Windows versions assign different labels
                // to the default global unicast prefix: Win10 uses label=1, Win11 uses label=2.
                //
                // We parse the output of: netsh interface ipv6 show prefixpolicies
                // Format: "        30      2  ::/0"  (precedence, label, prefix)
                //
                // Returns label on success, -1 on failure.

                std::string cmd = "netsh interface ipv6 show prefixpolicies 2>nul";
                FILE* pipe = PPP_POPEN(cmd.c_str(), "r");
                if (NULLPTR == pipe) {
                    return -1;
                }

                char buffer[512];
                std::string output;
                while (::fgets(buffer, sizeof(buffer), pipe) != NULLPTR) {
                    output += buffer;
                }
                PPP_PCLOSE(pipe);

                if (output.empty()) {
                    return -1;
                }

                // Parse each line looking for "::/0" (or "::/0 " with trailing space).
                // Each data line has format: precedence, label, prefix
                std::istringstream iss(output);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;

                    // Look for "::/0" in the line (could be "::/0" or "::ffff:0:0/96" etc.)
                    std::size_t pos = line.find("::/0");
                    if (pos == std::string::npos) continue;

                    // Make sure it's exactly "::/0" and not "::/0 " followed by more, 
                    // or "::ffff:0:0/96" (contains "::/0" as substring)
                    // "::/0" has len 4; next char should be space, end-of-line, or EOS
                    std::size_t end_pos = pos + 4;  // "::/0" is 4 chars
                    if (end_pos < line.size()) {
                        char next = line[end_pos];
                        if (next != ' ' && next != '\r' && next != '\n' && next != '\t') {
                            continue;  // e.g., "::ffff:0:0/96" — skip
                        }
                    }

                    // Parse: read space-delimited ints; the prefix is the last token
                    // Tokens: spaces, precedence(int), spaces, label(int), spaces, prefix(string)
                    int precedence = -1, label = -1;
                    std::istringstream liss(line);
                    if (liss >> precedence >> label) {
                        return label;
                    }
                }

                return -1;
            }

            bool SetIPv6PrefixPolicyPreferULA() noexcept
            {
                // Elevate ULA prefix (fd00::/8) precedence above global unicast (::/0).
                // Default: ::/0 precedence=30 (Win10) or 40 (Win11), fc00::/7 precedence=3
                // After:   fc00::/7 stays at 3, but fd00::/8 gets precedence=50 to win over ::/0.
                //
                // CRITICAL: The label MUST match ::/0's label for RFC 6724 rule 5
                // (prefer matching label). This varies: Win10 uses label=1, Win11 uses label=2.
                // We dynamically query the current ::/0 label rather than hardcoding.
                constexpr int ULA_PREFERRED_PRECEDENCE = 50;
                constexpr int ULA_LABEL_FALLBACK = 2;  // Win11 default, most common now

                int label = QueryGlobalUnicastLabel();
                if (label <= 0) {
                    label = ULA_LABEL_FALLBACK;
                    LOG_WARN("SetIPv6PrefixPolicyPreferULA: could not query ::/0 label, using fallback=%d", label);
                }
                else {
                    LOG_DEBUG("SetIPv6PrefixPolicyPreferULA: queried ::/0 label=%d", label);
                }

                return SetIPv6PrefixPolicy("fd00::/8", ULA_PREFERRED_PRECEDENCE, label);
            }

            bool RestoreIPv6PrefixPolicyULA() noexcept
            {
                // Remove the fd00::/8 prefix policy entry that was added by
                // SetIPv6PrefixPolicyPreferULA(). The original fc00::/7 entry
                // (precedence=3, label=13) remains untouched and handles ULA matching.
                constexpr const char* ULA_PREFIX = "fd00::/8";
                char command[512];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 delete prefixpolicy %s",
                    ULA_PREFIX);

                return ExecuteNetshCommand(command);
            }

            bool SetIPv6InterfaceMetric(int interface_index, int metric) noexcept
            {
                if (interface_index < 0 || metric < 0)
                {
                    return false;
                }

                // Lower the interface metric so Windows DNS client prefers
                // this interface's DNS servers over physical NIC's.
                // Using netsh: netsh interface ipv6 set interface <ifindex> metric=<value>
                char command[256];
                ::snprintf(command, sizeof(command),
                    "interface ipv6 set interface %d metric=%d",
                    interface_index, metric);

                return ExecuteNetshCommand(command);
            }
        }
    }
}
