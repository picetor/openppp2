#pragma once

/**
 * @file VirtualEthernetTcpMss.h
 * @brief TCP MSS clamping utilities for virtual Ethernet tunnel (IPv6).
 *
 * Clamps the TCP MSS option in IPv6 SYN packets to prevent
 * IP fragmentation inside the tunnel fabric.
 */

#include <ppp/stdafx.h>
#include <ppp/net/native/ip.h>
#include <ppp/ipv6/IPv6Packet.h>

namespace ppp {
    namespace app {
        namespace protocol {
            /**
             * @brief Tunnel encapsulation overhead in bytes.
             *
             * This constant represents the per-packet overhead introduced by
             * the tunnel fabric (VirtualEthernet header + encryption + framing).
             * It is subtracted from the path MTU to compute the effective
             * TCP MSS for tunneled connections.
             *
             * A conservative default of 52 bytes covers:
             *   - VirtualEthernet header (12-16 bytes)
             *   - Encryption/MAC overhead (32-40 bytes)
             *   - Padding/alignment (4-8 bytes)
             */
            static constexpr int            kVEthernetTunnelOverhead        = 52;

            /**
             * @brief Computes a dynamic TCP MSS value suitable for tunnel transport.
             *
             * The MSS is computed as:
             *   MSS = path_mtu - IPv6_header_fixed(40) - tunnel_overhead
             *
             * @param jumbo  When true, use jumbo frame MTU (9000); otherwise standard MTU (1500).
             * @param overhead  Per-packet tunnel encapsulation overhead in bytes.
             * @return  Clamped MSS value, never less than 536 (RFC 879 minimum).
             */
            static inline int ComputeDynamicTcpMss(bool jumbo, int overhead) noexcept {
                /* Standard Ethernet MTU; jumbo frames use 9000. */
                int mtu = jumbo ? 9000 : ppp::net::native::ip_hdr::MTU;

                /* Fixed IPv6 header is 40 bytes (RFC 8200). */
                static constexpr int IPv6_HEADER_SIZE = 40;

                int mss = mtu - IPv6_HEADER_SIZE - (std::max)(0, overhead);

                /* Clamp to the minimum allowable MSS (RFC 879). */
                return (std::max)(536, mss);
            }

            /**
             * @brief IPv6 Next Header values used for extension header traversal.
             */
            namespace IPv6ExtensionHeaders {
                static constexpr Byte HopByHop       = 0;
                static constexpr Byte Routing        = 43;
                static constexpr Byte Fragment       = 44;
                static constexpr Byte DestinationOpt = 60;
                static constexpr Byte Authentication = 51;  ///< AH — Authentication Header.
                static constexpr Byte Encapsulating  = 50;  ///< ESP — Encapsulating Security Payload.
                static constexpr Byte NoNext         = 59;
                static constexpr Byte TCP            = 6;
            }

