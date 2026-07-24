#include <ppp/net/IPEndPoint.h>
#include <windows/ppp/win32/network/Router.h>

#include <boost/asio.hpp>

typedef ppp::net::IPEndPoint IPEndPoint;

namespace ppp
{
    namespace win32
    {
        namespace network
        {
            template <typename Loop>
            static int Router_DeleteRoute(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, Loop&& loop) noexcept
            {
                if (NULLPTR == table)
                {
                    return -1;
                }

                int events = 0;
                for (DWORD dwNumEntries = 0; dwNumEntries < table->dwNumEntries; dwNumEntries++)
                {
                    MIB_IPFORWARDROW& route = table->table[dwNumEntries];
                    if (loop(route))
                    {
                        if (Router::Delete(route))
                        {
                            events++;
                        }
                    }
                }

                return events;
            }

            bool Router::GetBestRoute(uint32_t destination, uint32_t source, MIB_IPFORWARDROW& route) noexcept
            {
                int err = ::GetBestRoute(destination, source, &route);
                return err == NO_ERROR;
            }

            bool Router::GetBestRoute(uint32_t destination, MIB_IPFORWARDROW& route) noexcept
            {
                return GetBestRoute(destination, 0, route);
            }

            int Router::GetBestInterface(uint32_t ip) noexcept
            {
                DWORD dwBestIfIndex = 0;
                int err = ::GetBestInterface(ip, &dwBestIfIndex);
                if (err != NO_ERROR)
                {
                    return -1;
                }

                return dwBestIfIndex;
            }

            int Router::Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, uint32_t mask, uint32_t gw, int interface_index) noexcept
            {
                return Router_DeleteRoute(table,
                    [&](MIB_IPFORWARDROW& route) noexcept
                    {
                        return route.dwForwardDest == destination && route.dwForwardMask == mask && route.dwForwardNextHop == gw && (int)route.dwForwardIfIndex == interface_index;
                    });
            }

            int Router::Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, uint32_t mask, uint32_t gw) noexcept
            {
                return Router_DeleteRoute(table,
                    [&](MIB_IPFORWARDROW& route) noexcept
                    {
                        return route.dwForwardDest == destination && route.dwForwardMask == mask && route.dwForwardNextHop == gw;
                    });
            }

