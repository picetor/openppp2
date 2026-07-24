#include <ppp/tap/TapStub.h>

namespace ppp
{
    namespace tap
    {
        TapStub::TapStub(const std::shared_ptr<boost::asio::io_context>& context,
            uint32_t ip, uint32_t gw, uint32_t mask) noexcept
            : ITap(context, "proxy-stub", INVALID_HANDLE_VALUE, ip, gw, mask, false)
        {
        }

        std::shared_ptr<TapStub> TapStub::Create(
            const std::shared_ptr<boost::asio::io_context>& context) noexcept
        {
            if (NULLPTR == context)
            {
                return NULLPTR;
            }

            return ppp::make_shared_object<TapStub>(
                context,
                ::inet_addr("10.255.255.1"),
                ::inet_addr("10.255.255.2"),
                ::inet_addr("255.255.255.252"));
        }

        bool TapStub::IsReady() noexcept
        {
            return NULLPTR != GetContext();
        }

        bool TapStub::IsOpen() noexcept
        {
            return opened_ && IsReady();
        }

        bool TapStub::SetInterfaceMtu(int) noexcept
        {
            return true;
        }

        bool TapStub::Open() noexcept
        {
            if (!IsReady() || opened_)
            {
                return false;
            }

            opened_ = true;
            LOG_INFO("TapStub::Open: proxy-only runtime, no kernel adapter created");
            return true;
        }

        bool TapStub::Output(const std::shared_ptr<Byte>&, int) noexcept
        {
            return opened_;
        }

        bool TapStub::Output(const void*, int) noexcept
        {
            return opened_;
        }
    }
}
