#pragma once

#include <ppp/stdafx.h>

#include <Windows.h>
#include <Iphlpapi.h>
#include <netioapi.h>

namespace ppp
{
    namespace win32
    {
        namespace network
        {
            class Router final
            {
            public:
                static int                                  GetBestInterface(uint32_t ip) noexcept;
                static bool                                 GetBestRoute(uint32_t destination, MIB_IPFORWARDROW& route) noexcept;
                static bool                                 GetBestRoute(uint32_t destination, uint32_t source, MIB_IPFORWARDROW& route) noexcept;
                static std::shared_ptr<MIB_IPFORWARDTABLE>  GetIpForwardTable() noexcept;

            public:
                static int                                  Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, uint32_t mask, uint32_t gw, int interface_index) noexcept;
                static int                                  Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, uint32_t mask, uint32_t gw) noexcept;
                static int                                  Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, uint32_t gw) noexcept;
                static int                                  Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination) noexcept;
                static int                                  Delete(const std::shared_ptr<MIB_IPFORWARDTABLE>& table, uint32_t destination, int interface_index) noexcept;
                static bool                                 Delete(MIB_IPFORWARDROW& route) noexcept;

            public:
                static bool                                 Add(uint32_t destination, uint32_t gw, int metric) noexcept;
                static bool                                 Add(uint32_t destination, uint32_t mask, uint32_t gw, int metric) noexcept;
                static bool                                 Add(uint32_t destination, uint32_t mask, uint32_t gw, int metric, int interface_index) noexcept;
                static bool                                 Add(uint32_t destination, uint32_t mask, uint32_t gw, int metric, int interface_index, DWORD* error) noexcept;
                static bool                                 Add(MIB_IPFORWARDROW& route) noexcept;

            public:
                static bool                                 AddIPv6RouteEntry(const boost::asio::ip::address_v6& network, int prefix_length, const boost::asio::ip::address_v6& next_hop, int interface_index, bool* created = NULLPTR) noexcept;
                static int                                  DeleteIPv6RouteEntries(const ppp::vector<std::pair<boost::asio::ip::address_v6, int>>& routes, int interface_index) noexcept;
                static int                                  CaptureAndDeleteIPv6DefaultRoutes(int interface_index, ppp::vector<MIB_IPFORWARD_ROW2>& routes) noexcept;
                static int                                  RestoreIPv6Routes(const ppp::vector<MIB_IPFORWARD_ROW2>& routes) noexcept;
                static bool                                 GetIPv6IgnoreDefaultRoutes(int interface_index, bool& value) noexcept;
                static bool                                 SetIPv6IgnoreDefaultRoutes(int interface_index, bool value) noexcept;
                static bool                                 GetIPv6Forwarding(int interface_index, bool& value) noexcept;
                static bool                                 SetIPv6Forwarding(int interface_index, bool value) noexcept;
            };
        }
    }
}
