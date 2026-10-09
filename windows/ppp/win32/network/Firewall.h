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
                // Allow the replies that the userspace tunnel driver injects into
                // the VPN adapter (DNS answers, ICMP echo replies, tunneled UDP).
                // Those packets are attributed to no process, so the
                // program-scoped rules above cannot match them.
                //
                // Scope of this guard: it only helps when the drop really is
                // Windows Firewall policy. Measured on Windows, the injected
                // packets had valid IP/UDP checksums and reached the adapter, yet
                // the host stack discarded them at FWPM_LAYER_INBOUND_TRANSPORT_V4
                // while the ALE layer stayed flat - i.e. not a firewall policy
                // decision - and adding these rules changed nothing. Use the
                // client's "TUN injection probe" log line and
                // tests/tools/TunInboundProbe.ps1 to tell the two cases apart.
                //
                // local_addresses is the authoritative inbound scope and must be
                // the tunnel subnet ("192.168.14.0/24"); it is required, because
                // INetFwRule::Interfaces is documented for outbound rules only
                // and interface_name below is therefore best-effort.
                //
                // Creates/replaces "<rule_name> UDP" and "<rule_name> ICMPv4";
                // enabled = false removes them and needs no other argument.
                static bool AllowTunnelInbound(const char* rule_name, const char* interface_name,
                    const char* local_addresses, bool enabled) noexcept;
                static bool AddIPv6LeakBlockWfp(int interface_index, HANDLE& engine_handle) noexcept;
                static void RemoveIPv6LeakBlockWfp(HANDLE& engine_handle) noexcept;
            };
        }
    }
}
