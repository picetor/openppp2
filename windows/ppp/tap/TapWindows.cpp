#include <windows/ppp/tap/TapWindows.h>
#include <windows/ppp/win32/Win32Native.h>
#include <windows/ppp/win32/network/NetworkInterface.h>
#include <windows/ppp/win32/network/Router.h>
#include <windows/ppp/tap/tap-windows.h>

#include <ppp/io/File.h>
#include <ppp/ipv6/IPv6Packet.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/native/checksum.h>
#include <ppp/net/native/icmp.h>
#include <ppp/net/native/ip.h>
#include <ppp/net/native/tcp.h>
#include <ppp/net/native/udp.h>
#include <ppp/text/Encoding.h>
#include <ppp/threading/Executors.h>

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
        static std::atomic<bool> TAP_WINDOWS_DATAPLANE_TRACE(false);

        struct TapWindowsInputTrace final
        {
            TapWindows* owner = NULLPTR;
            const void* packet = NULLPTR;
            int packet_size = 0;
            uint64_t flow_id = 0;
            uint64_t started_at = 0;
        };

        static thread_local TapWindowsInputTrace TAP_WINDOWS_INPUT_TRACE;

        struct WintunIpv6TransportView final
        {
            const Byte* payload = NULLPTR;
            int payload_length = 0;
            int offset = 0;
            Byte protocol = 0;
            bool checksum_verifiable = false;
            unsigned int fragment_flags = 0;
        };

        static bool LocateWintunIpv6Transport(const Byte* packet, int packet_size,
            WintunIpv6TransportView& view) noexcept
        {
            if (NULLPTR == packet || packet_size < 40 || (packet[0] >> 4) != 6)
            {
                return false;
            }

            int cursor = 40;
            Byte next_header = packet[6];
            int extension_count = 0;
            bool fragmented = false;
            bool fragment_seen = false;
            unsigned int fragment_flags = 0;
            for (;;)
            {
                if (next_header == 0 || next_header == 43 || next_header == 60 || next_header == 51)
                {
                    if (++extension_count > 8 || cursor + 2 > packet_size)
                    {
                        return false;
                    }
                    const int extension_length = next_header == 51 ?
                        (static_cast<int>(packet[cursor + 1]) + 2) * 4 :
                        (static_cast<int>(packet[cursor + 1]) + 1) * 8;
                    if (extension_length < 8 || cursor + extension_length > packet_size)
                    {
                        return false;
                    }
                    next_header = packet[cursor];
                    cursor += extension_length;
                    continue;
                }
                if (next_header == 44)
                {
                    if (fragment_seen || ++extension_count > 8 || cursor + 8 > packet_size)
                    {
                        return false;
                    }
                    fragment_seen = true;
                    fragment_flags =
                        (static_cast<unsigned int>(packet[cursor + 2]) << 8) |
                        static_cast<unsigned int>(packet[cursor + 3]);
                    if ((fragment_flags & 0x0006U) != 0)
                    {
                        return false;
                    }
                    fragmented = (fragment_flags & 0xfff9U) != 0;
                    next_header = packet[cursor];
                    cursor += 8;
                    continue;
                }
                break;
            }

            if (cursor > packet_size)
            {
                return false;
            }
            view.payload = packet + cursor;
            view.payload_length = packet_size - cursor;
            view.offset = cursor;
            view.protocol = next_header;
            view.checksum_verifiable = !fragmented &&
                (next_header == IPPROTO_UDP || next_header == IPPROTO_TCP || next_header == IPPROTO_ICMPV6);
            view.fragment_flags = fragment_flags;
            return true;
        }

        static ppp::string BuildWintunCorrelationKey(const void* packet, int packet_size) noexcept
        {
            if (NULLPTR == packet || packet_size < 1)
            {
                return ppp::string();
            }
            const Byte* bytes = static_cast<const Byte*>(packet);
            const int version = bytes[0] >> 4;
            try
            {
                if (version == 4 && packet_size >= ppp::net::native::ip_hdr::IP_HLEN)
                {
                    const ppp::net::native::ip_hdr* ip =
                        reinterpret_cast<const ppp::net::native::ip_hdr*>(packet);
                    const int header_length = (ip->v_hl & 0x0f) << 2;
                    if (header_length < ppp::net::native::ip_hdr::IP_HLEN ||
                        header_length > packet_size || ntohs(ip->len) != packet_size ||
                        (ntohs(ip->flags) & (ppp::net::native::ip_hdr::IP_MF |
                            ppp::net::native::ip_hdr::IP_OFFMASK)) != 0)
                    {
                        return ppp::string();
                    }
                    const Byte* transport = bytes + header_length;
                    const int transport_length = packet_size - header_length;
                    uint32_t source = ip->src;
                    uint32_t destination = ip->dest;
                    char key[256];
                    if (ip->proto == IPPROTO_ICMP && transport_length >= 8 &&
                        (transport[0] == 0 || transport[0] == 8))
                    {
                        if (transport[0] == 0)
                        {
                            std::swap(source, destination);
                        }
                        uint16_t identifier = 0;
                        uint16_t sequence = 0;
                        memcpy(&identifier, transport + 4, sizeof(identifier));
                        memcpy(&sequence, transport + 6, sizeof(sequence));
                        snprintf(key, sizeof(key), "icmp|4|%u|%u|%u|%u",
                            (unsigned int)source, (unsigned int)destination,
                            (unsigned int)ntohs(identifier), (unsigned int)ntohs(sequence));
                        return key;
                    }
                    if (ip->proto == IPPROTO_UDP && transport_length >= 20)
                    {
                        uint16_t source_port = 0;
                        uint16_t destination_port = 0;
                        uint16_t transaction_id = 0;
                        uint16_t dns_flags = 0;
                        memcpy(&source_port, transport, sizeof(source_port));
                        memcpy(&destination_port, transport + 2, sizeof(destination_port));
                        if (ntohs(source_port) != PPP_DNS_SYS_PORT &&
                            ntohs(destination_port) != PPP_DNS_SYS_PORT)
                        {
                            return ppp::string();
                        }
                        memcpy(&transaction_id, transport + 8, sizeof(transaction_id));
                        memcpy(&dns_flags, transport + 10, sizeof(dns_flags));
                        if ((ntohs(dns_flags) & 0x8000U) != 0)
                        {
                            std::swap(source, destination);
                            std::swap(source_port, destination_port);
                        }
                        snprintf(key, sizeof(key), "dns|4|%u|%u|%u|%u|%u",
                            (unsigned int)source, (unsigned int)ntohs(source_port),
                            (unsigned int)destination, (unsigned int)ntohs(destination_port),
                            (unsigned int)ntohs(transaction_id));
                        return key;
                    }
                    return ppp::string();
                }
                if (version == 6 && packet_size >= 40)
                {
                    uint16_t payload_length = 0;
                    memcpy(&payload_length, bytes + 4, sizeof(payload_length));
                    if (40 + ntohs(payload_length) != packet_size)
                    {
                        return ppp::string();
                    }
                    WintunIpv6TransportView view;
                    if (!LocateWintunIpv6Transport(bytes, packet_size, view) ||
                        !view.checksum_verifiable)
                    {
                        return ppp::string();
                    }
                    boost::asio::ip::address_v6::bytes_type source_bytes;
                    boost::asio::ip::address_v6::bytes_type destination_bytes;
                    memcpy(source_bytes.data(), bytes + 8, source_bytes.size());
                    memcpy(destination_bytes.data(), bytes + 24, destination_bytes.size());
                    boost::asio::ip::address_v6 source(source_bytes);
                    boost::asio::ip::address_v6 destination(destination_bytes);
                    const Byte* transport = view.payload;
                    char key[512];
                    if (view.protocol == IPPROTO_ICMPV6 && view.payload_length >= 8 &&
                        (transport[0] == 128 || transport[0] == 129))
                    {
                        if (transport[0] == 129)
                        {
                            std::swap(source, destination);
                        }
                        uint16_t identifier = 0;
                        uint16_t sequence = 0;
                        memcpy(&identifier, transport + 4, sizeof(identifier));
                        memcpy(&sequence, transport + 6, sizeof(sequence));
                        snprintf(key, sizeof(key), "icmp|6|%s|%s|%u|%u",
                            source.to_string().c_str(), destination.to_string().c_str(),
                            (unsigned int)ntohs(identifier), (unsigned int)ntohs(sequence));
                        return key;
                    }
                    if (view.protocol == IPPROTO_UDP && view.payload_length >= 20)
                    {
                        uint16_t source_port = 0;
                        uint16_t destination_port = 0;
                        uint16_t transaction_id = 0;
                        uint16_t dns_flags = 0;
                        memcpy(&source_port, transport, sizeof(source_port));
                        memcpy(&destination_port, transport + 2, sizeof(destination_port));
                        if (ntohs(source_port) != PPP_DNS_SYS_PORT &&
                            ntohs(destination_port) != PPP_DNS_SYS_PORT)
                        {
                            return ppp::string();
                        }
                        memcpy(&transaction_id, transport + 8, sizeof(transaction_id));
                        memcpy(&dns_flags, transport + 10, sizeof(dns_flags));
                        if ((ntohs(dns_flags) & 0x8000U) != 0)
                        {
                            std::swap(source, destination);
                            std::swap(source_port, destination_port);
                        }
                        snprintf(key, sizeof(key), "dns|6|%s|%u|%s|%u|%u",
                            source.to_string().c_str(), (unsigned int)ntohs(source_port),
                            destination.to_string().c_str(), (unsigned int)ntohs(destination_port),
                            (unsigned int)ntohs(transaction_id));
                        return key;
                    }
                }
            }
            catch (...)
            {
            }
            return ppp::string();
        }

        static void LogWintunTraceStage(const char* stage, uint64_t flow_id,
            const void* packet, int packet_size, int interface_index, int mtu,
            uint64_t started_at, uint64_t stage_count, int error_code = 0,
            const char* route_origin = "remote", const char* route_action = "inject",
            int outbound = 0) noexcept
        {
            if (NULLPTR == stage || NULLPTR == packet || packet_size < 1)
            {
                return;
            }

            const Byte* bytes = static_cast<const Byte*>(packet);
            const int version = bytes[0] >> 4;
            int protocol = 0;
            int ip_header_length = 0;
            int ttl = 0;
            unsigned int flags = 0;
            unsigned int ip_checksum = 0;
            unsigned int transport_checksum = 0;
            unsigned int src_port = 0;
            unsigned int dst_port = 0;
            int dns_transaction_id = -1;
            int icmp_type = -1;
            int icmp_code = -1;
            int icmp_identifier = -1;
            int icmp_sequence = -1;
            bool transport_fields_available = true;
            std::string src_ip = "<invalid>";
            std::string dst_ip = "<invalid>";

            try
            {
                if (version == 4 && packet_size >= ppp::net::native::ip_hdr::IP_HLEN)
                {
                    const ppp::net::native::ip_hdr* ip =
                        reinterpret_cast<const ppp::net::native::ip_hdr*>(packet);
                    protocol = ip->proto;
                    ip_header_length = (ip->v_hl & 0x0f) << 2;
                    ttl = ip->ttl;
                    flags = ntohs(ip->flags);
                    transport_fields_available =
                        (flags & (ppp::net::native::ip_hdr::IP_MF |
                            ppp::net::native::ip_hdr::IP_OFFMASK)) == 0;
                    ip_checksum = ntohs(ip->chksum);
                    src_ip = boost::asio::ip::address_v4(ntohl(ip->src)).to_string();
                    dst_ip = boost::asio::ip::address_v4(ntohl(ip->dest)).to_string();
                }
                elif(version == 6 && packet_size >= 40)
                {
                    boost::asio::ip::address_v6::bytes_type source_bytes;
                    boost::asio::ip::address_v6::bytes_type destination_bytes;
                    memcpy(source_bytes.data(), bytes + 8, source_bytes.size());
                    memcpy(destination_bytes.data(), bytes + 24, destination_bytes.size());
                    WintunIpv6TransportView transport_view;
                    if (LocateWintunIpv6Transport(bytes, packet_size, transport_view))
                    {
                        protocol = transport_view.protocol;
                        ip_header_length = transport_view.offset;
                        flags = transport_view.fragment_flags;
                        transport_fields_available = transport_view.checksum_verifiable;
                    }
                    else
                    {
                        protocol = bytes[6];
                        ip_header_length = 40;
                    }
                    ttl = bytes[7];
                    src_ip = boost::asio::ip::address_v6(source_bytes).to_string();
                    dst_ip = boost::asio::ip::address_v6(destination_bytes).to_string();
                }
            }
            catch (...)
            {
                src_ip = "<invalid>";
                dst_ip = "<invalid>";
            }

            if (transport_fields_available && ip_header_length > 0 &&
                packet_size >= ip_header_length + 4)
            {
                const Byte* transport = bytes + ip_header_length;
                const int transport_length = packet_size - ip_header_length;
                if (protocol == IPPROTO_UDP || protocol == IPPROTO_TCP)
                {
                    uint16_t source = 0;
                    uint16_t destination = 0;
                    memcpy(&source, transport, sizeof(source));
                    memcpy(&destination, transport + 2, sizeof(destination));
                    src_port = ntohs(source);
                    dst_port = ntohs(destination);
                    const int checksum_offset = protocol == IPPROTO_UDP ? 6 : 16;
                    if (transport_length >= checksum_offset + 2)
                    {
                        uint16_t checksum = 0;
                        memcpy(&checksum, transport + checksum_offset, sizeof(checksum));
                        transport_checksum = ntohs(checksum);
                    }
                    if (protocol == IPPROTO_UDP && transport_length >= 10 &&
                        (src_port == PPP_DNS_SYS_PORT || dst_port == PPP_DNS_SYS_PORT))
                    {
                        uint16_t transaction_id = 0;
                        memcpy(&transaction_id, transport + 8, sizeof(transaction_id));
                        dns_transaction_id = ntohs(transaction_id);
                    }
                }
                elif(protocol == IPPROTO_ICMP || protocol == IPPROTO_ICMPV6)
                {
                    icmp_type = transport[0];
                    icmp_code = transport[1];
                    uint16_t checksum = 0;
                    memcpy(&checksum, transport + 2, sizeof(checksum));
                    transport_checksum = ntohs(checksum);
                    const bool echo = protocol == IPPROTO_ICMP ?
                        (icmp_type == 0 || icmp_type == 8) :
                        (icmp_type == 128 || icmp_type == 129);
                    if (echo && transport_length >= 8)
                    {
                        uint16_t identifier = 0;
                        uint16_t sequence = 0;
                        memcpy(&identifier, transport + 4, sizeof(identifier));
                        memcpy(&sequence, transport + 6, sizeof(sequence));
                        icmp_identifier = ntohs(identifier);
                        icmp_sequence = ntohs(sequence);
                    }
                }
            }

            NET_LUID luid = {};
            const DWORD luid_error = interface_index < 0 ? ERROR_INVALID_PARAMETER :
                ::ConvertInterfaceIndexToLuid((NET_IFINDEX)interface_index, &luid);
            const uint64_t elapsed = ppp::threading::Executors::GetTickCount() - started_at;
            LOG_DEBUG("DATAPLANE %s flow_id=%llu protocol=%d address_family=%d src_ip=%s src_port=%u dst_ip=%s dst_port=%u packet_length=%d ip_header_length=%d ttl=%d flags=0x%x ip_checksum=0x%04x transport_checksum=0x%04x dns_transaction_id=%d icmp_type=%d icmp_code=%d icmp_identifier=%d icmp_sequence=%d interface_index=%d interface_luid=%llu luid_error=%lu route_origin=%s route_action=%s outbound=%d error_code=%d elapsed_ms=%llu mtu=%d stage_count=%llu",
                stage, (unsigned long long)flow_id, protocol, version,
                src_ip.c_str(), src_port, dst_ip.c_str(), dst_port,
                packet_size, ip_header_length, ttl, flags, ip_checksum,
                transport_checksum, dns_transaction_id, icmp_type, icmp_code,
                icmp_identifier, icmp_sequence, interface_index,
                luid_error == NO_ERROR ? (unsigned long long)luid.Value : 0ULL,
                (unsigned long)luid_error,
                NULLPTR != route_origin ? route_origin : "unknown",
                NULLPTR != route_action ? route_action : "unknown",
                outbound, error_code, (unsigned long long)elapsed,
                mtu, (unsigned long long)stage_count);
        }

        static bool ValidateWintunInjectionPacket(const void* packet, int packet_size, int mtu,
            uint32_t expected_ipv4, const boost::asio::ip::address& expected_ipv6) noexcept
        {
            if (NULLPTR == packet || packet_size < 1 || packet_size > mtu)
            {
                return false;
            }

            const Byte* bytes = static_cast<const Byte*>(packet);
            const int version = bytes[0] >> 4;
            if (version == 4)
            {
                if (packet_size < ppp::net::native::ip_hdr::IP_HLEN)
                {
                    return false;
                }
                const ppp::net::native::ip_hdr* ip =
                    reinterpret_cast<const ppp::net::native::ip_hdr*>(packet);
                const int header_length = (ip->v_hl & 0x0f) << 2;
                const int total_length = ntohs(ip->len);
                if (header_length < ppp::net::native::ip_hdr::IP_HLEN ||
                    header_length > packet_size || total_length != packet_size ||
                    ppp::net::native::inet_chksum(const_cast<void*>(packet), header_length) != 0 ||
                    expected_ipv4 == IPEndPoint::AnyAddress || ip->dest != expected_ipv4)
                {
                    return false;
                }

                Byte* transport = const_cast<Byte*>(bytes + header_length);
                const int transport_length = packet_size - header_length;
                const unsigned int fragment_flags = ntohs(ip->flags);
                if ((fragment_flags &
                    (ppp::net::native::ip_hdr::IP_MF | ppp::net::native::ip_hdr::IP_OFFMASK)) != 0)
                {
                    // A single fragment cannot validate a checksum covering the
                    // reassembled transport segment. The IP header, length,
                    // destination and MTU checks above still apply.
                    return true;
                }
                if (ip->proto == ppp::net::native::ip_hdr::IP_PROTO_UDP)
                {
                    if (transport_length < (int)sizeof(ppp::net::native::udp_hdr))
                    {
                        return false;
                    }
                    ppp::net::native::udp_hdr* udp =
                        reinterpret_cast<ppp::net::native::udp_hdr*>(transport);
                    return ntohs(udp->len) == transport_length &&
                        (udp->chksum == 0 ||
                            ppp::net::native::inet_chksum_pseudo(transport, IPPROTO_UDP,
                                transport_length, ip->src, ip->dest) == 0);
                }
                if (ip->proto == ppp::net::native::ip_hdr::IP_PROTO_ICMP)
                {
                    return transport_length >= (int)sizeof(ppp::net::native::icmp_hdr) &&
                        ppp::net::native::inet_chksum(transport, transport_length) == 0;
                }
                if (ip->proto == ppp::net::native::ip_hdr::IP_PROTO_TCP)
                {
                    return transport_length >= ppp::net::native::tcp_hdr::TCP_HLEN &&
                        ppp::net::native::inet_chksum_pseudo(transport, IPPROTO_TCP,
                            transport_length, ip->src, ip->dest) == 0;
                }
                return true;
            }
            if (version == 6)
            {
                if (packet_size < 40)
                {
                    return false;
                }
                uint16_t payload_length = 0;
                memcpy(&payload_length, bytes + 4, sizeof(payload_length));
                if (40 + ntohs(payload_length) != packet_size ||
                    !expected_ipv6.is_v6() || expected_ipv6.is_unspecified())
                {
                    return false;
                }
                const boost::asio::ip::address_v6::bytes_type expected =
                    expected_ipv6.to_v6().to_bytes();
                if (0 != memcmp(bytes + 24, expected.data(), expected.size()))
                {
                    return false;
                }

                WintunIpv6TransportView transport_view;
                if (!LocateWintunIpv6Transport(bytes, packet_size, transport_view))
                {
                    return false;
                }
                const int transport_length = transport_view.payload_length;
                const Byte next_header = transport_view.protocol;
                if (next_header != IPPROTO_UDP && next_header != IPPROTO_TCP &&
                    next_header != IPPROTO_ICMPV6)
                {
                    // Unknown upper-layer protocols are structurally valid but
                    // do not have a checksum rule in this validator.
                    return true;
                }

                if (!transport_view.checksum_verifiable)
                {
                    // A fragmented upper-layer packet requires reassembly before
                    // its checksum can be checked. The extension chain and base
                    // IPv6 invariants have already been validated.
                    return true;
                }
                const int minimum = next_header == IPPROTO_UDP ? 8 :
                    (next_header == IPPROTO_TCP ? 20 : 4);
                if (transport_length < minimum)
                {
                    return false;
                }
                if (next_header == IPPROTO_UDP)
                {
                    uint16_t udp_length = 0;
                    uint16_t udp_checksum = 0;
                    memcpy(&udp_length, transport_view.payload + 4, sizeof(udp_length));
                    memcpy(&udp_checksum, transport_view.payload + 6, sizeof(udp_checksum));
                    if (ntohs(udp_length) != transport_length || udp_checksum == 0)
                    {
                        return false;
                    }
                }

                boost::asio::ip::address_v6::bytes_type source_bytes;
                memcpy(source_bytes.data(), bytes + 8, source_bytes.size());
                const boost::asio::ip::address_v6 source(source_bytes);
                const boost::asio::ip::address_v6 destination(expected);
                return ppp::ipv6::ComputePseudoChecksum(
                    const_cast<Byte*>(transport_view.payload), static_cast<unsigned int>(transport_length),
                    source, destination, next_header) == 0;
            }
            return false;
        }

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
                const uint64_t flow_id = wintun_flow_id_.fetch_add(1, std::memory_order_relaxed) + 1;
                const uint64_t trace_started = ppp::threading::Executors::GetTickCount();
                return OutputWintun(packet, packet_size, flow_id, trace_started);
            }

            return ITap::Output(packet, packet_size);
        }

        bool TapWindows::OutputWithTrace(const void* packet, int packet_size, const char* stage) noexcept
        {
            if (!wintun_)
            {
                return Output(packet, packet_size);
            }

            const uint64_t now = ppp::threading::Executors::GetTickCount();
            TraceContext context;
            const bool trace_enabled = GetDataplaneTrace();
            bool correlated = false;
            if (trace_enabled)
            {
                const ppp::string correlation_key = BuildWintunCorrelationKey(packet, packet_size);
                correlated = !correlation_key.empty() && TakeWintunTrace(correlation_key, context);
            }
            const uint64_t flow_id = correlated ? context.flow_id :
                wintun_flow_id_.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64_t trace_started = correlated ? context.started_at : now;
            const bool remote_rx = NULLPTR != stage && 0 == strcmp(stage, "REMOTE_RX");
            const bool local_rx = NULLPTR != stage && 0 == strcmp(stage, "LOCAL_RX");
            const uint64_t rx_count = remote_rx ?
                wintun_remote_rx_packets_.fetch_add(1, std::memory_order_relaxed) + 1 :
                (local_rx ? wintun_local_rx_packets_.fetch_add(1, std::memory_order_relaxed) + 1 : 0);
            if (correlated && remote_rx)
            {
                wintun_correlated_remote_rx_packets_.fetch_add(1, std::memory_order_relaxed);
            }
            if (trace_enabled && NULLPTR != stage && *stage != '\0')
            {
                LogWintunTraceStage(stage, flow_id, packet, packet_size,
                    GetInterfaceIndex(), interface_mtu_.load(std::memory_order_relaxed),
                    trace_started, rx_count);
            }
            return OutputWintun(packet, packet_size, flow_id, trace_started);
        }

        bool TapWindows::TraceInputStage(const char* stage, const char* route_origin,
            const char* route_action, int outbound, int error_code) noexcept
        {
            if (!wintun_ || NULLPTR == stage || *stage == '\0')
            {
                return false;
            }

            uint64_t count = 0;
            if (0 == strcmp(stage, "POLICY_SELECTED"))
            {
                count = wintun_policy_selected_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            elif(0 == strcmp(stage, "REMOTE_TX"))
            {
                count = wintun_remote_tx_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            else
            {
                return false;
            }

            const TapWindowsInputTrace& trace = TAP_WINDOWS_INPUT_TRACE;
            if (GetDataplaneTrace() && trace.owner == this && trace.flow_id != 0 &&
                NULLPTR != trace.packet && trace.packet_size > 0)
            {
                LogWintunTraceStage(stage, trace.flow_id, trace.packet, trace.packet_size,
                    GetInterfaceIndex(), interface_mtu_.load(std::memory_order_relaxed),
                    trace.started_at, count, error_code, route_origin, route_action, outbound);
            }
            return true;
        }

        bool TapWindows::TracePacketStage(const void* packet, int packet_size,
            const char* stage, const char* route_origin, const char* route_action,
            int outbound, int error_code) noexcept
        {
            if (!wintun_ || NULLPTR == packet || packet_size < 1 ||
                NULLPTR == stage || *stage == '\0')
            {
                return false;
            }

            uint64_t count = 0;
            if (0 == strcmp(stage, "POLICY_SELECTED"))
            {
                count = wintun_policy_selected_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            elif(0 == strcmp(stage, "REMOTE_TX"))
            {
                count = wintun_remote_tx_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            else
            {
                return false;
            }

            if (!GetDataplaneTrace())
            {
                return true;
            }
            const ppp::string key = BuildWintunCorrelationKey(packet, packet_size);
            TraceContext context;
            if (key.empty() || !PeekWintunTrace(key, context))
            {
                return false;
            }
            LogWintunTraceStage(stage, context.flow_id, packet, packet_size,
                GetInterfaceIndex(), interface_mtu_.load(std::memory_order_relaxed),
                context.started_at, count, error_code, route_origin, route_action, outbound);
            return true;
        }

        void TapWindows::OnInput(PacketInputEventArgs& e) noexcept
        {
            TapWindowsInputTrace previous_trace = TAP_WINDOWS_INPUT_TRACE;
            bool trace_active = false;
            if (wintun_)
            {
                const uint64_t count = wintun_tun_rx_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (GetDataplaneTrace() && NULLPTR != e.Packet && e.PacketLength > 0)
                {
                    const uint64_t flow_id = wintun_flow_id_.fetch_add(1, std::memory_order_relaxed) + 1;
                    const uint64_t now = ppp::threading::Executors::GetTickCount();
                    const ppp::string key = BuildWintunCorrelationKey(e.Packet, e.PacketLength);
                    if (!key.empty())
                    {
                        RememberWintunTrace(key, TraceContext{ flow_id, now, now + 60000 });
                    }
                    LogWintunTraceStage("TUN_RX", flow_id, e.Packet, e.PacketLength,
                        GetInterfaceIndex(), interface_mtu_.load(std::memory_order_relaxed),
                        now, count, 0, "local", "tunnel", 1);
                    TAP_WINDOWS_INPUT_TRACE = TapWindowsInputTrace{
                        this, e.Packet, e.PacketLength, flow_id, now };
                    trace_active = true;
                }
            }
            ITap::OnInput(e);
            if (trace_active)
            {
                TAP_WINDOWS_INPUT_TRACE = previous_trace;
            }
        }

        void TapWindows::RememberWintunTrace(const ppp::string& key,
            const TraceContext& context) noexcept
        {
            if (key.empty() || context.flow_id == 0)
            {
                return;
            }
            std::lock_guard<std::mutex> guard(wintun_trace_mutex_);
            if (wintun_trace_contexts_.size() >= 8192)
            {
                const uint64_t now = ppp::threading::Executors::GetTickCount();
                for (auto iterator = wintun_trace_contexts_.begin(); iterator != wintun_trace_contexts_.end();)
                {
                    if (iterator->second.expires_at <= now)
                    {
                        iterator = wintun_trace_contexts_.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
                if (wintun_trace_contexts_.size() >= 8192)
                {
                    wintun_trace_contexts_.erase(wintun_trace_contexts_.begin());
                }
            }
            wintun_trace_contexts_[key] = context;
        }

        bool TapWindows::PeekWintunTrace(const ppp::string& key, TraceContext& context) noexcept
        {
            if (key.empty())
            {
                return false;
            }
            const uint64_t now = ppp::threading::Executors::GetTickCount();
            std::lock_guard<std::mutex> guard(wintun_trace_mutex_);
            auto iterator = wintun_trace_contexts_.find(key);
            if (iterator == wintun_trace_contexts_.end())
            {
                return false;
            }
            if (iterator->second.expires_at <= now)
            {
                wintun_trace_contexts_.erase(iterator);
                return false;
            }
            context = iterator->second;
            return true;
        }

        bool TapWindows::TakeWintunTrace(const ppp::string& key, TraceContext& context) noexcept
        {
            if (key.empty())
            {
                return false;
            }
            const uint64_t now = ppp::threading::Executors::GetTickCount();
            std::lock_guard<std::mutex> guard(wintun_trace_mutex_);
            auto iterator = wintun_trace_contexts_.find(key);
            if (iterator == wintun_trace_contexts_.end())
            {
                return false;
            }
            context = iterator->second;
            wintun_trace_contexts_.erase(iterator);
            return context.expires_at > now;
        }

        bool TapWindows::OutputWintun(const void* packet, int packet_size,
            uint64_t flow_id, uint64_t trace_started) noexcept
        {
            if (NULLPTR == packet || packet_size < 1)
            {
                wintun_validate_failures_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (!ValidateWintunInjectionPacket(packet, packet_size,
                    interface_mtu_.load(std::memory_order_relaxed), IPAddress, IPv6Address))
            {
                wintun_validate_failures_.fetch_add(1, std::memory_order_relaxed);
                LogWintunFailure("WINTUN_VALIDATE_FAIL", flow_id, packet_size);
                return false;
            }

            const uint64_t built_count =
                wintun_built_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (GetDataplaneTrace())
            {
                LogWintunTraceStage("PACKET_BUILT", flow_id, packet, packet_size,
                    GetInterfaceIndex(), interface_mtu_.load(std::memory_order_relaxed),
                    trace_started, built_count);
            }

            WintunAdapter* wintun = static_cast<WintunAdapter*>(GetHandle());
            if (!wintun->IsOpen())
            {
                wintun_submit_failures_.fetch_add(1, std::memory_order_relaxed);
                LogWintunFailure("WINTUN_SUBMIT_CLOSED", flow_id, packet_size);
                return false;
            }

            bool allocated = false;
            const bool submitted = wintun->SendPacket((uint8_t*)packet, packet_size, &allocated);
            if (allocated)
            {
                const uint64_t allocated_count = wintun_allocated_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (GetDataplaneTrace())
                {
                    LogWintunTraceStage("WINTUN_ALLOCATED", flow_id, packet, packet_size,
                        GetInterfaceIndex(), interface_mtu_.load(std::memory_order_relaxed),
                        trace_started, allocated_count);
                }
            }
            if (submitted)
            {
                const uint64_t count = wintun_submit_successes_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (GetDataplaneTrace())
                {
                    LogWintunTraceStage("WINTUN_SUBMITTED WINTUN_SUBMIT_OK", flow_id,
                        packet, packet_size, GetInterfaceIndex(),
                        interface_mtu_.load(std::memory_order_relaxed), trace_started, count);
                }
            }
            else
            {
                wintun_submit_failures_.fetch_add(1, std::memory_order_relaxed);
                LogWintunFailure("WINTUN_SUBMIT_FAIL", flow_id, packet_size);
            }
            return submitted;
        }

        void TapWindows::LogWintunFailure(const char* stage, uint64_t flow_id, int packet_size) noexcept
        {
            const uint64_t now = ppp::threading::Executors::GetTickCount();
            uint64_t previous = wintun_error_log_last_ms_.load(std::memory_order_relaxed);
            if (now >= previous + 1000 &&
                wintun_error_log_last_ms_.compare_exchange_strong(previous, now, std::memory_order_relaxed))
            {
                const uint64_t suppressed = wintun_error_log_suppressed_.exchange(0, std::memory_order_relaxed);
                LOG_ERROR("DATAPLANE %s flow_id=%llu bytes=%d mtu=%d suppressed=%llu",
                    NULLPTR != stage ? stage : "WINTUN_FAILURE",
                    (unsigned long long)flow_id,
                    packet_size,
                    interface_mtu_.load(std::memory_order_relaxed),
                    (unsigned long long)suppressed);
            }
            else
            {
                wintun_error_log_suppressed_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool TapWindows::Output(const std::shared_ptr<Byte>& packet, int packet_size) noexcept
        {
            if (wintun_)
            {
                return Output(packet.get(), packet_size);
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

        void TapWindows::SetDataplaneTrace(bool value) noexcept
        {
            TAP_WINDOWS_DATAPLANE_TRACE.store(value, std::memory_order_release);
        }

        bool TapWindows::GetDataplaneTrace() noexcept
        {
            return TAP_WINDOWS_DATAPLANE_TRACE.load(std::memory_order_acquire);
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

            const bool result = ppp::win32::network::SetInterfaceMtuIpSubInterface(interface_index, mtu);
            if (result)
            {
                interface_mtu_.store(ppp::net::native::ip_hdr::Mtu(mtu, false), std::memory_order_relaxed);
            }
            return result;
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
