#include <windows/ppp/tap/TapWindows.h>
#include <windows/ppp/win32/Win32Native.h>
#include <windows/ppp/win32/network/NetworkInterface.h>
#include <windows/ppp/win32/network/Router.h>
#include <windows/ppp/tap/tap-windows.h>

#include <ppp/io/File.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/text/Encoding.h>

#include <windows/ppp/tap/WintunAdapter.h>

#include <iostream>
#include <Windows.h>
#include <process.h>
#include <Shlwapi.h>
#include <Shellapi.h>
#include <iphlpapi.h>

#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")

typedef ppp::net::IPEndPoint IPEndPoint;
typedef ppp::net::Ipep       Ipep;

namespace ppp
{
    namespace tap
    {
        static ppp::string TapWindows_FindComponentId(const ppp::string& key, ppp::win32::network::NetworkInterfacePtr& network_interface) noexcept;
        static std::atomic<TapWindows::DriverMode> TAP_WINDOWS_DRIVER_MODE(TapWindows::DriverMode::Auto);

        // Wintun creates a new interface while a previous TAP instance may
        // still retain the old IPv4 address.  AddIPAddress() reports
        // ERROR_OBJECT_ALREADY_EXISTS in that situation, but the error does
        // not mean that the requested address belongs to the target interface.
        // Keep all ownership checks here so the startup path cannot continue
        // with an APIPA-only Wintun interface.
        static bool GetIPv4AddressOwners(uint32_t ip, ppp::vector<int>& owners) noexcept
        {
            owners.clear();

            ULONG buffer_length = 15000;
            ppp::vector<BYTE> buffer(buffer_length);
            ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
            DWORD result = ::GetAdaptersAddresses(AF_INET, flags, NULLPTR,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_length);
            if (result == ERROR_BUFFER_OVERFLOW)
            {
                buffer.resize(buffer_length);
                result = ::GetAdaptersAddresses(AF_INET, flags, NULLPTR,
                    reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_length);
            }
            if (result != NO_ERROR)
            {
                fprintf(stdout, "[SetAddresses] GetAdaptersAddresses failed err=%lu\r\n",
                    static_cast<unsigned long>(result));
                return false;
            }

            for (PIP_ADAPTER_ADDRESSES adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                adapter != NULLPTR; adapter = adapter->Next)
            {
                for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
                    unicast != NULLPTR; unicast = unicast->Next)
                {
                    if (NULLPTR == unicast->Address.lpSockaddr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET)
                    {
                        continue;
                    }

                    const SOCKADDR_IN* address = reinterpret_cast<const SOCKADDR_IN*>(unicast->Address.lpSockaddr);
                    if (address->sin_addr.S_un.S_addr == ip)
                    {
                        owners.emplace_back(static_cast<int>(adapter->IfIndex));
                        break;
                    }
                }
            }
            return true;
        }

        static bool HasIPv4Address(int interface_index, uint32_t ip) noexcept
        {
            ppp::vector<int> owners;
            if (!GetIPv4AddressOwners(ip, owners))
            {
                return false;
            }
            return std::find(owners.begin(), owners.end(), interface_index) != owners.end();
        }

        static bool WaitForIPv4Address(int interface_index, uint32_t ip) noexcept
        {
            for (int attempt = 0; attempt < 10; ++attempt)
            {
                if (HasIPv4Address(interface_index, ip))
                {
                    return true;
                }
                ::Sleep(50);
            }
            return false;
        }

