#pragma once

#include <stdio.h>
#include <stdint.h>

namespace ppp {
    namespace net {
        namespace native {
#pragma pack(push, 1)
            struct
#if defined(__GNUC__) || defined(__clang__)
                __attribute__((packed)) 
#endif
            ip_hdr {
            public:
                enum Flags {
                    IP_RF                                                   = 0x8000,            /* reserved fragment flag */
                    IP_DF                                                   = 0x4000,            /* dont fragment flag */
                    IP_MF                                                   = 0x2000,            /* more fragments flag */
                    IP_OFFMASK                                              = 0x1fff,            /* mask for fragmenting bits */
                };

            public:
                 /* version / header length / type of service */
                 unsigned char                                              v_hl;
                 /* type of service */
                 unsigned char                                              tos;
                 /* total length */
                 unsigned short                                             len;
                 /* identification */
                 unsigned short                                             id;
                 /* fragment offset field */                                
                 unsigned short                                             flags;
                 /* time to live */
                 unsigned char                                              ttl;
                 /* protocol */
                 unsigned char                                              proto;
                 /* checksum */
                 unsigned short                                             chksum;
                 /* source and destination IP addresses */
                 unsigned int                                               src;
                 union {
                    unsigned int                                            dst;
                    unsigned int                                            dest;
                 };

            public:
                static int                                                  IPH_V(struct ip_hdr* hdr) noexcept {
                    return ((hdr)->v_hl >> 4);      
                }       
                static int                                                  IPH_HL(struct ip_hdr* hdr) noexcept {
                    return ((hdr)->v_hl & 0x0f);        
                }       
                static int                                                  IPH_PROTO(struct ip_hdr* hdr) noexcept {
                    return ((hdr)->proto & 0xff);       
                }       
                static int                                                  IPH_OFFSET(struct ip_hdr* hdr) noexcept {
                    return (hdr)->flags;        
                }       
                static int                                                  IPH_TTL(struct ip_hdr* hdr) noexcept {
                    return ((hdr)->ttl & 0xff);
                }

            public:
                static int                                                  Mtu(int mtu, bool v4_or_v6) noexcept {
                    static constexpr int MTU_V4_MIN = 68;
                    static constexpr int MTU_V6_MIN = 1280;

                    if (mtu > ip_hdr::MTU) {
                        mtu = ip_hdr::MTU;
                    }
                    elif(v4_or_v6) {
                        if (mtu < MTU_V4_MIN) {
                            mtu = MTU_V4_MIN;
                        }
                    }
                    elif(mtu < MTU_V6_MIN) {
                        mtu = MTU_V6_MIN;
                    }

                    return mtu;
                }
                static int                                                  Mss(int mtu, bool v4_or_v6) noexcept {
                    mtu = ip_hdr::Mtu(mtu, v4_or_v6);
                    if (v4_or_v6) {
                        return mtu - (ip_hdr::IP_HLEN << 0);
                    }
                    else {
                        return mtu - (ip_hdr::IP_HLEN << 1);
                    }
                }

            public:
                /**
                 * @brief Names the test that made Parse() reject a packet.
                 *
                 * The data path used to log only the packet length and the first
                 * byte, which cannot tell a truncated frame from an expired TTL
                 * from a protocol the stack does not carry (for example an IGMP
                 * report carrying a router-alert option, IHL=6).  The reason
                 * makes such a drop diagnosable without a packet capture.
                 */
                typedef enum {
                    IPParseReason_None = 0,             ///< The packet parsed.
                    IPParseReason_InvalidPacket,        ///< Null buffer.
                    IPParseReason_Truncated,            ///< The buffer is shorter than a base IPv4 header.
                    IPParseReason_Version,              ///< The version nibble is not 4.
                    IPParseReason_HeaderLength,         ///< IHL exceeds the buffer, or is below 20 bytes.
                    IPParseReason_Ttl,                  ///< The TTL already expired.
                    IPParseReason_Destination,          ///< The destination address is 0.0.0.0.
                    IPParseReason_Source,               ///< The source address is 0.0.0.0 or 255.255.255.255.
                    IPParseReason_Checksum,             ///< The header checksum is wrong (only when PACKET_CHECKSUM is enabled).
                    IPParseReason_Protocol,             ///< The protocol is not TCP, UDP or ICMP.
                } IPParseReason;

                /// @brief Stable short name of an IPParseReason, for logs.
                static const char*                                          ParseReasonName(IPParseReason reason) noexcept;

                /// @brief Short name of an IP protocol number ("tcp", "igmp", ...), or NULLPTR.
                static const char*                                          ProtocolName(int proto) noexcept;

                /**
                 * @brief Parses an IPv4 header.
                 * @param packet The raw frame.
                 * @param size The frame length; truncated in place to the IP total length when they disagree.
                 * @param reason When not NULLPTR, receives the failing test on rejection.
                 */
                static struct ip_hdr*                                       Parse(const void* packet, int& size, IPParseReason* reason = NULLPTR) noexcept;
                static unsigned short                                       NewId() noexcept;

            public:
                static const int                                            IP_HLEN;
                static const unsigned char                                  IP_DFT_TTL;

            public:
                static constexpr unsigned char                              IP_VER                  = 4;
                static constexpr unsigned int                               IP_ADDR_ANY_VALUE       = INADDR_ANY;
                static constexpr unsigned int                               IP_ADDR_BROADCAST_VALUE = INADDR_BROADCAST;
                static constexpr int                                        TOS_ROUTIN_MODE         = 0;
                static constexpr unsigned char                              IP_PROTO_IP             = 0;
                static constexpr unsigned char                              IP_PROTO_ICMP           = 1;
                static constexpr unsigned char                              IP_PROTO_UDP            = 17;
                static constexpr unsigned char                              IP_PROTO_TCP            = 6;
                static constexpr int                                        MTU                     = 1500;
            };
#pragma pack(pop)
        }
    }
}