/**
 * @file PeerPrefixRouteManager.h
 * @brief Installs and tears down peer-prefix routes on the owning switcher.
 * @license GPL-3.0
 */

#pragma once

#include <ppp/stdafx.h>

namespace ppp {
    namespace app {
        namespace protocol {
            struct VirtualEthernetInformationExtensions;
            struct PeerPrefixRouteEntry;
        }
        namespace client {
            class VEthernetNetworkSwitcher;

            /**
             * @brief Installs and tears down peer-prefix routes on the owning switcher.
             *
             * Unlike the upstream reference implementation this port has no
             * route/ RouteCoordinator layer, so host routes are installed and
             * removed through the owning switcher's AddRoute/DeleteRoute helpers.
             * The rib/fib tables are NOT replaced here because the mainline
             * client shares them with geo routing and RemoteEndpointLoader.
             */
            class PeerPrefixRouteManager {
            public:
                void Bind(VEthernetNetworkSwitcher* owner) noexcept;

                bool Apply(const ppp::app::protocol::VirtualEthernetInformationExtensions& extensions) noexcept;
                void Clear() noexcept;

            private:
                VEthernetNetworkSwitcher* owner_ = NULLPTR;
            };
        }
    }
}
