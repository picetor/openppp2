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
                uint32_t ip, uint32_t gw, uint32_t mask) noexcept;

            bool IsReady() noexcept override;
            bool IsOpen() noexcept override;
            bool SetInterfaceMtu(int mtu) noexcept override;
            bool Open() noexcept override;
            bool Output(const std::shared_ptr<Byte>& packet, int packet_size) noexcept override;
            bool Output(const void* packet, int packet_size) noexcept override;

            static std::shared_ptr<TapStub> Create(
                const std::shared_ptr<boost::asio::io_context>& context) noexcept;

        private:
            bool opened_ = false;
        };
    }
}
