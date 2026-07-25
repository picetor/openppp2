#pragma once

#include <ppp/tap/ITap.h>

namespace ppp
{
    namespace tap
    {
        // No-op adapter used by the desktop proxy runtime.  It satisfies the
        // VEthernet lifecycle without creating or modifying a kernel adapter.
        class TapStub final : public ITap
        {
        public:
            TapStub(const std::shared_ptr<boost::asio::io_context>& context,
                uint32_t ip, uint32_t gw, uint32_t mask,
                const ppp::vector<boost::asio::ip::address>& dns_addresses) noexcept;

            bool IsReady() noexcept override;
            bool IsOpen() noexcept override;
            bool SetInterfaceMtu(int mtu) noexcept override;
            bool Open() noexcept override;
            bool Output(const std::shared_ptr<Byte>& packet, int packet_size) noexcept override;
            bool Output(const void* packet, int packet_size) noexcept override;
            const ppp::vector<boost::asio::ip::address>& GetDnsAddresses() const noexcept { return dns_addresses_; }

            static std::shared_ptr<TapStub> Create(
                const std::shared_ptr<boost::asio::io_context>& context,
                const ppp::vector<boost::asio::ip::address>& dns_addresses) noexcept;

        private:
            bool opened_ = false;
            ppp::vector<boost::asio::ip::address> dns_addresses_;
        };
    }
}
