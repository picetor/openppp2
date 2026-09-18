#pragma once

#include <ppp/stdafx.h>
#include <ppp/net/IPEndPoint.h>

namespace ppp
{
    namespace net
    {
        namespace native
        {
            enum class RouteOrigin : uint8_t
            {
                Unknown = 0,
                TunnelDefault,
                TunnelPolicy,
                PhysicalSystem,
                Bypass,
                GeoDirect,
                ServerPin
            };

            enum class RouteAction : uint8_t
            {
                Unspecified = 0,
                Tunnel,
                Direct,
                Reject
            };

            typedef struct RouteEntry
            {
                uint32_t                                                Destination;
                int                                                     Prefix;
                uint32_t                                                NextHop;
                RouteOrigin                                             Origin = RouteOrigin::Unknown;
                RouteAction                                             Action = RouteAction::Unspecified;
            }                                                           RouteEntry;

            typedef ppp::vector<RouteEntry>                             RouteEntries;
            typedef ppp::unordered_map<uint32_t, RouteEntries>          RouteEntriesTable;

            static constexpr int                                        MIN_PREFIX_VALUE    = 0;
            static constexpr int                                        MAX_PREFIX_VALUE    = 32;
            static constexpr int                                        MAX_PREFIX_VALUE_V4 = MAX_PREFIX_VALUE;
            static constexpr int                                        MAX_PREFIX_VALUE_V6 = 128;

            // RIB
            class RouteInformationTable
            {
            public:
                bool                                                    AddRoute(uint32_t ip, int prefix, uint32_t gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                    AddRoute(const ppp::string& cidr, uint32_t gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                    AddAllRoutes(const ppp::string& cidrs, uint32_t gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                    AddAllRoutesByIPList(const ppp::string& path, uint32_t gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                    IsAvailable() noexcept { return routes.begin() != routes.end(); }

            public:
                bool                                                    DeleteRoute(uint32_t ip) noexcept;
                bool                                                    DeleteRoute(uint32_t ip, uint32_t gw) noexcept;
                bool                                                    DeleteRoute(uint32_t ip, int prefix, uint32_t gw) noexcept;

            public:
                RouteEntriesTable&                                      GetAllRoutes() noexcept;
                void                                                    Clear() noexcept;

            private:
                RouteEntriesTable                                       routes;
            };

            // RouteEntry6: IPv6 route entry storing addresses as boost::asio::ip::address
            typedef struct RouteEntry6
            {
                boost::asio::ip::address                                   Destination;
                int                                                        Prefix;
                boost::asio::ip::address                                   NextHop;
                RouteOrigin                                                Origin = RouteOrigin::Unknown;
                RouteAction                                                Action = RouteAction::Unspecified;
            }                                                              RouteEntry6;

            typedef ppp::vector<RouteEntry6>                               RouteEntries6;

            // RIB6 for IPv6 route-level splitting
            // No FIB needed — OS routing table handles v6 longest-prefix-match
            class RouteInformationTable6
            {
            public:
                bool                                                       AddRoute(const boost::asio::ip::address& ip, int prefix, const boost::asio::ip::address& gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                       AddRoute(const ppp::string& cidr, const boost::asio::ip::address& gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                       AddAllRoutesByIPList(const ppp::string& path, const boost::asio::ip::address& gw, RouteOrigin origin = RouteOrigin::Unknown, RouteAction action = RouteAction::Unspecified) noexcept;
                bool                                                       IsAvailable() noexcept { return routes.begin() != routes.end(); }

            public:
                RouteEntries6&                                             GetAllRoutes() noexcept { return routes; }
                void                                                       Clear() noexcept { routes.clear(); }

            private:
                RouteEntries6                                              routes;
            };

            // FIB
            class ForwardInformationTable
            {
            public:
                ForwardInformationTable() noexcept = default;
                ForwardInformationTable(RouteInformationTable& rib) noexcept;

            public:
                uint32_t                                                GetNextHop(uint32_t ip) noexcept;
                static uint32_t                                         GetNextHop(uint32_t ip, RouteEntriesTable& routes) noexcept;
                static uint32_t                                         GetNextHop(uint32_t ip, int min_prefix_value, int max_prefix_value, RouteEntriesTable& routes) noexcept;
                static bool                                             TryGetBestRoute(uint32_t ip, RouteEntriesTable& routes, RouteEntry& route) noexcept;
                static bool                                             TryGetBestRoute(uint32_t ip, int min_prefix_value, int max_prefix_value, RouteEntriesTable& routes, RouteEntry& route) noexcept;
                void                                                    Fill(RouteInformationTable& rib) noexcept;
                void                                                    Clear() noexcept;
                RouteEntriesTable&                                      GetAllRoutes() noexcept;
                bool                                                    IsAvailable() noexcept { return routes.begin() != routes.end(); }

            private:
                RouteEntriesTable                                       routes;
            };

            // FIB6 for IPv6 — mirrors ForwardInformationTable but for 128-bit addresses.
            class ForwardInformationTable6
            {
            public:
                ForwardInformationTable6() noexcept = default;
                ForwardInformationTable6(RouteInformationTable6& rib) noexcept;

            public:
                boost::asio::ip::address                                GetNextHop(const boost::asio::ip::address& ip) noexcept;
                static boost::asio::ip::address                         GetNextHop(const boost::asio::ip::address& ip, RouteEntries6& routes) noexcept;
                void                                                    Fill(RouteInformationTable6& rib) noexcept;
                void                                                    Clear() noexcept;
                RouteEntries6&                                          GetAllRoutes() noexcept;
                bool                                                    IsAvailable() noexcept { return routes.begin() != routes.end(); }

            private:
                RouteEntries6                                           routes;
            };
        }
    }
}
