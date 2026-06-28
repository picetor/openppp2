#include <windows/ppp/win32/network/NetworkInterface.h>

#include <ppp/stdafx.h>
#include <ppp/net/IPEndPoint.h>

#include <Windows.h>
#include <Iphlpapi.h>
#include <netioapi.h>

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
        }
    }
}
