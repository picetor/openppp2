#pragma once

#include <ppp/stdafx.h>
#include <ppp/tap/ITap.h>

namespace ppp
{
    namespace tap
    {
        class TapWindows final : public ppp::tap::ITap
        {
            friend struct                           WintunAdapterDriver;

        public:
            struct WintunDiagnostics final
            {
                uint64_t flow_id;
                uint64_t tun_rx_packets;
                uint64_t policy_selected_packets;
                uint64_t remote_tx_packets;
                uint64_t remote_rx_packets;
                uint64_t local_rx_packets;
                uint64_t correlated_remote_rx_packets;
                uint64_t validate_failures;
                uint64_t built_packets;
                uint64_t allocated_packets;
                uint64_t submit_successes;
                uint64_t submit_failures;
                int interface_mtu;
            };

            enum class DriverMode
            {
                Auto,
                Wintun,
                Tap
            };

            TapWindows(const std::shared_ptr<boost::asio::io_context>& context, const ppp::string& id, void* tun, uint32_t address, uint32_t gw, uint32_t mask, bool hosted_network);
            virtual ~TapWindows() noexcept = default;

        public:
            virtual bool                            SetInterfaceMtu(int mtu) noexcept override;
            virtual void                            Dispose() noexcept override;
            WintunDiagnostics                       GetWintunDiagnostics() const noexcept
            {
                return WintunDiagnostics{
                    wintun_flow_id_.load(std::memory_order_relaxed),
                    wintun_tun_rx_packets_.load(std::memory_order_relaxed),
                    wintun_policy_selected_packets_.load(std::memory_order_relaxed),
                    wintun_remote_tx_packets_.load(std::memory_order_relaxed),
                    wintun_remote_rx_packets_.load(std::memory_order_relaxed),
                    wintun_local_rx_packets_.load(std::memory_order_relaxed),
                    wintun_correlated_remote_rx_packets_.load(std::memory_order_relaxed),
                    wintun_validate_failures_.load(std::memory_order_relaxed),
                    wintun_built_packets_.load(std::memory_order_relaxed),
                    wintun_allocated_packets_.load(std::memory_order_relaxed),
                    wintun_submit_successes_.load(std::memory_order_relaxed),
                    wintun_submit_failures_.load(std::memory_order_relaxed),
                    interface_mtu_.load(std::memory_order_relaxed) };
            }

        public:
            static bool                             DnsFlushResolverCache() noexcept;
            static bool                             SetAddresses(int interface_index, uint32_t ip, uint32_t mask, uint32_t gw) noexcept;
            static bool                             SetDnsAddresses(int interface_index, ppp::vector<uint32_t>& servers) noexcept;
            static bool                             SetDnsAddresses(int interface_index, ppp::vector<ppp::string>& servers) noexcept;
            static std::shared_ptr<ITap>            Create(const std::shared_ptr<boost::asio::io_context>& context, const ppp::string& componentId, uint32_t ip, uint32_t gw, uint32_t mask, uint32_t lease_time_in_seconds, bool hosted_network, const ppp::vector<uint32_t>& dns_addresses);
            static ppp::string                      InstallDriver(const ppp::string& path, const ppp::string& declareTapName) noexcept;
            static bool                             UninstallDriver(const ppp::string& path) noexcept;

        public:
            static bool                             IsWintun() noexcept;
            static void                             SetDriverMode(DriverMode mode) noexcept;
            static DriverMode                       GetDriverMode() noexcept;
            static void                             SetDataplaneTrace(bool value) noexcept;
            static bool                             GetDataplaneTrace() noexcept;
            static ppp::string                      FindComponentId() noexcept;
            static ppp::string                      FindComponentId(const ppp::string& key) noexcept;
            static bool                             FindAllComponentIds(ppp::unordered_set<ppp::string>& componentIds) noexcept;
            static int                              GetNetworkInterfaceIndex(const ppp::string& componentId) noexcept;
            
        protected:
            virtual bool                            Output(const void* packet, int packet_size) noexcept override;
            virtual bool                            Output(const std::shared_ptr<Byte>& packet, int packet_size) noexcept override;
            virtual bool                            OutputWithTrace(const void* packet, int packet_size, const char* stage) noexcept override;
            virtual bool                            TraceInputStage(const char* stage, const char* route_origin,
                                                        const char* route_action, int outbound,
                                                        int error_code = 0) noexcept override;
            virtual bool                            TracePacketStage(const void* packet, int packet_size,
                                                        const char* stage, const char* route_origin,
                                                        const char* route_action, int outbound,
                                                        int error_code = 0) noexcept override;
            virtual void                            OnInput(PacketInputEventArgs& e) noexcept override;
            virtual bool                            AsynchronousReadPacketLoops() noexcept override;

        private:
            struct TraceContext final
            {
                uint64_t flow_id = 0;
                uint64_t started_at = 0;
                uint64_t expires_at = 0;
            };

            void                                    LogWintunFailure(const char* stage, uint64_t flow_id, int packet_size) noexcept;
            bool                                    OutputWintun(const void* packet, int packet_size, uint64_t flow_id, uint64_t trace_started) noexcept;
            void                                    RememberWintunTrace(const ppp::string& key, const TraceContext& context) noexcept;
            bool                                    PeekWintunTrace(const ppp::string& key, TraceContext& context) noexcept;
            bool                                    TakeWintunTrace(const ppp::string& key, TraceContext& context) noexcept;
            static void*                            OpenDriver(const ppp::string& componentId) noexcept;
            static bool                             ConfigureDriver_SetDhcpMASQ(const void* handle, uint32_t ip, uint32_t gw, uint32_t mask, uint32_t lease_time_in_seconds) noexcept;
            static bool                             ConfigureDriver_SetTunModeWithAddress(const void* handle, uint32_t ip, uint32_t gw, uint32_t mask) noexcept;
            static bool                             ConfigureDriver_SetNetifUp(const void* handle, bool up) noexcept;
            static bool                             ConfigureDriver_SetDhcpOptionData(const void* handle, uint32_t ip, uint32_t gw, uint32_t mask, uint32_t dhcp, const ppp::vector<uint32_t>& dns_addresses) noexcept;

        private:
            std::shared_ptr<void>                   wintun_;
            std::atomic<int>                        interface_mtu_{ 1500 };
            std::atomic<uint64_t>                   wintun_flow_id_{ 0 };
            std::atomic<uint64_t>                   wintun_tun_rx_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_policy_selected_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_remote_tx_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_remote_rx_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_local_rx_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_correlated_remote_rx_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_validate_failures_{ 0 };
            std::atomic<uint64_t>                   wintun_built_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_allocated_packets_{ 0 };
            std::atomic<uint64_t>                   wintun_submit_successes_{ 0 };
            std::atomic<uint64_t>                   wintun_submit_failures_{ 0 };
            std::atomic<uint64_t>                   wintun_error_log_last_ms_{ 0 };
            std::atomic<uint64_t>                   wintun_error_log_suppressed_{ 0 };
            std::mutex                              wintun_trace_mutex_;
            ppp::unordered_map<ppp::string, TraceContext> wintun_trace_contexts_;
        };
    }
}