            int Router::Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, uint32_t gw) noexcept
            {
                return Router_DeleteRoute(table,
                    [&](MIB_IPFORWARDROW& route) noexcept
                    {
                        return route.dwForwardDest == destination && route.dwForwardNextHop == gw;
                    });
            }

            int Router::Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination) noexcept
            {
                return Router_DeleteRoute(table,
                    [&](MIB_IPFORWARDROW& route) noexcept
                    {
                        return route.dwForwardDest == destination;
                    });
            }

            int Router::Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, int interface_index) noexcept
            {
                return Router_DeleteRoute(table,
                    [&](MIB_IPFORWARDROW& route) noexcept
                    {
                        return route.dwForwardDest == destination && (int)route.dwForwardIfIndex == interface_index;
                    });
            }

            bool Router::Delete(MIB_IPFORWARDROW& route) noexcept
            {
                int err = ::DeleteIpForwardEntry(&route);
                return err == NO_ERROR;
            }

            std::shared_ptr<MIB_IPFORWARDTABLE> Router::GetIpForwardTable() noexcept
            {
                PMIB_IPFORWARDTABLE pRouteTable = NULLPTR;
                DWORD dwSize = 0;
                DWORD dwErr = ::GetIpForwardTable(pRouteTable, &dwSize, true);
                if (dwErr == ERROR_INSUFFICIENT_BUFFER)
                {
                    pRouteTable = (PMIB_IPFORWARDTABLE)Malloc(dwSize);
                    if (NULLPTR == pRouteTable)
                    {
                        return NULLPTR;
                    }

                    dwErr = ::GetIpForwardTable(pRouteTable, &dwSize, true);
                }

                std::shared_ptr<MIB_IPFORWARDTABLE> pRouteTablePtr(pRouteTable,
                    [](MIB_IPFORWARDTABLE* p) noexcept
                    {
                        Mfree(p);
                    });
                if (dwErr != ERROR_SUCCESS)
                {
                    pRouteTablePtr.reset();
                }

                return pRouteTablePtr;
            }

            bool Router::Add(uint32_t destination, uint32_t gw, int metric) noexcept
            {
                return Add(destination, IPEndPoint::NoneAddress, gw, metric);
            }

            bool Router::Add(uint32_t destination, uint32_t mask, uint32_t gw, int metric) noexcept
            {
                int interface_index = GetBestInterface(gw);
                return Add(destination, mask, gw, metric, interface_index);
            }

            bool Router::Add(uint32_t destination, uint32_t mask, uint32_t gw, int metric, int interface_index) noexcept
            {
                if (interface_index < 0)
                {
                    return false;
                }

                if (metric < 1)
                {
                    metric = 1;
                }

                MIB_IPFORWARDROW route;
                memset(&route, 0, sizeof(MIB_IPFORWARDROW));

                route.dwForwardDest = destination;
                route.dwForwardMask = mask;
                route.dwForwardPolicy = 0;
                route.dwForwardNextHop = gw;
                route.dwForwardIfIndex = interface_index;
                route.dwForwardType = MIB_IPROUTE_TYPE_DIRECT;
                route.dwForwardProto = MIB_IPPROTO_NETMGMT;
                route.dwForwardAge = 0;
                route.dwForwardNextHopAS = 0;
                route.dwForwardMetric1 = metric;
                route.dwForwardMetric2 = -1;
                route.dwForwardMetric3 = -1;
                route.dwForwardMetric4 = -1;
                route.dwForwardMetric5 = -1;

                MIB_IPINTERFACE_ROW mib{};
                mib.Family = AF_INET;
                mib.InterfaceIndex = interface_index;

                int err = ::GetIpInterfaceEntry(&mib);
                if (err != NO_ERROR)
                {
                    return false;
                }
                elif((int64_t)metric < (int64_t)mib.Metric)
                {
                    route.dwForwardMetric1 = mib.Metric;
                }

                return Router::Add(route);
            }

            bool Router::Add(MIB_IPFORWARDROW& route) noexcept
            {
                int err = ::CreateIpForwardEntry(&route);
                return err == NO_ERROR;
            }

            bool Router::AddIPv6RouteEntry(const boost::asio::ip::address_v6& network, int prefix_length, const boost::asio::ip::address_v6& next_hop, int interface_index, bool* created) noexcept
            {
                if (created) {
                    *created = false;
                }
                if (interface_index < 0)
                {
                    return false;
                }

                if (prefix_length < 0 || prefix_length > 128)
                {
                    return false;
                }

                if (network.is_unspecified() || next_hop.is_unspecified())
                {
                    return false;
                }

                MIB_IPFORWARD_ROW2 row;
                memset(&row, 0, sizeof(row));

                row.InterfaceIndex = interface_index;
                row.Metric = 1;
                row.Protocol = MIB_IPPROTO_NETMGMT;
                row.Origin = NlroManual;
                row.PreferredLifetime = 0xFFFFFFFF;
                row.ValidLifetime = 0xFFFFFFFF;
                row.SitePrefixLength = 0;
                row.DestinationPrefix.PrefixLength = static_cast<UINT8>(prefix_length);

                // Set destination prefix address (::/prefix)
                {
                    auto bytes = network.to_bytes();
                    IN6_ADDR in6;
                    memcpy(&in6, bytes.data(), sizeof(in6));
                    row.DestinationPrefix.Prefix.Ipv6.sin6_family = AF_INET6;
                    row.DestinationPrefix.Prefix.Ipv6.sin6_addr = in6;
                }

                // Set next hop address
                {
                    auto bytes = next_hop.to_bytes();
                    IN6_ADDR in6;
                    memcpy(&in6, bytes.data(), sizeof(in6));
                    row.NextHop.Ipv6.sin6_family = AF_INET6;
                    row.NextHop.Ipv6.sin6_addr = in6;

                    // Link-local (fe80::) next-hop addresses MUST have sin6_scope_id
                    // set to the interface index, otherwise CreateIpForwardEntry2
                    // fails with ERROR_INVALID_PARAMETER.
                    if (next_hop.is_link_local()) {
                        unsigned long scope_id = static_cast<unsigned long>(next_hop.scope_id());
                        if (scope_id == 0) {
                            scope_id = static_cast<unsigned long>(interface_index);
                        }
                        row.NextHop.Ipv6.sin6_scope_id = scope_id;
                    }
                }

                DWORD result = ::CreateIpForwardEntry2(&row);
                if (result != NO_ERROR) {
                    // ERROR_OBJECT_ALREADY_EXISTS (5010) means the route is already
                    // installed — this can happen when retrying after a failed connection
                    // attempt whose cleanup did not remove the IPv6 route. Treat it as
                    // success since the route is already in place.
                    if (result == ERROR_OBJECT_ALREADY_EXISTS) {
                        return true;
                    }

                    LOG_DEBUG("Router::AddIPv6RouteEntry: CreateIpForwardEntry2 failed, result=%lu, ifindex=%d", result, interface_index);

                    if (result == ERROR_NOT_SUPPORTED) {
                        // Fallback to netsh when the API is not supported on this system.
                        // CreateIpForwardEntry2 may return ERROR_NOT_SUPPORTED (50) on
                        // some Windows versions/configurations where the IPv6 forwarding
                        // API is unavailable. netsh interface ipv6 is the traditional way.
                        std::string network_str = network.to_string();
                        std::string next_hop_str = next_hop.to_string();
                        char command[1024];
                        ::snprintf(command, sizeof(command),
                            "netsh interface ipv6 add route %s/%d %d %s > nul 2>&1",
                            network_str.c_str(), prefix_length, interface_index, next_hop_str.c_str());
                        int rc = ::system(command);
                        if (rc == 0) {
                            if (created) {
                                *created = true;
                            }
                            return true;
                        }
                        LOG_DEBUG("Router::AddIPv6RouteEntry: netsh fallback also failed, rc=%d", rc);
                    }
                }
                if (result == NO_ERROR && created) {
                    *created = true;
                }
                return result == NO_ERROR;
            }

            int Router::DeleteIPv6RouteEntries(const ppp::vector<std::pair<boost::asio::ip::address_v6, int>>& routes, int interface_index) noexcept
            {
                if (interface_index < 0 || routes.empty())
                {
                    return 0;
                }

                PMIB_IPFORWARD_TABLE2 table = NULLPTR;
                DWORD result = ::GetIpForwardTable2(AF_INET6, &table);
                if (result != NO_ERROR || NULLPTR == table)
                {
                    LOG_DEBUG("Router::DeleteIPv6RouteEntries: GetIpForwardTable2 failed, result=%lu, ifindex=%d",
                        result, interface_index);
                    return -1;
                }

                int deleted = 0;
                for (ULONG i = 0; i < table->NumEntries; ++i)
                {
                    MIB_IPFORWARD_ROW2& row = table->Table[i];
                    if (row.InterfaceIndex != static_cast<NET_IFINDEX>(interface_index) ||
                        row.DestinationPrefix.Prefix.si_family != AF_INET6)
                    {
                        continue;
                    }

                    const int prefix_length = static_cast<int>(row.DestinationPrefix.PrefixLength);
                    const IN6_ADDR& destination = row.DestinationPrefix.Prefix.Ipv6.sin6_addr;
                    bool matched = false;
                    for (const auto& route : routes)
                    {
                        if (route.second != prefix_length)
                        {
                            continue;
                        }
                        const auto bytes = route.first.to_bytes();
                        if (::memcmp(&destination, bytes.data(), sizeof(destination)) == 0)
                        {
                            matched = true;
                            break;
                        }
                    }
                    if (matched && ::DeleteIpForwardEntry2(&row) == NO_ERROR)
                    {
                        ++deleted;
                    }
                }

                ::FreeMibTable(table);
                return deleted;
            }

            int Router::CaptureAndDeleteIPv6DefaultRoutes(int interface_index, ppp::vector<MIB_IPFORWARD_ROW2>& routes) noexcept
            {
                if (interface_index < 0)
                {
                    return -1;
                }

                PMIB_IPFORWARD_TABLE2 table = NULLPTR;
                DWORD result = ::GetIpForwardTable2(AF_INET6, &table);
                if (result != NO_ERROR || NULLPTR == table)
                {
                    return -1;
                }

                int deleted = 0;
                for (ULONG i = 0; i < table->NumEntries; ++i)
                {
                    MIB_IPFORWARD_ROW2& row = table->Table[i];
                    if (row.InterfaceIndex != static_cast<NET_IFINDEX>(interface_index) ||
                        row.DestinationPrefix.Prefix.si_family != AF_INET6 ||
                        row.DestinationPrefix.PrefixLength != 0 ||
                        !IN6_IS_ADDR_UNSPECIFIED(&row.DestinationPrefix.Prefix.Ipv6.sin6_addr))
                    {
                        continue;
                    }

                    bool captured = false;
                    for (const MIB_IPFORWARD_ROW2& saved : routes)
                    {
                        if (saved.InterfaceLuid.Value == row.InterfaceLuid.Value &&
                            saved.NextHop.si_family == row.NextHop.si_family &&
                            ::memcmp(&saved.NextHop.Ipv6, &row.NextHop.Ipv6, sizeof(row.NextHop.Ipv6)) == 0)
                        {
                            captured = true;
                            break;
                        }
                    }
                    if (!captured)
                    {
                        routes.emplace_back(row);
                    }

                    result = ::DeleteIpForwardEntry2(&row);
                    if (result == NO_ERROR || result == ERROR_NOT_FOUND)
                    {
                        ++deleted;
                    }
                    else
                    {
                        LOG_ERROR("Router::CaptureAndDeleteIPv6DefaultRoutes: delete failed, result=%lu, ifindex=%d",
                            result, interface_index);
                    }
                }

                ::FreeMibTable(table);
                return deleted;
            }

            int Router::RestoreIPv6Routes(const ppp::vector<MIB_IPFORWARD_ROW2>& routes) noexcept
            {
                int restored = 0;
                for (const MIB_IPFORWARD_ROW2& saved : routes)
                {
                    MIB_IPFORWARD_ROW2 row = saved;
                    DWORD result = ::CreateIpForwardEntry2(&row);
                    if (result == NO_ERROR || result == ERROR_OBJECT_ALREADY_EXISTS)
                    {
                        ++restored;
                    }
                    else
                    {
                        LOG_ERROR("Router::RestoreIPv6Routes: restore failed, result=%lu, ifindex=%u",
                            result, static_cast<unsigned int>(row.InterfaceIndex));
                    }
                }
                return restored;
            }

            bool Router::GetIPv6IgnoreDefaultRoutes(int interface_index, bool& value) noexcept
            {
                value = false;
                if (interface_index < 0)
                {
                    return false;
                }

                MIB_IPINTERFACE_ROW row;
                ::InitializeIpInterfaceEntry(&row);
                row.Family = AF_INET6;
                row.InterfaceIndex = static_cast<NET_IFINDEX>(interface_index);
                DWORD result = ::GetIpInterfaceEntry(&row);
                if (result != NO_ERROR)
                {
                    return false;
                }
                value = row.DisableDefaultRoutes != FALSE;
                return true;
            }

            bool Router::SetIPv6IgnoreDefaultRoutes(int interface_index, bool value) noexcept
            {
                if (interface_index < 0)
                {
                    return false;
                }

                MIB_IPINTERFACE_ROW row;
                ::InitializeIpInterfaceEntry(&row);
                row.Family = AF_INET6;
                row.InterfaceIndex = static_cast<NET_IFINDEX>(interface_index);
                DWORD result = ::GetIpInterfaceEntry(&row);
                if (result != NO_ERROR)
                {
                    return false;
                }
                row.DisableDefaultRoutes = value ? TRUE : FALSE;
                result = ::SetIpInterfaceEntry(&row);
                if (result != NO_ERROR)
                {
                    LOG_ERROR("Router::SetIPv6IgnoreDefaultRoutes: failed, result=%lu, ifindex=%d, value=%d",
                        result, interface_index, static_cast<int>(value));
                    return false;
                }
                return true;
            }
        }
    }
}
