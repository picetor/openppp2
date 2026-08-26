#include <windows/ppp/win32/network/WfpNat66.h>

#include <windows/ppp/win32/network/NetworkInterface.h>
#include <ppp/ipv6/IPv6Packet.h>

#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "Iphlpapi.lib")

namespace ppp {
    namespace win32 {
        namespace network {
            namespace {
                static HANDLE wfp_handle = INVALID_HANDLE_VALUE;

                static bool GetGlobalIPv6Address(int interface_index, boost::asio::ip::address_v6& address) noexcept {
                    address = boost::asio::ip::address_v6();
                    if (interface_index < 0) {
                        return false;
                    }

                    ULONG size = 16 * 1024;
                    ppp::vector<BYTE> buffer(size);
                    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
                    ULONG result = ::GetAdaptersAddresses(AF_INET6, flags, NULLPTR, adapters, &size);
                    if (result == ERROR_BUFFER_OVERFLOW) {
                        buffer.resize(size);
                        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                        result = ::GetAdaptersAddresses(AF_INET6, flags, NULLPTR, adapters, &size);
                    }
                    if (result != NO_ERROR) {
                        return false;
                    }

                    for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULLPTR; adapter = adapter->Next) {
                        if (static_cast<int>(adapter->IfIndex) != interface_index || adapter->OperStatus != IfOperStatusUp) {
                            continue;
                        }

                        for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress; unicast != NULLPTR; unicast = unicast->Next) {
                            if (NULLPTR == unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET6) {
                                continue;
                            }

                            const SOCKADDR_IN6* sockaddr = reinterpret_cast<const SOCKADDR_IN6*>(unicast->Address.lpSockaddr);
                            boost::asio::ip::address_v6::bytes_type bytes = {};
                            std::memcpy(bytes.data(), &sockaddr->sin6_addr, bytes.size());
                            boost::asio::ip::address_v6 candidate(bytes);
                            if (candidate.is_unspecified() || candidate.is_loopback() || candidate.is_link_local() || candidate.is_multicast() || candidate.is_site_local()) {
                                continue;
                            }
                            // fc00::/7 is a private source and cannot be used as
                            // the public NAT66 translation address.
                            if ((bytes[0] & 0xfe) == 0xfc) {
                                continue;
                            }

                            address = candidate;
                            return true;
                        }
                    }
                    return false;
                }

                static bool ParseUlaPrefix(const ppp::configurations::AppConfiguration& configuration,
                    boost::asio::ip::address_v6& prefix, int& prefix_length) noexcept {
                    ppp::string text = configuration.server.ipv6.cidr;
                    std::size_t slash = text.find('/');
                    prefix_length = configuration.server.ipv6.prefix_length;
                    if (slash != ppp::string::npos) {
                        ppp::string length = text.substr(slash + 1);
                        text = text.substr(0, slash);
                        prefix_length = atoi(length.data());
                    }
                    boost::system::error_code ec;
                    prefix = boost::asio::ip::make_address_v6(text.data(), ec);
                    prefix_length = std::max<int>(0, std::min<int>(128, prefix_length));
                    if (ec || prefix.is_unspecified() || prefix.is_multicast() || prefix.is_link_local()) {
                        return false;
                    }
                    prefix = ppp::ipv6::ComputeNetworkAddress(prefix, prefix_length);
                    return true;
                }

                static bool OpenDevice() noexcept {
                    if (wfp_handle != INVALID_HANDLE_VALUE) {
                        return true;
                    }
                    wfp_handle = ::CreateFileW(OPENPPP2_WFP_DEVICE_WIN32,
                        GENERIC_READ | GENERIC_WRITE,
                        0, NULLPTR, OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, NULLPTR);
                    return wfp_handle != INVALID_HANDLE_VALUE;
                }
            }

            bool WfpNat66::Configure(const ppp::configurations::AppConfiguration& configuration,
                int transit_interface_index, int uplink_interface_index) noexcept {
                if (configuration.server.ipv6.mode != ppp::configurations::AppConfiguration::IPv6Mode_Nat66 ||
                    transit_interface_index <= 0 || uplink_interface_index <= 0) {
                    return false;
                }

                boost::asio::ip::address_v6 ula_prefix;
                int prefix_length = 0;
                if (!ParseUlaPrefix(configuration, ula_prefix, prefix_length)) {
                    return false;
                }

                boost::asio::ip::address_v6 external_address;
                if (!GetGlobalIPv6Address(uplink_interface_index, external_address)) {
                    LOG_WARN("WfpNat66::Configure: no global IPv6 address on uplink ifindex=%d", uplink_interface_index);
                    return false;
                }

                if (!OpenDevice()) {
                    LOG_WARN("WfpNat66::Configure: signed WFP driver is not available, error=%lu", ::GetLastError());
                    return false;
                }

                OPENPPP2_WFP_NAT66_CONFIG config = {};
                config.Version = OPENPPP2_WFP_PROTOCOL_VERSION;
                config.Flags = OPENPPP2_WFP_FLAG_ENABLED | OPENPPP2_WFP_FLAG_FAIL_CLOSED;
                config.TransitIfIndex = static_cast<uint32_t>(transit_interface_index);
                config.UplinkIfIndex = static_cast<uint32_t>(uplink_interface_index);
                std::memcpy(config.UlaPrefix, ula_prefix.to_bytes().data(), sizeof(config.UlaPrefix));
                config.UlaPrefixLength = static_cast<uint8_t>(prefix_length);
                std::memcpy(config.ExternalAddress, external_address.to_bytes().data(), sizeof(config.ExternalAddress));

                DWORD returned = 0;
                if (!::DeviceIoControl(wfp_handle, OPENPPP2_WFP_IOCTL_CONFIGURE,
                    &config, sizeof(config), NULLPTR, 0, &returned, NULLPTR)) {
                    LOG_ERROR("WfpNat66::Configure: driver configuration failed, error=%lu", ::GetLastError());
                    ::CloseHandle(wfp_handle);
                    wfp_handle = INVALID_HANDLE_VALUE;
                    return false;
                }
                return true;
            }

            bool WfpNat66::Clear() noexcept {
                if (wfp_handle == INVALID_HANDLE_VALUE && !OpenDevice()) {
                    return true;
                }

                DWORD returned = 0;
                bool ok = !!::DeviceIoControl(wfp_handle, OPENPPP2_WFP_IOCTL_CLEAR,
                    NULLPTR, 0, NULLPTR, 0, &returned, NULLPTR);
                ::CloseHandle(wfp_handle);
                wfp_handle = INVALID_HANDLE_VALUE;
                return ok;
            }

            bool WfpNat66::Query(OPENPPP2_WFP_NAT66_STATUS& status) noexcept {
                status = {};
                if (!OpenDevice()) {
                    return false;
                }

                DWORD returned = 0;
                return !!::DeviceIoControl(wfp_handle, OPENPPP2_WFP_IOCTL_QUERY,
                    NULLPTR, 0, &status, sizeof(status), &returned, NULLPTR) &&
                    returned >= sizeof(status);
            }

            bool WfpNat66::IsAvailable() noexcept {
                if (!OpenDevice()) {
                    return false;
                }
                OPENPPP2_WFP_NAT66_STATUS status = {};
                return Query(status) && status.Version == OPENPPP2_WFP_PROTOCOL_VERSION;
            }
        }
    }
}