            /**
             * @brief Clamps the TCP MSS option in an IPv6 TCP SYN packet.
             *
             * Parses the IPv6 header, skips extension headers, locates the
             * TCP header, and modifies the MSS option (kind=2) if present.
             * The checksum is NOT updated by this function; the caller must
             * recompute if needed.
             *
             * @param packet        Raw IPv6 packet buffer.
             * @param packet_length Length of the packet buffer in bytes.
             * @param mss           The MSS value to clamp to.
             */
            static inline void ClampTcpMssIPv6(Byte* packet, int packet_length, int mss) noexcept {
                /* Minimum: IPv6 header (40) + TCP header (20). */
                if (NULLPTR == packet || packet_length < 60) {
                    return;
                }

                /* 
                 * Parse fixed IPv6 header.
                 * Layout (RFC 8200):
                 *   Byte 0:   Version (4 bits) | Traffic Class (8 bits)
                 *   Byte 1:   Traffic Class (4 bits) | Flow Label (4 bits)
                 *   Bytes 2-3: Flow Label (16 bits)
                 *   Bytes 4-5: Payload Length
                 *   Byte 6:   Next Header
                 *   Byte 7:   Hop Limit
                 *   Bytes 8-23: Source Address (128 bits)
                 *   Bytes 24-39: Destination Address (128 bits)
                 */
                int offset = 40; /* Fixed IPv6 header size. */
                Byte next_header = packet[6];

                /* Traverse IPv6 extension headers until we find TCP or hit a non-extensible header. */
                while (offset < packet_length) {
                    if (next_header == IPv6ExtensionHeaders::TCP) {
                        break; /* Found TCP header at 'offset'. */
                    }

                    if (next_header == IPv6ExtensionHeaders::HopByHop ||
                        next_header == IPv6ExtensionHeaders::Routing ||
                        next_header == IPv6ExtensionHeaders::DestinationOpt ||
                        next_header == IPv6ExtensionHeaders::Authentication) {
                        /* 
                         * These extension headers share the same TLV format:
                         *   Byte 0: Next Header
                         *   Byte 1: Header Extension Length (in 8-octet units, excluding the first 8 octets)
                         * So total size = (HeaderExtLen + 1) * 8
                         */
                        if (offset + 2 > packet_length) {
                            return; /* Truncated. */
                        }
                        Byte ext_len_byte = packet[offset + 1];
                        int ext_hdr_len = (ext_len_byte + 1) * 8;
                        next_header = packet[offset]; /* Read next header before advancing. */
                        offset += ext_hdr_len;
                        continue;
                    }

                    if (next_header == IPv6ExtensionHeaders::Fragment) {
                        /* Fragment header is 8 bytes total. */
                        if (offset + 8 > packet_length) {
                            return; /* Truncated. */
                        }
                        next_header = packet[offset]; /* Byte 0: Next Header. */
                        offset += 8;
                        continue;
                    }

                    if (next_header == IPv6ExtensionHeaders::Encapsulating) {
                        /* ESP header has no Next Header field we can parse generically. Stop here. */
                        return;
                    }

                    /* Unknown or NoNext — cannot proceed. */
                    return;
                }

                /* 'offset' now points to the TCP header (if next_header==TCP). */
                if (next_header != IPv6ExtensionHeaders::TCP) {
                    return; /* Not a TCP packet. */
                }

                /* Ensure we have at least the full TCP header (20 bytes). */
                if (offset + 20 > packet_length) {
                    return; /* Truncated TCP header. */
                }

                /*
                 * TCP header layout:
                 *   Byte 0:  Source Port (high)
                 *   Byte 1:  Source Port (low)
                 *   Byte 2:  Destination Port (high)
                 *   Byte 3:  Destination Port (low)
                 *   Byte 4:  Sequence Number (4 bytes)
                 *   Byte 8:  Acknowledgment Number (4 bytes)
                 *   Byte 12: Data Offset (4 bits) | Reserved (4 bits)
                 *   Byte 13: Flags (8 bits)
                 *      Bit 0 (0x01): FIN
                 *      Bit 1 (0x02): SYN
                 *      Bit 2 (0x04): RST
                 *      Bit 3 (0x08): PSH
                 *      Bit 4 (0x10): ACK
                 *      Bit 5 (0x20): URG
                 *      ...
                 *   Bytes 14-15: Window Size
                 */
                Byte data_offset_byte = packet[offset + 12];
                int tcp_header_len = (data_offset_byte >> 4) * 4; /* Data offset in 32-bit words. */
                if (tcp_header_len < 20 || offset + tcp_header_len > packet_length) {
                    return; /* Invalid or truncated TCP header. */
                }

                /* Check SYN flag (bit 1 at byte 13). */
                Byte flags = packet[offset + 13];
                if ((flags & 0x02) == 0) {
                    return; /* Not a SYN segment — MSS option only appears in SYNs. */
                }

                /* 
                 * Scan TCP options for MSS option (kind=2).
                 * TCP option format:
                 *   kind=0: End of Options List
                 *   kind=1: NOP (No-Operation)
                 *   kind=2: MSS (length=4, value=2 bytes)
                 *   kind>1: kind + length + payload
                 */
                int options_offset = offset + 20; /* Start of TCP options (after fixed 20-byte header). */
                int options_end = offset + tcp_header_len;

                while (options_offset + 1 <= options_end) {
                    Byte kind = packet[options_offset];

                    if (kind == 0) {
                        break; /* End of Options List. */
                    }

                    if (kind == 1) {
                        options_offset += 1; /* NOP — single byte. */
                        continue;
                    }

                    if (kind == 2) {
                        /* MSS option: kind(1) + length(1) + value(2) = 4 bytes total. */
                        if (options_offset + 4 > options_end) {
                            break; /* Truncated MSS option. */
                        }

                        Byte option_len = packet[options_offset + 1];
                        if (option_len != 4) {
                            options_offset += option_len;
                            continue; /* Malformed MSS option, skip. */
                        }

                        /* Read current MSS value (big-endian). */
                        unsigned short current_mss =
                            (static_cast<unsigned short>(packet[options_offset + 2]) << 8) |
                            (static_cast<unsigned short>(packet[options_offset + 3]));

                        /* Clamp MSS to our computed value. */
                        unsigned short clamped_mss = static_cast<unsigned short>(
                            (std::min)(static_cast<int>(current_mss), mss));

                        /* Write clamped MSS back (big-endian). */
                        packet[options_offset + 2] = static_cast<Byte>((clamped_mss >> 8) & 0xFF);
                        packet[options_offset + 3] = static_cast<Byte>(clamped_mss & 0xFF);

                        return; /* MSS option processed. */
                    }

                    /* Generic option with length byte. */
                    if (options_offset + 2 > options_end) {
                        break; /* Truncated option header. */
                    }

                    Byte option_len = packet[options_offset + 1];
                    if (option_len < 2) {
                        break; /* Invalid option length. */
                    }

                    options_offset += option_len;
                }
            }
        }
    }
}
