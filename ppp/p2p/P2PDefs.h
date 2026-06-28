#pragma once

/**
 * @file P2PDefs.h
 * @brief Common P2P type definitions and constants.
 */

namespace ppp {
    namespace p2p {
        /**
         * @brief NAT type classification enum.
         *
         * Inferred from observed UDP relay traffic patterns.
         */
        enum P2PNatType {
            Unknown          = 0,  ///< NAT type could not be determined.
            FullCone         = 1,  ///< Full-cone NAT (one-to-one mapping).
            RestrictedCone   = 2,  ///< Address-restricted cone NAT.
            PortRestricted   = 3,  ///< Port-restricted cone NAT.
            Symmetric        = 4,  ///< Symmetric NAT.
            UdpBlocked       = 5,  ///< UDP is blocked entirely.
        };
    }
}
