#include <ppp/net/native/checksum.h>
#include <ppp/net/packet/IPFrame.h>
#include <ppp/net/packet/UdpFrame.h>
#include <ppp/ipv6/IPv6Packet.h>

using namespace ppp::net::native;

namespace ppp {
    namespace net {
        namespace packet {
            std::shared_ptr<IPFrame> UdpFrame::ToIp(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) {
                if (this->AddressesFamily != AddressFamily::InterNetwork) {
                    throw std::runtime_error("UDP frames of this address family type are not supported.");
                }

                std::shared_ptr<BufferSegment> payload = this->Payload;
                if (NULLPTR == payload || NULLPTR == payload->Buffer) {
                    return NULLPTR;
                }

                int payload_size = payload->Length;
                if (payload_size <= 0) {
                    return NULLPTR;
                }

                int payload_offset = sizeof(udp_hdr);
                int message_size_ = payload_offset + payload_size;

                std::shared_ptr<Byte> message_ = ppp::threading::BufferswapAllocator::MakeByteArray(allocator, message_size_);
                if (NULLPTR == message_) {
                    return NULLPTR;
                }
                else {
                    memcpy(message_.get() + payload_offset, payload->Buffer.get(), payload_size);
                }

                struct udp_hdr* udphdr = (struct udp_hdr*)message_.get();
                udphdr->src = ntohs(this->Source.Port);
                udphdr->dest = ntohs(this->Destination.Port);
                udphdr->len = ntohs(message_size_);
                udphdr->chksum = 0;

                UInt16 pseudo_checksum = inet_chksum_pseudo(message_.get(),
                    ip_hdr::IP_PROTO_UDP,
                    message_size_,
                    this->Source.GetAddress(),
                    this->Destination.GetAddress());
                if (pseudo_checksum == 0) {
                    pseudo_checksum = 0xffff;
                }

                udphdr->chksum = pseudo_checksum;

                std::shared_ptr<IPFrame> packet = make_shared_object<IPFrame>();
                if (NULLPTR == packet) {
                    return NULLPTR;
                }

                packet->ProtocolType = ip_hdr::IP_PROTO_UDP;
                packet->Source = this->Source.GetAddress();
                packet->Destination = this->Destination.GetAddress();
                packet->Ttl = this->Ttl;
                packet->Flags = (IPFlags)0x00;
                packet->Tos = ppp::net::Socket::IsDefaultFlashTypeOfService() ? IPFrame::DefaultFlashTypeOfService() : 0x04;

                std::shared_ptr<BufferSegment> packet_payload = make_shared_object<BufferSegment>(message_, message_size_);
                if (NULLPTR == packet_payload) {
                    return NULLPTR;
                }
                
                packet->Payload = packet_payload;
                return packet;
            }