        static bool HasIPv4Route(int interface_index, uint32_t destination, uint32_t mask, uint32_t gateway) noexcept
        {
            std::shared_ptr<MIB_IPFORWARDTABLE> table = ppp::win32::network::Router::GetIpForwardTable();
            if (NULLPTR == table)
            {
                return false;
            }

            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                const MIB_IPFORWARDROW& route = table->table[i];
                if (static_cast<int>(route.dwForwardIfIndex) == interface_index &&
                    route.dwForwardDest == destination &&
                    route.dwForwardMask == mask &&
                    route.dwForwardNextHop == gateway)
                {
                    return true;
                }
            }
            return false;
        }

        static bool IsIPv4SubnetInUse(uint32_t network, uint32_t mask, int ignored_interface_index) noexcept
        {
            ULONG buffer_length = 15000;
            ppp::vector<BYTE> buffer(buffer_length);
            ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
            DWORD result = ::GetAdaptersAddresses(AF_INET, flags, NULLPTR,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_length);
            if (result == ERROR_BUFFER_OVERFLOW)
            {
                buffer.resize(buffer_length);
                result = ::GetAdaptersAddresses(AF_INET, flags, NULLPTR,
                    reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_length);
            }
            if (result != NO_ERROR)
            {
                fprintf(stdout, "[SetAddresses] GetAdaptersAddresses subnet check failed err=%lu\r\n",
                    static_cast<unsigned long>(result));
                return true;
            }

            const uint32_t network_host = ntohl(network);
            const uint32_t mask_host = ntohl(mask);
            for (PIP_ADAPTER_ADDRESSES adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                adapter != NULLPTR; adapter = adapter->Next)
            {
                if (static_cast<int>(adapter->IfIndex) == ignored_interface_index)
                {
                    continue;
                }

                for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
                    unicast != NULLPTR; unicast = unicast->Next)
                {
                    if (NULLPTR == unicast->Address.lpSockaddr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET)
                    {
                        continue;
                    }

                    const SOCKADDR_IN* address = reinterpret_cast<const SOCKADDR_IN*>(unicast->Address.lpSockaddr);
                    if ((ntohl(address->sin_addr.S_un.S_addr) & mask_host) == network_host)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        static bool IsStaleVirtualAdapter(int interface_index) noexcept;

        static bool HasExternalIPv4AddressOwner(const ppp::vector<int>& owners, int interface_index) noexcept
        {
            for (int owner : owners)
            {
                if (owner != interface_index && !IsStaleVirtualAdapter(owner))
                {
                    return true;
                }
            }
            return false;
        }

        // Windows does not allow the same IPv4 address/subnet to be assigned
        // to two adapters.  Another PPP client may legitimately keep the
        // historical openppp2 address (192.168.12.68), so do not steal it.
        // For the normal /24 tunnel layout, preserve the host and gateway
        // portions and move only the private third octet to an unused subnet.
        static bool SelectAvailableIPv4Address(int interface_index, uint32_t& ip,
            uint32_t& gateway, uint32_t mask) noexcept
        {
            ppp::vector<int> owners;
            if (!GetIPv4AddressOwners(ip, owners))
            {
                return false;
            }
            if (!HasExternalIPv4AddressOwner(owners, interface_index))
            {
                return true;
            }

            const uint32_t mask_host = ntohl(mask);
            if (mask_host != 0xFFFFFF00U)
            {
                fprintf(stdout,
                    "[SetAddresses] IPv4 address conflict cannot auto-relocate non-/24 mask ip=%s mask=%s\r\n",
                    IPEndPoint(ip, 0).ToAddressString().data(),
                    IPEndPoint(mask, 0).ToAddressString().data());
                return false;
            }

            const uint32_t ip_host = ntohl(ip);
            const uint32_t gateway_host = ntohl(gateway);
            const uint32_t host_ip = ip_host & 0xFFU;
            uint32_t host_gateway = gateway_host & 0xFFU;
            if (host_ip == 0U || host_ip == 255U ||
                host_gateway == 0U || host_gateway == 255U || host_ip == host_gateway)
            {
                fprintf(stdout,
                    "[SetAddresses] IPv4 address conflict cannot auto-relocate invalid host layout ip=%s gateway=%s\r\n",
                    IPEndPoint(ip, 0).ToAddressString().data(),
                    IPEndPoint(gateway, 0).ToAddressString().data());
                return false;
            }

            const uint32_t first = (ip_host >> 24) & 0xFFU;
            const uint32_t second = (ip_host >> 16) & 0xFFU;
            const bool requested_private = first == 10U ||
                (first == 172U && second >= 16U && second <= 31U) ||
                (first == 192U && second == 168U);
            const uint32_t pool_prefix = requested_private ?
                (ip_host & 0xFFFF0000U) : 0xC0A80000U;
            const uint32_t requested_third = (ip_host >> 8) & 0xFFU;
            const uint32_t requested_network = ip_host & mask_host;

            for (uint32_t offset = 1; offset <= 255; ++offset)
            {
                const uint32_t third = (requested_third + offset) & 0xFFU;
                const uint32_t candidate_network_host = pool_prefix | (third << 8);
                if (candidate_network_host == requested_network ||
                    IsIPv4SubnetInUse(htonl(candidate_network_host), mask, interface_index))
                {
                    continue;
                }

                ip = htonl(candidate_network_host | host_ip);
                host_gateway = std::min<uint32_t>(host_gateway, 254U);
                gateway = htonl(candidate_network_host | host_gateway);
                fprintf(stdout,
                    "[SetAddresses] IPv4 subnet conflict resolved without touching other adapter: "
                    "requested=%s/%s selected=%s gateway=%s\r\n",
                    IPEndPoint(htonl(requested_network), 0).ToAddressString().data(),
                    IPEndPoint(mask, 0).ToAddressString().data(),
                    IPEndPoint(ip, 0).ToAddressString().data(),
                    IPEndPoint(gateway, 0).ToAddressString().data());
                return true;
            }

            fprintf(stdout, "[SetAddresses] no unused private /24 subnet available for ip=%s\r\n",
                IPEndPoint(ip, 0).ToAddressString().data());
            return false;
        }

        static bool IsStaleVirtualAdapter(int interface_index) noexcept
        {
            ppp::win32::network::NetworkInterfacePtr network_interface =
                ppp::win32::network::GetNetworkInterfaceByInterfaceIndex(interface_index);
            if (NULLPTR == network_interface)
            {
                return false;
            }

            // Only remove addresses from adapters carrying openppp2's own
            // marker. A generic TAP/Wintun/PPP label is not ownership proof;
            // for example, PPP 1 is a separate PPP client's TAP adapter.
            const char* const openppp2_marker = "PPP PRIVATE NETWORK 2";
            return (!network_interface->Description.empty() &&
                network_interface->Description.find(openppp2_marker) != ppp::string::npos) ||
                (!network_interface->ConnectionId.empty() &&
                    network_interface->ConnectionId.find(openppp2_marker) != ppp::string::npos);
        }

        static bool DeleteIPv4AddressByNetsh(int interface_index, uint32_t ip) noexcept
        {
            ppp::string interface_name = ppp::win32::network::GetInterfaceName(interface_index);
            IPEndPoint ipEP(ip, 0);
            if (interface_name.empty() || IPEndPoint::IsInvalid(ipEP))
            {
                fprintf(stdout, "[SetAddresses] cannot delete stale address idx=%d name='%s' ip=%u\r\n",
                    interface_index, interface_name.data(), ip);
                return false;
            }

            ppp::string arguments = "interface ipv4 delete address name=\"";
            arguments += interface_name;
            arguments += "\" address=";
            arguments += ipEP.ToAddressString();

            int return_code = INFINITE;
            const bool launched = ppp::win32::Win32Native::Execute(
                false, "netsh.exe", arguments.data(), &return_code, 3000);
            fprintf(stdout, "[SetAddresses] delete stale address idx=%d name='%s' ip=%s launched=%d rc=%d\r\n",
                interface_index, interface_name.data(), ipEP.ToAddressString().data(),
                launched ? 1 : 0, return_code);
            return launched && return_code == ERROR_SUCCESS;
        }

        static void DeleteStaleVirtualIPv4Routes(int interface_index, uint32_t ip,
            uint32_t mask, uint32_t gateway) noexcept
        {
            std::shared_ptr<MIB_IPFORWARDTABLE> table = ppp::win32::network::Router::GetIpForwardTable();
            if (NULLPTR == table)
            {
                return;
            }

            const uint32_t network = htonl(ntohl(ip) & ntohl(mask));
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                MIB_IPFORWARDROW& route = table->table[i];
                const bool connected_route = route.dwForwardDest == network &&
                    route.dwForwardMask == mask;
                const bool tunnel_gateway_route = !IPEndPoint::IsInvalid(IPEndPoint(gateway, 0)) &&
                    route.dwForwardNextHop == gateway;
                if (static_cast<int>(route.dwForwardIfIndex) != interface_index ||
                    (!connected_route && !tunnel_gateway_route))
                {
                    continue;
                }

                if (ppp::win32::network::Router::Delete(route))
                {
                    fprintf(stdout, "[SetAddresses] deleted stale route idx=%d dest=%u mask=%u gateway=%u\r\n",
                        interface_index, route.dwForwardDest, route.dwForwardMask, route.dwForwardNextHop);
                }
            }
        }

        static bool EnsureIPv4AddressOwnership(int interface_index, uint32_t ip,
            uint32_t mask, uint32_t gateway) noexcept
        {
            ppp::vector<int> owners;
            if (!GetIPv4AddressOwners(ip, owners))
            {
                return false;
            }

            bool ok = true;
            for (int owner : owners)
            {
                if (owner == interface_index)
                {
                    continue;
                }

                if (!IsStaleVirtualAdapter(owner))
                {
                    // Never remove an address from a physical or unrelated
                    // adapter merely because it collides with the tunnel.
                    fprintf(stdout, "[SetAddresses] address conflict ip=%u owner_idx=%d is not a stale PPP/TAP adapter\r\n",
                        ip, owner);
                    ok = false;
                    continue;
                }

                fprintf(stdout, "[SetAddresses] removing stale virtual address ip=%u owner_idx=%d target_idx=%d\r\n",
                    ip, owner, interface_index);
                DeleteStaleVirtualIPv4Routes(owner, ip, mask, gateway);
                if (!DeleteIPv4AddressByNetsh(owner, ip))
                {
                    ok = false;
                }
            }

            // netsh changes the IP configuration asynchronously from the
            // perspective of GetAdaptersAddresses.  Give the adapter a short
            // window to publish the new ownership before AddIPAddress runs.
            for (int attempt = 0; attempt < 10; ++attempt)
            {
                ppp::vector<int> current_owners;
                if (!GetIPv4AddressOwners(ip, current_owners))
                {
                    return false;
                }

                bool conflicting_owner = false;
                for (int owner : current_owners)
                {
                    if (owner != interface_index)
                    {
                        conflicting_owner = true;
                    }
                }

                if (!conflicting_owner)
                {
                    return ok;
                }

                ::Sleep(50);
            }

            fprintf(stdout, "[SetAddresses] stale address ownership was not resolved ip=%u target_idx=%d\r\n",
                ip, interface_index);
            return false;
        }

        TapWindows::TapWindows(const std::shared_ptr<boost::asio::io_context>& context, const ppp::string& id, void* tun, uint32_t address, uint32_t gw, uint32_t mask, bool hosted_network)
            : ITap(context, id, tun, address, gw, mask, hosted_network)
        {

        }

        /* Refer: https://github.com/liulilittle/SkylakeNAT/blob/master/SkylakeNAT/tap.cpp */
        static uint32_t dhcp_masq_addr(const uint32_t local, const uint32_t netmask, const int offset) noexcept
        {
            int dsa; /* DHCP server addr */

            if (offset < 0)
            {
                dsa = (local | (~netmask)) + offset;
            }
            else
            {
                dsa = (local & netmask) + offset;
            }

            if (dsa == local)
            {
                fprintf(stdout, "There is a clash between the --ifconfig local address and the internal DHCP server address"
                    "-- both are set to %s -- please use the --ip-win32 dynamic option to choose a different free address from the"
                    " --ifconfig subnet for the internal DHCP server\n", ppp::net::Ipep::ToAddress(dsa).to_string().data());
            }

            if ((local & netmask) != (dsa & netmask))
            {
                fprintf(stdout, "--ip-win32 dynamic [offset] : offset is outside of --ifconfig subnet\n");
            }

            return htonl(dsa);
        }

        bool TapWindows::DnsFlushResolverCache() noexcept
        {
            return ppp::win32::Win32Native::DnsFlushResolverCache();
        }

        bool TapWindows::SetDnsAddresses(int interface_index, ppp::vector<ppp::string>& servers) noexcept
        {
            return ppp::win32::network::SetDnsAddresses(interface_index, servers);
        }

        bool TapWindows::SetDnsAddresses(int interface_index, ppp::vector<uint32_t>& servers) noexcept
        {
            ppp::vector<ppp::string> addresses;
            for (uint32_t server : servers)
            {
                IPEndPoint ip(server, 0);
                if (IPEndPoint::IsInvalid(ip))
                {
                    continue;
                }

                ppp::string address = ip.ToAddressString();
                addresses.emplace_back(address);
            }
            return SetDnsAddresses(interface_index, addresses);
        }

        static bool SetAddressesByNetsh(int interface_index, uint32_t ip, uint32_t mask, uint32_t gw) noexcept
        {
            ppp::string interface_name = ppp::win32::network::GetInterfaceName(interface_index);
            if (interface_name.empty())
            {
                fprintf(stdout, "[SetAddresses] netsh fallback cannot resolve interface name idx=%d\r\n", interface_index);
                return false;
            }

            IPEndPoint ipEP(ip, 0);
            IPEndPoint maskEP(mask, 0);
            if (IPEndPoint::IsInvalid(ipEP) || IPEndPoint::IsInvalid(maskEP))
            {
                fprintf(stdout, "[SetAddresses] netsh fallback invalid address idx=%d ip=%u mask=%u\r\n",
                    interface_index, ip, mask);
                return false;
            }

            ppp::string arguments = "interface ipv4 set address name=\"";
            arguments += interface_name;
            arguments += "\" source=static address=";
            arguments += ipEP.ToAddressString();
            arguments += " mask=";
            arguments += maskEP.ToAddressString();

            IPEndPoint gwEP(gw, 0);
            if (!IPEndPoint::IsInvalid(gwEP))
            {
                arguments += " gateway=";
                arguments += gwEP.ToAddressString();
                arguments += " gwmetric=1";
            }

            int return_code = INFINITE;
            const bool launched = ppp::win32::Win32Native::Execute(false, "netsh.exe", arguments.data(), &return_code);
            fprintf(stdout, "[SetAddresses] netsh idx=%d name='%s' launched=%d rc=%d\r\n",
                interface_index, interface_name.data(), launched ? 1 : 0, return_code);
            return launched && return_code == ERROR_SUCCESS;
        }

        static bool SetAddressesByIpHelper(int interface_index, uint32_t ip, uint32_t mask, uint32_t gw) noexcept
        {
            if (interface_index <= 0)
            {
                fprintf(stdout, "[SetAddresses-iphlpapi] FAIL: invalid interface index=%d\r\n", interface_index);
                return false;
            }

            IPEndPoint ipEP(ip, 0);
            IPEndPoint maskEP(mask, 0);
            IPEndPoint gwEP(gw, 0);
            const bool has_gateway = !IPEndPoint::IsInvalid(gwEP);

            ULONG nte_context = 0;
            ULONG nte_instance = 0;
            // The address values used by this module are the same values as
            // in_addr.s_addr: a DWORD whose bytes are already in network order.
            // AddIPAddress expects that value directly; applying htonl here
            // would reverse the address a second time on Windows.
            DWORD address_error = ::AddIPAddress(
                ip, mask, static_cast<DWORD>(interface_index),
                &nte_context, &nte_instance);
            bool address_ok = address_error == NO_ERROR;
            if (!address_ok && (address_error == ERROR_OBJECT_ALREADY_EXISTS ||
                address_error == ERROR_ALREADY_EXISTS))
            {
                // ERROR_OBJECT_ALREADY_EXISTS is ambiguous here: the same
                // address may already exist on another adapter.  It is only
                // success when the target interface owns the address.
                address_ok = HasIPv4Address(interface_index, ip);
            }
            fprintf(stdout,
                "[SetAddresses-iphlpapi] AddIPAddress idx=%d ip=%s mask=%s err=%lu ctx=%lu verified=%d ok=%d\r\n",
                interface_index, ipEP.ToAddressString().data(), maskEP.ToAddressString().data(),
                static_cast<unsigned long>(address_error), static_cast<unsigned long>(nte_context),
                HasIPv4Address(interface_index, ip) ? 1 : 0, address_ok ? 1 : 0);

            if (!address_ok || !has_gateway)
            {
                return address_ok;
            }

            MIB_IPFORWARDROW route;
            ::ZeroMemory(&route, sizeof(route));
            route.dwForwardDest = 0;
            route.dwForwardMask = 0;
            // MIB_IPFORWARDROW address fields use the same network-order
            // representation as in_addr.s_addr.
            route.dwForwardNextHop = gw;
            route.dwForwardIfIndex = static_cast<DWORD>(interface_index);
            route.dwForwardType = MIB_IPROUTE_TYPE_INDIRECT;
            route.dwForwardProto = MIB_IPPROTO_NETMGMT;

            MIB_IPINTERFACE_ROW interface_row;
            ::ZeroMemory(&interface_row, sizeof(interface_row));
            interface_row.Family = AF_INET;
            interface_row.InterfaceIndex = static_cast<NET_IFINDEX>(interface_index);
            DWORD metric = 1;
            DWORD interface_error = ::GetIpInterfaceEntry(&interface_row);
            if (interface_error == NO_ERROR && interface_row.Metric > metric)
            {
                metric = interface_row.Metric;
            }
            route.dwForwardMetric1 = metric;

            DWORD route_error = ::CreateIpForwardEntry(&route);
            const bool route_ok = route_error == NO_ERROR ||
                ((route_error == ERROR_OBJECT_ALREADY_EXISTS ||
                    route_error == ERROR_ALREADY_EXISTS) &&
                    HasIPv4Route(interface_index, route.dwForwardDest,
                        route.dwForwardMask, route.dwForwardNextHop));
            fprintf(stdout,
                "[SetAddresses-iphlpapi] CreateIpForwardEntry idx=%d gateway=%s metric=%lu interface_err=%lu err=%lu verified=%d ok=%d\r\n",
                interface_index, gwEP.ToAddressString().data(),
                static_cast<unsigned long>(metric), static_cast<unsigned long>(interface_error),
                static_cast<unsigned long>(route_error),
                HasIPv4Route(interface_index, route.dwForwardDest,
                    route.dwForwardMask, route.dwForwardNextHop) ? 1 : 0,
                route_ok ? 1 : 0);
            return route_ok;
        }

        bool TapWindows::SetAddresses(int interface_index, uint32_t ip, uint32_t mask, uint32_t gw) noexcept
        {
            IPEndPoint ipEP(ip, 0);
            if (IPEndPoint::IsInvalid(ipEP))
            {
                fprintf(stdout, "[SetAddresses] FAIL: invalid IP (ip=%u)\r\n", ip);
                return false;
            }

            IPEndPoint maskEP(mask, 0);
            if (IPEndPoint::IsInvalid(maskEP))
            {
                fprintf(stdout, "[SetAddresses] FAIL: invalid mask (mask=%u)\r\n", mask);
                return false;
            }

            IPEndPoint gwEP(gw, 0);
            const bool has_gateway = !IPEndPoint::IsInvalid(gwEP);

            if (!EnsureIPv4AddressOwnership(interface_index, ip, mask, gw))
            {
                fprintf(stdout, "[SetAddresses] FAIL: IPv4 address ownership conflict idx=%d ip=%s\r\n",
                    interface_index, ipEP.ToAddressString().data());
                return false;
            }

            fprintf(stdout, "[SetAddresses] Using WMI: idx=%d ip=%s mask=%s gw=%s\r\n",
                interface_index, ipEP.ToAddressString().data(), maskEP.ToAddressString().data(),
                has_gateway ? gwEP.ToAddressString().data() : "none");

            bool wmi_ok = ppp::win32::network::SetIPAddresses(
                interface_index, { ipEP.ToAddressString() }, { maskEP.ToAddressString() });
            if (wmi_ok && has_gateway)
            {
                wmi_ok = ppp::win32::network::SetDefaultIPGateway(interface_index, { gwEP.ToAddressString() });
            }
            if (wmi_ok)
            {
                if (WaitForIPv4Address(interface_index, ip))
                {
                    return true;
                }
                fprintf(stdout, "[SetAddresses] WMI reported success but idx=%d does not own ip=%s; continuing with fallback\r\n",
                    interface_index, ipEP.ToAddressString().data());
            }

            fprintf(stdout, "[SetAddresses] WMI configuration failed; trying netsh\r\n");
            if (SetAddressesByNetsh(interface_index, ip, mask, gw) &&
                WaitForIPv4Address(interface_index, ip))
            {
                return true;
            }

            fprintf(stdout, "[SetAddresses] netsh reported success but idx=%d does not own ip=%s; continuing with IP Helper\r\n",
                interface_index, ipEP.ToAddressString().data());

            // Wintun can have a valid interface index before its friendly name
            // is exposed through the WMI adapter-configuration provider. In
            // that state netsh may launch successfully but return code 1. The
            // IP Helper API addresses the interface by index and works for
            // both Wintun and TAP without relying on the Control Panel name.
            fprintf(stdout, "[SetAddresses] netsh failed; trying IP Helper by interface index\r\n");
            const bool ip_helper_ok = SetAddressesByIpHelper(interface_index, ip, mask, gw);
            if (!ip_helper_ok || !WaitForIPv4Address(interface_index, ip))
            {
                fprintf(stdout, "[SetAddresses] FAIL: target idx=%d does not own ip=%s after all configuration methods\r\n",
                    interface_index, ipEP.ToAddressString().data());
                return false;
            }
            return true;
        }

        bool TapWindows::FindAllComponentIds(ppp::unordered_set<ppp::string>& componentIds) noexcept
        {
            return ppp::win32::network::GetAllComponentIds(componentIds);
        }

        static bool SetAdapterInterface(int interface_index, uint32_t ip, uint32_t gw, uint32_t mask, bool hosted_network, const ppp::vector<uint32_t>& dns_addresses) noexcept
        {
            ppp::vector<ppp::string> dns_addresses_stloc;
            Ipep::ToAddresses(dns_addresses, dns_addresses_stloc);

            ppp::vector<ppp::string> ips_stloc;
            Ipep::ToAddresses({ ip }, ips_stloc);

            ppp::vector<ppp::string> gw_stloc;
            Ipep::ToAddresses({ gw }, gw_stloc);

            ppp::vector<ppp::string> mask_stloc;
            Ipep::ToAddresses({ mask }, mask_stloc);

            bool set_addr_ok = false;
            if (hosted_network)
            {
                set_addr_ok = TapWindows::SetAddresses(interface_index, ip, mask, gw);
                fprintf(stdout, "[SetAdapterInterface] SetAddresses(hosted) = %s\r\n", set_addr_ok ? "OK" : "FAIL");
            }
            else
            {
                set_addr_ok = TapWindows::SetAddresses(interface_index, ip, mask, IPEndPoint::NoneAddress);
                fprintf(stdout, "[SetAdapterInterface] SetAddresses(non-hosted) = %s\r\n", set_addr_ok ? "OK" : "FAIL");
            }

            bool set_dns_ok = TapWindows::SetDnsAddresses(interface_index, dns_addresses_stloc);
            fprintf(stdout, "[SetAdapterInterface] SetDnsAddresses(WMI) = %s\r\n", set_dns_ok ? "OK" : "FAIL");

            if (!set_dns_ok && !dns_addresses_stloc.empty())
            {
                // WMI DNS failed, fallback to netsh.
                // TAP DHCP Option 6 pushes DNS to the driver, but Windows DHCP Client
                // may not always pick it up. netsh ensures DNS is set on the interface.
                ppp::string ifname = ppp::win32::network::GetInterfaceName(interface_index);
                fprintf(stdout, "[SetAdapterInterface] netsh DNS fallback on '%s' (idx=%d)...\r\n",
                    ifname.data(), interface_index);

                if (!ifname.empty())
                {
                    // Do not use system() here. Besides inheriting the shell,
                    // it makes netsh's default DNS validation run inside the
                    // core startup thread. On Wintun that validation can wait
                    // several seconds (or until the TUI kills the core), so
                    // RPC_LISTEN is never reached. Execute netsh directly and
                    // disable validation: DNS is a local adapter setting, and
                    // reachability is not a valid startup prerequisite.
                    {
                        ppp::string arguments = "interface ipv4 set dnsservers name=\"";
                        arguments += ifname;
                        arguments += "\" source=static address=";
                        arguments += dns_addresses_stloc[0];
                        arguments += " validate=no";
                        int rc = INFINITE;
                        const bool launched = ppp::win32::Win32Native::Execute(
                            false, "netsh.exe", arguments.data(), &rc, 3000);
                        fprintf(stdout, "[SetAdapterInterface] netsh set dns 1: %s (launched=%d rc=%d)\r\n",
                            dns_addresses_stloc[0].data(), launched ? 1 : 0, rc);
                    }
                    // Set secondary DNS. Keep the same no-validation behavior
                    // and log before/after every command so a future driver or
                    // Windows-version-specific delay is visible immediately.
                    for (size_t i = 1; i < dns_addresses_stloc.size() && i < 4; ++i)
                    {
                        const auto& s = dns_addresses_stloc[i];
                        if (s.empty() || s == "255.255.255.255") continue;
                        fprintf(stdout, "[SetAdapterInterface] netsh add dns %zu begin: %s\r\n",
                            i + 1, s.data());
                        ppp::string arguments = "interface ipv4 add dnsservers name=\"";
                        arguments += ifname;
                        arguments += "\" address=";
                        arguments += s;
                        arguments += " index=";
                        arguments += std::to_string(i + 1);
                        arguments += " validate=no";
                        int rc = INFINITE;
                        const bool launched = ppp::win32::Win32Native::Execute(
                            false, "netsh.exe", arguments.data(), &rc, 3000);
                        fprintf(stdout, "[SetAdapterInterface] netsh add dns %zu: %s (launched=%d rc=%d)\r\n",
                            i + 1, s.data(), launched ? 1 : 0, rc);
                    }
                }
            }

            fprintf(stdout, "[SetAdapterInterface] address/dns setup complete addr=%d dns=%d\r\n",
                set_addr_ok ? 1 : 0, set_dns_ok ? 1 : 0);

            return set_addr_ok;
        }

        struct WintunAdapterDriver final
        {
        public:
            static std::shared_ptr<ITap> CreateWintunAdapter(const std::shared_ptr<boost::asio::io_context>& context, const ppp::string& nic, uint32_t ip, uint32_t gw, uint32_t mask, bool hosted_network, const ppp::vector<uint32_t>& dns_addresses) noexcept
            {
                GUID* NULL_GUID = NULLPTR;
                std::shared_ptr<WintunAdapter> wintun = make_shared_object<WintunAdapter>(
                    ppp::text::Encoding::ascii_to_wstring(stl::transform<std::string>(nic)), L"PPP PRIVATE NETWORK 2", NULL_GUID, WintunAdapter::MAX_RING_BUFFER_SIZE);
                if (NULL == wintun)
                {
                    return NULLPTR;
                }

                if (!wintun->Open())
                {
                    fprintf(stdout, "[TapWindows::CreateWintunAdapter] WintunAdapter::Open failed\r\n");
                    wintun->Stop();
                    return NULLPTR;
                }

                int interface_index = wintun->GetInterfaceIndex();
                fprintf(stdout, "[TapWindows::CreateWintunAdapter] interface_index=%d\r\n", interface_index);
                if (interface_index < 0)
                {
                    wintun->Stop();
                    return NULLPTR;
                }

                if (!SelectAvailableIPv4Address(interface_index, ip, gw, mask))
                {
                    fprintf(stdout, "[TapWindows::CreateWintunAdapter] no usable IPv4 address/subnet\r\n");
                    wintun->Stop();
                    return NULLPTR;
                }

                if (!SetAdapterInterface(interface_index, ip, gw, mask, hosted_network, dns_addresses))
                {
                    fprintf(stdout, "[TapWindows::CreateWintunAdapter] SetAdapterInterface failed\r\n");
                    wintun->Stop();
                    return NULLPTR;
                }

                fprintf(stdout, "[TapWindows::CreateWintunAdapter] starting Wintun receive thread\r\n");
                if (!wintun->Start())
                {
                    fprintf(stdout, "[TapWindows::CreateWintunAdapter] WintunAdapter::Start failed\r\n");
                    wintun->Stop();
                    return NULLPTR;
                }
                fprintf(stdout, "[TapWindows::CreateWintunAdapter] Wintun receive thread started\r\n");

                std::shared_ptr<TapWindows> tap = make_shared_object<TapWindows>(context, nic, wintun.get(), ip, gw, mask, hosted_network);
                if (NULL == tap)
                {
                    wintun->Stop();
                    return NULLPTR;
                }

                tap->wintun_ = wrap_shared_pointer<void>(wintun.get(), wintun); // alias borrows wintun's refcount, keeping WintunAdapter alive
                tap->GetInterfaceIndex() = interface_index;
                return tap;
            }
        };

        std::shared_ptr<ITap> TapWindows::Create(const std::shared_ptr<boost::asio::io_context>& context, const ppp::string& componentId, uint32_t ip, uint32_t gw, uint32_t mask, uint32_t lease_time_in_seconds, bool hosted_network, const ppp::vector<uint32_t>& dns_addresses)
        {
            fprintf(stdout, "[TapWindows::Create] componentId=%s ip=%u gw=%u mask=%u\r\n", componentId.data(), ip, gw, mask);

            if (NULLPTR == context)
            {
                fprintf(stdout, "[TapWindows::Create] FAIL: NULLPTR == context\r\n");
                return NULLPTR;
            }

            if (componentId.empty())
            {
                fprintf(stdout, "[TapWindows::Create] FAIL: componentId.empty()\r\n");
                return NULLPTR;
            }

            IPEndPoint ipEP(ip, 0);
            if (IPEndPoint::IsInvalid(ipEP))
            {
                return NULLPTR;
            }

            IPEndPoint gwEP(gw, 0);
            if (IPEndPoint::IsInvalid(gwEP))
            {
                return NULLPTR;
            }

            IPEndPoint maskEP(mask, 0);
            if (IPEndPoint::IsInvalid(maskEP))
            {
                return NULLPTR;
            }

            if (lease_time_in_seconds < 1)
            {
                lease_time_in_seconds = 86400;
            }

            DriverMode driver_mode = GetDriverMode();
            if (driver_mode != DriverMode::Tap && WintunAdapter::Ready())
            {
                std::shared_ptr<ITap> wintun = WintunAdapterDriver::CreateWintunAdapter(
                    context, componentId, ip, gw, mask, hosted_network, dns_addresses);
                if (wintun)
                {
                    return wintun;
                }

                if (driver_mode == DriverMode::Wintun)
                {
                    return NULLPTR;
                }
            }
            elif(driver_mode == DriverMode::Wintun)
            {
                return NULLPTR;
            }

            ppp::win32::network::NetworkInterfacePtr tap_network_interface;
            ppp::string tap_component_id = TapWindows_FindComponentId(componentId, tap_network_interface);
            void* tun = tap_component_id.empty() ? NULLPTR : OpenDriver(tap_component_id.data());

            if ((NULLPTR == tun || tun == INVALID_HANDLE_VALUE) && driver_mode == DriverMode::Auto)
            {
                ppp::string driver_path = ppp::io::File::GetFullPath(
                    (ppp::GetApplicationStartupPath() + "\\Driver\\").data());
                tap_component_id = InstallDriver(driver_path.data(), componentId);
                if (!tap_component_id.empty())
                {
                    tun = OpenDriver(tap_component_id.data());
                }
            }

            fprintf(stdout, "[TapWindows::Create] OpenDriver('%s')=%p\r\n", tap_component_id.data(), tun);
            if (NULLPTR == tun || tun == INVALID_HANDLE_VALUE)
            {
                fprintf(stdout, "[TapWindows::Create] FAIL: OpenDriver failed (err=%d)\r\n", GetLastError());
                return NULLPTR;
            }

            int interface_index = GetNetworkInterfaceIndex(tap_component_id);
            fprintf(stdout, "[TapWindows::Create] GetNetworkInterfaceIndex=%d\r\n", interface_index);
            if (interface_index < 0)
            {
                fprintf(stdout, "[TapWindows::Create] FAIL: invalid interface index\r\n");
                CloseHandle(tun);
                return NULLPTR;
            }

            if (!SelectAvailableIPv4Address(interface_index, ip, gw, mask))
            {
                fprintf(stdout, "[TapWindows::Create] FAIL: no usable IPv4 address/subnet\r\n");
                CloseHandle(tun);
                return NULLPTR;
            }

            bool ok = ConfigureDriver_SetNetifUp(tun, true) &&
                (ConfigureDriver_SetTunModeWithAddress(tun, ip, gw, mask) || 
                    ConfigureDriver_SetTunModeWithAddress(tun, ip, (ip & mask), mask)) &&
                ConfigureDriver_SetDhcpMASQ(tun, ip, gw, mask, lease_time_in_seconds) &&
                ConfigureDriver_SetDhcpOptionData(tun, ip, gw, mask, gw, dns_addresses);

            if (!ok)
            {
                fprintf(stdout, "[TapWindows::Create] FAIL: ConfigureDriver failed\r\n");
                CloseHandle(tun);
                return NULLPTR;
            }

            std::shared_ptr<TapWindows> tap = make_shared_object<TapWindows>(context, tap_component_id, tun, ip, gw, mask, hosted_network);
            if (NULLPTR == tap)
            {
                CloseHandle(tun);
                return NULLPTR;
            }
            else 
            {
                tap->GetInterfaceIndex() = interface_index;
            }
            
            // Use WMI to configure IP, gateway, and DNS on the network interface.
            fprintf(stdout, "[TapWindows::Create] Setting adapter interface via WMI...\r\n");
            ok = SetAdapterInterface(interface_index, ip, gw, mask, hosted_network, dns_addresses);
            if (ok)
            {
                // Assign a default IPv6 ULA address to the TAP interface.
                // The IPv6 address is derived from the IPv4 IP for consistency:
                //   IPv4 192.168.12.68 → IPv6 fd00::c0a8:0c44/64
                // Mark it as SkipAsSource so Windows won't auto-select it for
                // outbound connections. Only the server-assigned IPv6 address
                // (applied later via ApplyIPv6Assignment) should be used as source.
                {
                    uint32_t ip6_a = (ip >> 24) & 0xFF, ip6_b = (ip >> 16) & 0xFF;
                    uint32_t ip6_c = (ip >> 8) & 0xFF, ip6_d = ip & 0xFF;
                    char ipv6_addr[64];
                    const int ipv6_prefix_len = 64;
                    ::snprintf(ipv6_addr, sizeof(ipv6_addr), "fd00::%02x%02x:%02x%02x", ip6_a, ip6_b, ip6_c, ip6_d);
                    ppp::string ipv6_addr_str(ipv6_addr);
                    ppp::win32::network::SetIPv6Address(interface_index, ipv6_addr_str, ipv6_prefix_len);
                    ppp::win32::network::SetIPv6AddressSkipAsSource(interface_index, ipv6_addr_str);
                    // Lower TAP interface metric to 5 so Windows DNS client
                    // prefers VPN DNS servers over physical NIC's DNS servers,
                    // preventing DNS leaks.
                    ppp::win32::network::SetIPv6InterfaceMetric(interface_index, 5);
                    fprintf(stdout, "[TapWindows::Create] Set IPv6 ULA on TAP: %s/%d (SkipAsSource, Metric=5)\r\n", ipv6_addr, ipv6_prefix_len);
                }

                // Initialize the async I/O stream from the TAP handle.
                if (!tap->InitializeStream())
                {
                    fprintf(stdout, "[TapWindows::Create] FAIL: InitializeStream failed\r\n");
                    CloseHandle(tun);
                    return NULLPTR;
                }

                fprintf(stdout, "[TapWindows::Create] SUCCESS!\r\n");
                return tap;
            }

            fprintf(stdout, "[TapWindows::Create] FAIL: SetAdapterInterface (WMI) failed\r\n");
            tap->Dispose();
            return NULLPTR;
        }

        void* TapWindows::OpenDriver(const ppp::string& componentId) noexcept
        {
            char szDeviceName[MAX_PATH];
            if (snprintf(szDeviceName, sizeof(szDeviceName), "\\\\.\\Global\\%s.tap", componentId.data()) < 1)
            {
                return NULLPTR;
            }

            HANDLE handle = CreateFileA(szDeviceName,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULLPTR,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_SYSTEM,
                NULLPTR);
            if (NULLPTR == handle || handle == INVALID_HANDLE_VALUE)
            {
                handle = NULLPTR;
            }

            return handle;
        }

        int TapWindows::GetNetworkInterfaceIndex(const ppp::string& componentId) noexcept
        {
            using NetworkInterface = ppp::win32::network::AdapterInterfacePtr;

            if (WintunAdapter::Ready())
            {
                int idx = ppp::win32::network::GetIfIndexByFriendlyName(ppp::text::Encoding::ascii_to_wstring(stl::transform<std::string>(componentId)));
                if (idx > 0) return idx;
                // Fall through to GUID matching if friendly name lookup failed
            }

            if (componentId.empty())
            {
                return -1;
            }

            ppp::vector<NetworkInterface> interfaces;
            if (!ppp::win32::network::GetAllAdapterInterfaces(interfaces))
            {
                return -1;
            }

            boost::uuids::uuid reft_id = StringToGuid(componentId);
            for (NetworkInterface& ni : interfaces)
            {
                boost::uuids::uuid left_id = StringToGuid(ni->Id);
                if (left_id == reft_id)
                {
                    return ni->IfIndex;
                }
            }

            return -1;
        }

        bool TapWindows::Output(const void* packet, int packet_size) noexcept
        {
            if (wintun_)
            {
                if (NULLPTR == packet || packet_size < 1)
                {
                    return true;
                }

                WintunAdapter* wintun = static_cast<WintunAdapter*>(GetHandle());
                if (!wintun->IsOpen())
                {
                    return false;
                }

                return wintun->SendPacket((uint8_t*)packet, packet_size);
            }

            return ITap::Output(packet, packet_size);
        }

        bool TapWindows::Output(const std::shared_ptr<Byte>& packet, int packet_size) noexcept
        {
            if (wintun_)
            {
                if (NULLPTR == packet || packet_size < 1)
                {
                    return true;
                }

                WintunAdapter* wintun = static_cast<WintunAdapter*>(GetHandle());
                if (!wintun->IsOpen())
                {
                    return false;
                }

                return wintun->SendPacket((uint8_t*)packet.get(), packet_size);
            }

            return ITap::Output(packet, packet_size);
        }

        bool TapWindows::AsynchronousReadPacketLoops() noexcept
        {
            if (wintun_)
            {
                WintunAdapter* wintun = static_cast<WintunAdapter*>(GetHandle());
                if (!wintun->IsOpen())
                {
                    LOG_DEBUG("TapWindows::AsynchronousReadPacketLoops: wintun not open");
                    return false;
                }

                auto packet_input = make_shared_object<WintunAdapter::PacketHandler>();
                if (NULLPTR == packet_input)
                {
                    return false;
                }

                auto self = shared_from_this();
                *packet_input =
                    [self, this](const uint8_t* data, uint32_t len) noexcept
                    {
                        int packet_length = std::max<int>(len, -1);
                        if (packet_length > 0)
                        {
                            PacketInputEventArgs e{ (char*)data, packet_length };
                            OnInput(e);
                        }
                    };
                wintun->PacketInput = packet_input;
                return true;
            }
            
            return ITap::AsynchronousReadPacketLoops();
        }

        bool TapWindows::ConfigureDriver_SetNetifUp(const void* handle, bool up) noexcept
        {
            if (NULLPTR == handle || handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            Byte media_status[] = { 1, 0, 0, 0 };
            if (!up)
            {
                media_status[0] = 0;
            }

            return ppp::win32::Win32Native::DeviceIoControl(handle, TAP_WIN_IOCTL_SET_MEDIA_STATUS, media_status, sizeof(media_status));
        }

        bool TapWindows::ConfigureDriver_SetDhcpMASQ(const void* handle, uint32_t ip, uint32_t gw, uint32_t mask, uint32_t lease_time_in_seconds) noexcept
        {
            if (NULLPTR == handle || handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            uint32_t dhcp[] =
            {
                ip,
                mask,
                gw,
                lease_time_in_seconds, /* lease time in seconds */
            };
            return ppp::win32::Win32Native::DeviceIoControl(handle, TAP_WIN_IOCTL_CONFIG_DHCP_MASQ, dhcp, sizeof(dhcp));
        }

        // Configures TAP-Windows driver for TUN mode operation (NOT TAP mode).
        // CRITICAL: In TUN mode, the driver requires the "gateway" parameter to be the NETWORK ADDRESS (ip & mask),
        // NOT the actual gateway IP (e.g., 10.0.0.1). This serves as the TUN interface's peer address per driver specification.
        //
        // Why this is necessary:
        //   - TAP-Windows driver in TUN mode expects network address (e.g., 10.0.0.0 for 10.0.0.0/24) as the peer endpoint
        //   - Actual gateway configuration is handled separately by SetAddresses():
        //        * hosted_network mode: Sets OS interface gateway to intended gw (e.g., 10.0.0.1)
        //        * non-hosted_network mode: Sets gateway to 0.0.0.0 (no gateway)
        //   - This separation resolves the historical inconsistency:
        //        * Driver layer: Uses network address (required by TAP-Windows TUN implementation)
        //        * OS network layer: Uses standard gateway (10.0.0.1) for cross-platform consistency
        //        * Other platforms (Linux/Unix): Configure gateway directly at OS layer without driver quirks
        //
        // Parameter note:
        //   gw MUST be (ip & mask) - passing actual gateway IP here will break TUN mode operation.
        //   See Create() implementation: ConfigureDriver_SetTunModeWithAddress(tun, ip, (ip & mask), mask)
        //
        // This function is essential for TUN mode initialization on Windows and MUST be called
        // with correctly computed network address. Do not confuse with OS-level gateway configuration.
        bool TapWindows::ConfigureDriver_SetTunModeWithAddress(const void* handle, uint32_t ip, uint32_t gw, uint32_t mask) noexcept
        {
            if (NULLPTR == handle || handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            uint32_t address[3] =
            {
                ip,
                gw,      // MUST be network address (ip & mask), NOT actual gateway
                mask,
            };
            return ppp::win32::Win32Native::DeviceIoControl(handle, TAP_WIN_IOCTL_CONFIG_TUN, address, sizeof(address));
        }

        bool TapWindows::ConfigureDriver_SetDhcpOptionData(const void* handle, uint32_t ip, uint32_t gw, uint32_t mask, uint32_t dhcp, const ppp::vector<uint32_t>& dns_addresses) noexcept
        {
            if (NULLPTR == handle || handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            ppp::vector<BYTE> dhcpOptionData;
            BYTE* ip_bytes = (BYTE*)&ip;
            BYTE* gw_bytes = (BYTE*)&gw;
            BYTE* mask_bytes = (BYTE*)&mask;
            BYTE* dhcp_bytes = (BYTE*)&dhcp;

            // IP地址 (Option 50: Requested IP Address)
            dhcpOptionData.emplace_back(0x32);
            dhcpOptionData.emplace_back(0x04);
            for (uint32_t i = 0; i < sizeof(uint32_t); i++)
            {
                dhcpOptionData.emplace_back(ip_bytes[i]);
            }

            // 子网地址 (Option 1: Subnet Mask)
            dhcpOptionData.emplace_back(0x01);
            dhcpOptionData.emplace_back(0x04);
            for (uint32_t i = 0; i < sizeof(uint32_t); i++)
            {
                dhcpOptionData.emplace_back(mask_bytes[i]);
            }

            // 网关服务器 (Option 3: Router/Gateway)
            dhcpOptionData.emplace_back(0x03);
            dhcpOptionData.emplace_back(0x04);
            for (uint32_t i = 0; i < sizeof(uint32_t); i++)
            {
                dhcpOptionData.emplace_back(gw_bytes[i]);
            }

            // DNS服务器
            {
                uint32_t dnsAddressesSize = 0;
                uint32_t dnsAddressesLocal[] = { 0, 0 };
                if (dns_addresses.size() > 1)
                {
                    dnsAddressesSize = sizeof(dnsAddressesLocal);
                    dnsAddressesLocal[0] = dns_addresses[0];
                    dnsAddressesLocal[1] = dns_addresses[1];
                }
                elif(dns_addresses.size() > 0)
                {
                    dnsAddressesSize = sizeof(*dnsAddressesLocal);
                    dnsAddressesLocal[0] = dns_addresses[0];
                }

                dhcpOptionData.emplace_back(0x06);
                dhcpOptionData.emplace_back(dnsAddressesSize);
                for (uint32_t i = 0; i < dnsAddressesSize; i++)
                {
                    BYTE* dnsAddressesBytes = (BYTE*)&dnsAddressesLocal[0];
                    dhcpOptionData.emplace_back(dnsAddressesBytes[i]);
                }
            }

            // DHCP服务器 (Option 54: DHCP Server Identifier)
            dhcpOptionData.emplace_back(0x36);
            dhcpOptionData.emplace_back(0x04);
            for (uint32_t i = 0; i < sizeof(uint32_t); i++)
            {
                dhcpOptionData.emplace_back(dhcp_bytes[i]);
            }

            return ppp::win32::Win32Native::DeviceIoControl(handle, TAP_WIN_IOCTL_CONFIG_DHCP_SET_OPT, dhcpOptionData.data(), (int)dhcpOptionData.size());
        }

        bool TapWindows::IsWintun() noexcept
        {
            return GetDriverMode() != DriverMode::Tap && WintunAdapter::Ready();
        }

        void TapWindows::SetDriverMode(DriverMode mode) noexcept
        {
            TAP_WINDOWS_DRIVER_MODE.store(mode, std::memory_order_release);
        }

        TapWindows::DriverMode TapWindows::GetDriverMode() noexcept
        {
            return TAP_WINDOWS_DRIVER_MODE.load(std::memory_order_acquire);
        }

        ppp::string TapWindows::FindComponentId() noexcept
        {
            ppp::unordered_set<ppp::string> componentIds;
            if (TapWindows::FindAllComponentIds(componentIds))
            {
                auto tail = componentIds.begin();
                auto endl = componentIds.end();
                if (tail != endl)
                {
                    return *tail;
                }
            }
            return ppp::string();
        }

        static ppp::string TapWindows_FindComponentId(const ppp::string& key, ppp::win32::network::NetworkInterfacePtr& network_interface) noexcept
        {
            ppp::string componentId = key;
            if (key.size() > 0)
            {
                componentId = LTrim<ppp::string>(componentId);
                componentId = RTrim<ppp::string>(componentId);
            }

            if (componentId.size() > 0)
            {
                using NetworkInterfacePtr = ppp::win32::network::NetworkInterfacePtr;

                ppp::vector<NetworkInterfacePtr> interfaces;
                if (ppp::win32::network::GetAllNetworkInterfaces(interfaces))
                {
                    bool component_uuid_sgen = false;
                    boost::uuids::uuid component_uuid;
                    boost::uuids::string_generator sgen;
                    try
                    {
                        component_uuid = sgen(componentId);
                        component_uuid_sgen = true;
                    }
                    catch (const std::exception&)
                    {
                        component_uuid_sgen = false;
                    }

                    ppp::string component_id = ToLower<ppp::string>(componentId);
                    auto is_tap_adapter = [](const NetworkInterfacePtr& ni) noexcept {
                        if (NULLPTR == ni) {
                            return false;
                        }
                        ppp::string description = ToLower<ppp::string>(ni->Description);
                        return description.find("tap-windows") != ppp::string::npos ||
                            description.find("tap0901") != ppp::string::npos;
                    };
                    std::size_t interfaces_size = interfaces.size();
                    for (std::size_t i = 0; i < interfaces_size; i++)
                    {
                        NetworkInterfacePtr& ni = interfaces[i];
                        // A friendly name can be shared by different virtual
                        // drivers. In TAP mode, never return a Wintun/PPP
                        // interface just because its connection name matches.
                        if (!is_tap_adapter(ni)) {
                            continue;
                        }
                        if (component_uuid_sgen)
                        {
                            if (StringToGuid(ni->Guid) == component_uuid)
                            {
                                network_interface = ni;
                                return ni->Guid;
                            }
                        }

                        ppp::string connection_id = ToLower<ppp::string>(ni->ConnectionId);
                        connection_id = LTrim<ppp::string>(connection_id);
                        connection_id = RTrim<ppp::string>(connection_id);
                        if (connection_id == component_id)
                        {
                            network_interface = ni;
                            return ni->Guid;
                        }
                    }
                }
                return ppp::string();
            }
            else
            {
                return TapWindows::FindComponentId();
            }
        }

        ppp::string TapWindows::FindComponentId(const ppp::string& key) noexcept
        {
            if (GetDriverMode() != DriverMode::Tap && WintunAdapter::Ready())
            {
                return key;
            }

            ppp::win32::network::NetworkInterfacePtr ni;
            return TapWindows_FindComponentId(key, ni);
        }

        ppp::string TapWindows::InstallDriver(const ppp::string& path, const ppp::string& declareTapName) noexcept
        {
            fprintf(stdout, "[TapInstall] begin path='%s' name='%s'\r\n", path.data(), declareTapName.data());
            if (path.empty() || declareTapName.empty())
            {
                fprintf(stdout, "[TapInstall] FAIL: empty driver path or adapter name\r\n");
                return ppp::string();
            }

            ppp::string installPath = ppp::io::File::RewritePath((path + "tapinstall.exe").data());
            if (!PathFileExistsA(installPath.data()))
            {
                fprintf(stdout, "[TapInstall] FAIL: tapinstall not found path='%s' err=%lu\r\n",
                    installPath.data(), static_cast<unsigned long>(GetLastError()));
                return ppp::string();
            }

            ppp::string driverPath = path + "OemVista.inf";
            ppp::string argumentsText = "install \"" + driverPath + "\" tap0901";

            ppp::unordered_set<ppp::string> olds;
            const bool old_query_ok = TapWindows::FindAllComponentIds(olds);
            fprintf(stdout, "[TapInstall] before install query ok=%d count=%zu\r\n",
                old_query_ok ? 1 : 0, olds.size());

            int dwExitCode = INFINITE;
            const bool launched = ppp::win32::Win32Native::Execute(false, installPath.data(), argumentsText.data(), &dwExitCode);
            const DWORD execute_error = launched ? ERROR_SUCCESS : GetLastError();
            fprintf(stdout, "[TapInstall] execute path='%s' args='%s' launched=%d exit=%d\r\n",
                installPath.data(), argumentsText.data(), launched ? 1 : 0, dwExitCode);
            if (!launched)
            {
                fprintf(stdout, "[TapInstall] FAIL: CreateProcess error=%lu\r\n",
                    static_cast<unsigned long>(execute_error));
                return ppp::string();
            }

            // tapinstall uses 3010 when the package was installed and Windows
            // reports that a reboot is required. The adapter can still be
            // opened in the current process, so treat it as a successful
            // install and validate the device below.
            if (dwExitCode != ERROR_SUCCESS && dwExitCode != ERROR_SUCCESS_REBOOT_REQUIRED)
            {
                fprintf(stdout, "[TapInstall] FAIL: tapinstall exit=%d\r\n", dwExitCode);
                return ppp::string();
            }

            ppp::unordered_set<ppp::string> news;
            bool found_new_component = false;
            // Device installation is asynchronous. Querying WMI immediately
            // after tapinstall can return the old device list even though the
            // new TAP adapter is already being created.
            for (int attempt = 0; attempt < 40; ++attempt)
            {
                news.clear();
                const bool new_query_ok = TapWindows::FindAllComponentIds(news);
                for (const ppp::string& key : olds)
                {
                    auto tail = news.find(key);
                    if (tail != news.end())
                    {
                        news.erase(tail);
                    }
                }

                if (!news.empty())
                {
                    found_new_component = true;
                    fprintf(stdout, "[TapInstall] after install query attempt=%d ok=%d new_count=%zu\r\n",
                        attempt + 1, new_query_ok ? 1 : 0, news.size());
                    break;
                }

                if (attempt == 0 || attempt == 9 || attempt == 19 || attempt == 39)
                {
                    fprintf(stdout, "[TapInstall] waiting for device enumeration attempt=%d ok=%d total=%zu\r\n",
                        attempt + 1, new_query_ok ? 1 : 0, news.size() + olds.size());
                }
                ::Sleep(250);
            }

            if (!found_new_component || news.empty())
            {
                fprintf(stdout, "[TapInstall] FAIL: no new TAP component after install\r\n");
                return ppp::string();
            }

            for (const ppp::string& newGuid : news)
            {
                ppp::win32::network::NetworkInterfacePtr network_interface;
                bool found_interface = false;
                for (int attempt = 0; attempt < 20; ++attempt)
                {
                    network_interface.reset();
                    TapWindows_FindComponentId(newGuid, network_interface);
                    if (NULLPTR != network_interface)
                    {
                        found_interface = true;
                        break;
                    }
                    ::Sleep(250);
                }

                if (!found_interface)
                {
                    fprintf(stdout, "[TapInstall] component=%s not visible through WMI after install\r\n", newGuid.data());
                    continue;
                }

                for (int attempt = 0; attempt < 20; ++attempt)
                {
                    if (ppp::win32::network::SetInterfaceName(network_interface->InterfaceIndex, declareTapName))
                    {
                        fprintf(stdout, "[TapInstall] SUCCESS component=%s interface_index=%d name='%s'\r\n",
                            newGuid.data(), network_interface->InterfaceIndex, declareTapName.data());
                        return newGuid;
                    }
                    if (attempt == 0 || attempt == 9 || attempt == 19)
                    {
                        fprintf(stdout, "[TapInstall] rename pending component=%s interface_index=%d attempt=%d\r\n",
                            newGuid.data(), network_interface->InterfaceIndex, attempt + 1);
                    }
                    ::Sleep(250);
                }

                // The GUID is already a unique identity for the new device.
                // Keep going with it when only the friendly-name update was
                // rejected; Create() opens the adapter by component ID and
                // does not touch the other TAP/PPP interfaces.
                fprintf(stdout, "[TapInstall] rename failed, continuing with component=%s\r\n", newGuid.data());
                return newGuid;
            }

            fprintf(stdout, "[TapInstall] FAIL: no newly installed component became usable\r\n");
            return ppp::string();
        }

        bool TapWindows::UninstallDriver(const ppp::string& path) noexcept
        {
            if (path.empty())
            {
                return false;
            }

            ppp::string installPath = ppp::io::File::RewritePath((path + "tapinstall.exe").data());
            if (!PathFileExistsA(installPath.data()))
            {
                return false;
            }

            int dwExitCode = INFINITE;
            if (!ppp::win32::Win32Native::Execute(false, installPath.data(), "remove tap0901", &dwExitCode))
            {
                return false;
            }

            return dwExitCode == ERROR_SUCCESS;
        }

        bool TapWindows::SetInterfaceMtu(int mtu) noexcept
        {
            int interface_index = GetInterfaceIndex();
            if (interface_index == -1)
            {
                return false;
            }

            return ppp::win32::network::SetInterfaceMtuIpSubInterface(interface_index, mtu);
        }

        void TapWindows::Dispose() noexcept
        {
            if (wintun_) // only use Wintun path if this instance actually owns a WintunAdapter (not TAP fallback)
            {
                void* handle = GetHandle();
                if (NULLPTR != handle)
                {
                    WintunAdapter* wintun = static_cast<WintunAdapter*>(handle);
                    wintun->Stop();
                }

                wintun_.reset();
            }

            ITap::Dispose();
        }
    }
}
