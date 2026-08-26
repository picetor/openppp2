#pragma once

#include <ppp/stdafx.h>
#include <ppp/configurations/AppConfiguration.h>
#include <windows/wfp/OpenPpp2WfpProtocol.h>

namespace ppp {
    namespace win32 {
        namespace network {
            /**
             * @brief Controls the signed OpenPPP2 WFP NAT66 callout driver.
             *
             * The server owns only the dynamic configuration session.  Closing
             * the device handle or calling Clear() disables the callout and
             * releases all driver-side flow state; the driver itself owns the
             * WFP filters and never relies on a user-mode process staying alive.
             */
            class WfpNat66 final {
            public:
                static bool Configure(const ppp::configurations::AppConfiguration& configuration,
                    int transit_interface_index, int uplink_interface_index) noexcept;
                static bool Clear() noexcept;
                static bool Query(OPENPPP2_WFP_NAT66_STATUS& status) noexcept;
                static bool IsAvailable() noexcept;
            };
        }
    }
}