            std::shared_ptr<BufferSegment> UdpFrame::ToIp6(const std::shared_ptr<ppp::threading::BufferswapAllocator>& allocator) {
                if (this->AddressesFamily != AddressFamily::InterNetworkV6) {
                    throw std::runtime_error("UDP frames of this address family type are not supported.");
                }

                std::shared_ptr<BufferSegment> payload = this->Payload;
                if (NULLPTR == payload || NULLPTR == payload->Buffer) {
                    return NULLPTR;
                }

                int payload_size = payload->Length;
                if (payload_size <= 0) {
                    return NULLPTR;
                }

                int udp_hdr_size = sizeof(udp_hdr);
                int ipv6_hdr_size = sizeof(ppp::ipv6::PacketHeader);
                int udp_length = udp_hdr_size + payload_size;
                int total_length = ipv6_hdr_size + udp_length;

                std::shared_ptr<Byte> message_ = ppp::threading::BufferswapAllocator::MakeByteArray(allocator, total_length);
                if (NULLPTR == message_) {
                    return NULLPTR;
                }

                Byte* p = message_.get();

                // Helper: extract an address_v6 from IPEndPoint raw bytes.
                auto to_address_v6 = [](IPEndPoint& ep) noexcept -> boost::asio::ip::address_v6 {
                    int len;
                    Byte* data = ep.GetAddressBytes(len);
                    boost::asio::ip::address_v6::bytes_type bytes{};
                    if (len >= (int)bytes.size()) {
                        memcpy(bytes.data(), data, bytes.size());
                    }
                    return boost::asio::ip::address_v6(bytes);
                };

                // --- IPv6 header ---
                ppp::ipv6::PacketHeader* ipv6 = reinterpret_cast<ppp::ipv6::PacketHeader*>(p);
                ipv6->VersionTrafficClass = 0x60;
                ipv6->TrafficClassFlow    = 0;
                ipv6->FlowLabelLow        = 0;
                ipv6->PayloadLength       = htons(static_cast<uint16_t>(udp_length));
                ipv6->NextHeader          = 17; /* UDP */
                ipv6->HopLimit            = this->Ttl != 0 ? this->Ttl : ppp::ipv6::IPv6_DEFAULT_HOP_LIMIT;

                {
                    boost::asio::ip::address_v6 src_v6 = to_address_v6(this->Source);
                    boost::asio::ip::address_v6 dst_v6 = to_address_v6(this->Destination);
                    boost::asio::ip::address_v6::bytes_type src_bytes = src_v6.to_bytes();
                    boost::asio::ip::address_v6::bytes_type dst_bytes = dst_v6.to_bytes();
                    memcpy(ipv6->Source,      src_bytes.data(), sizeof(ipv6->Source));
                    memcpy(ipv6->Destination, dst_bytes.data(), sizeof(ipv6->Destination));
                }

                // --- UDP header ---
                udp_hdr* udp = reinterpret_cast<udp_hdr*>(p + ipv6_hdr_size);
                udp->src    = htons(static_cast<uint16_t>(this->Source.Port));
                udp->dest   = htons(static_cast<uint16_t>(this->Destination.Port));
                udp->len    = htons(static_cast<uint16_t>(udp_length));
                udp->chksum = 0;

                // --- Payload ---
                memcpy(p + ipv6_hdr_size + udp_hdr_size, payload->Buffer.get(), payload_size);

                // --- UDP checksum (mandatory for IPv6) ---
                {
                    boost::asio::ip::address_v6 src_v6 = to_address_v6(this->Source);
                    boost::asio::ip::address_v6 dst_v6 = to_address_v6(this->Destination);
                    unsigned short chk = ppp::ipv6::ComputePseudoChecksum(
                        reinterpret_cast<unsigned char*>(udp), udp_length,
                        src_v6, dst_v6, 17);
                    if (chk == 0) {
                        chk = 0xffff;
                    }
                    udp->chksum = chk;
                }

                return make_shared_object<BufferSegment>(message_, total_length);
            }

            std::shared_ptr<UdpFrame> UdpFrame::Parse(const IPFrame* frame) noexcept {
                if (NULLPTR == frame) {
                    return NULLPTR;
                }

                std::shared_ptr<BufferSegment> messages = frame->Payload;
                if (NULLPTR == messages || messages->Length <= 0) {
                    return NULLPTR;
                }

                struct udp_hdr* udphdr = (struct udp_hdr*)messages->Buffer.get();
                if (NULLPTR == udphdr) {
                    return NULLPTR;
                }

                if (messages->Length != ntohs(udphdr->len)) {
                    return NULLPTR;
                }

                int offset = sizeof(struct udp_hdr);
                int payload_len = messages->Length - offset;
                if (payload_len <= 0) {
                    return NULLPTR;
                }

#if defined(PACKET_CHECKSUM)
                if (udphdr->chksum != 0) {
                    UInt32 pseudo_checksum = inet_chksum_pseudo((unsigned char*)udphdr,
                        ip_hdr::IP_PROTO_UDP,
                        messages->Length,
                        frame->Source,
                        frame->Destination);
                    if (pseudo_checksum != 0) {
                        return NULLPTR;
                    }
                }
#endif

                std::shared_ptr<UdpFrame> packet = make_shared_object<UdpFrame>();
                if (NULLPTR == packet) {
                    return NULLPTR;
                }
                
                packet->AddressesFamily = AddressFamily::InterNetwork;
                packet->Ttl = frame->Ttl;
                packet->Source = IPEndPoint(frame->Source, ntohs(udphdr->src));
                packet->Destination = IPEndPoint(frame->Destination, ntohs(udphdr->dest));

                std::shared_ptr<Byte> buffer = messages->Buffer;
                std::shared_ptr<BufferSegment> packet_payload = make_shared_object<BufferSegment>(
                    wrap_shared_pointer(buffer.get() + offset, buffer), payload_len);

                if (NULLPTR == packet_payload) {
                    return NULLPTR;
                }

                packet->Payload = packet_payload;
                return packet;
            }
        }
    }
}