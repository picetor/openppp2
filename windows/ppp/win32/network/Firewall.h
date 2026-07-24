#pragma once

#include <ppp/stdafx.h>

namespace ppp
{
    namespace win32
    {
        namespace network
        {
            class Fw
            {
            public:
                typedef enum
                {
                    NetFirewallType_DomainNetwork,
                    NetFirewallType_PrivateNetwork,
                    NetFirewallType_PublicNetwork,
                } NetFirewallType;

            public:
                static bool NetFirewallAddApplication(const char* name, const char* executablePath, NetFirewallType netFwType) noexcept;
                static bool NetFirewallAddApplication(const char* name, const char* executablePath) noexcept;
                static bool NetFirewallAddAllApplication(const char* name, const char* executablePath) noexcept;
                static bool SetIPv6LeakBlock(const char* rule_name, const char* interface_name, bool enabled) noexcept;
                static bool AddIPv6LeakBlockWfp(int interface_index, HANDLE& engine_handle) noexcept;
                static void RemoveIPv6LeakBlockWfp(HANDLE& engine_handle) noexcept;
            };
        }
    }
}
