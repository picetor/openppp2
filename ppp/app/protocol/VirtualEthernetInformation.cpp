#include <ppp/app/protocol/VirtualEthernetInformation.h>
#include <ppp/diagnostics/Error.h>

using ppp::auxiliary::JsonAuxiliary;

namespace ppp {
    namespace app {
        namespace protocol {
            VirtualEthernetInformation::VirtualEthernetInformation() noexcept {
                Clear();
            }

            static ppp::string STATIC_TO_STRRING(VirtualEthernetInformation& information, bool styled) noexcept {
                Json::Value json;
                information.ToJson(json);

                if (styled) {
                    return JsonAuxiliary::ToStyledString(json);
                }
                else {
                    return JsonAuxiliary::ToString(json);
                }
            }

            ppp::string VirtualEthernetInformation::ToString() noexcept {
                return STATIC_TO_STRRING(*this, true);
            }

            ppp::string VirtualEthernetInformation::ToJson() noexcept {
                return STATIC_TO_STRRING(*this, false);
            }

            void VirtualEthernetInformation::ToJson(Json::Value& json) noexcept {
                json["BandwidthQoS"]    = stl::to_string<ppp::string>(this->BandwidthQoS);
                json["ExpiredTime"]     = this->ExpiredTime;
                json["IncomingTraffic"] = stl::to_string<ppp::string>(this->IncomingTraffic);
                json["OutgoingTraffic"] = stl::to_string<ppp::string>(this->OutgoingTraffic);
            }

            std::shared_ptr<VirtualEthernetInformation> VirtualEthernetInformation::FromJson(const Json::Value& json) noexcept {
                if (!json.isObject()) {
                    return NULLPTR;
                }

                std::shared_ptr<VirtualEthernetInformation> infomartion = make_shared_object<VirtualEthernetInformation>();
                if (NULLPTR == infomartion) {
                    return NULLPTR;
                }

                infomartion->ExpiredTime     = JsonAuxiliary::AsValue<long long>(json["ExpiredTime"]);
                infomartion->BandwidthQoS    = JsonAuxiliary::AsValue<long long>(json["BandwidthQoS"]);
                infomartion->IncomingTraffic = JsonAuxiliary::AsValue<unsigned long long>(json["IncomingTraffic"]);
                infomartion->OutgoingTraffic = JsonAuxiliary::AsValue<unsigned long long>(json["OutgoingTraffic"]);
                return infomartion;
            }

            std::shared_ptr<VirtualEthernetInformation> VirtualEthernetInformation::FromJson(const ppp::string& json) noexcept {
                if (json.empty()) {
                    return NULLPTR;
                }

                Json::Value config = JsonAuxiliary::FromString(json);
                return FromJson(config);
            }

            void VirtualEthernetInformation::Clear() noexcept {
                this->ExpiredTime     = 0;
                this->BandwidthQoS    = 0;
                this->IncomingTraffic = 0;
                this->OutgoingTraffic = 0;
            }

            // ---- VirtualEthernetInformationExtensions ----

            void VirtualEthernetInformationExtensions::Clear() noexcept {
                AssignedIPv6Mode = IPv6Mode_None;
                AssignedIPv6AddressPrefixLength = 0;
                AssignedIPv6Flags = 0;
                AssignedIPv6Address = boost::asio::ip::address();
                AssignedIPv6Gateway = boost::asio::ip::address();
                AssignedIPv6RoutePrefix = boost::asio::ip::address();
                AssignedIPv6RoutePrefixLength = 0;
                AssignedIPv6Dns1 = boost::asio::ip::address();
                AssignedIPv6Dns2 = boost::asio::ip::address();
                IPv6StatusCode = 0;
                RequestedIPv6Address = boost::asio::ip::address();
                IPv6StatusMessage.clear();
                ClientExitIP = boost::asio::ip::address();
                ClientIPv4Req.Clear();
                ClientIPv4Assign.Clear();
                P2P.Clear();
            }

            bool VirtualEthernetInformationExtensions::HasAny() const noexcept {
                return AssignedIPv6Mode != IPv6Mode_None ||
                    AssignedIPv6AddressPrefixLength != 0 ||
                    AssignedIPv6Flags != 0 ||
                    AssignedIPv6Address.is_v6() ||
                    AssignedIPv6Gateway.is_v6() ||
                    AssignedIPv6RoutePrefix.is_v6() ||
                    AssignedIPv6RoutePrefixLength != 0 ||
                    AssignedIPv6Dns1.is_v6() ||
                    AssignedIPv6Dns2.is_v6() ||
                    RequestedIPv6Address.is_v6() ||
                    IPv6StatusCode != IPv6Status_None ||
                    !IPv6StatusMessage.empty() ||
                    !ClientExitIP.is_unspecified() ||
                    ClientIPv4Req.HasAny() ||
                    ClientIPv4Assign.HasAny() ||
                    P2P.HasAny();
            }

            void VirtualEthernetInformationExtensions::ToJson(Json::Value& json) const noexcept {
                json["AssignedIPv6Mode"] = AssignedIPv6Mode;
                json["AssignedIPv6AddressPrefixLength"] = AssignedIPv6AddressPrefixLength;
                json["AssignedIPv6Flags"] = AssignedIPv6Flags;
                json["AssignedIPv6RoutePrefixLength"] = AssignedIPv6RoutePrefixLength;
                json["IPv6StatusCode"] = IPv6StatusCode;

                if (AssignedIPv6Address.is_v6()) {
                    std::string value = AssignedIPv6Address.to_string();
                    json["AssignedIPv6Address"] = Json::Value(value.c_str());
                }
                if (AssignedIPv6Gateway.is_v6()) {
                    std::string value = AssignedIPv6Gateway.to_string();
                    json["AssignedIPv6Gateway"] = Json::Value(value.c_str());
                }
                if (AssignedIPv6RoutePrefix.is_v6()) {
                    std::string value = AssignedIPv6RoutePrefix.to_string();
                    json["AssignedIPv6RoutePrefix"] = Json::Value(value.c_str());
                }
                if (RequestedIPv6Address.is_v6()) {
                    std::string value = RequestedIPv6Address.to_string();
                    json["RequestedIPv6Address"] = Json::Value(value.c_str());
                }
                if (AssignedIPv6Dns1.is_v6()) {
                    std::string value = AssignedIPv6Dns1.to_string();
                    json["AssignedIPv6Dns1"] = Json::Value(value.c_str());
                }
                if (AssignedIPv6Dns2.is_v6()) {
                    std::string value = AssignedIPv6Dns2.to_string();
                    json["AssignedIPv6Dns2"] = Json::Value(value.c_str());
                }
                if (!IPv6StatusMessage.empty()) {
                    json["IPv6StatusMessage"] = Json::Value(IPv6StatusMessage.c_str());
                }
                if (!ClientExitIP.is_unspecified()) {
                    std::string value = ClientExitIP.to_string();
                    json["ClientExitIP"] = Json::Value(value.c_str());
                }
                if (ClientIPv4Req.HasAny()) {
                    Json::Value ipv4_req;
                    ClientIPv4Req.ToJson(ipv4_req);
                    json["client-ipv4-request"] = ipv4_req;
                }
                if (ClientIPv4Assign.HasAny()) {
                    Json::Value ipv4_assign;
                    ClientIPv4Assign.ToJson(ipv4_assign);
                    json["client-ipv4"] = ipv4_assign;
                }
                if (P2P.HasAny()) {
                    Json::Value p2p;
                    P2P.ToJson(p2p);
                    json["p2p"] = p2p;
                }
            }

            ppp::string VirtualEthernetInformationExtensions::ToJson() const noexcept {
                Json::Value json;
                ToJson(json);
                return JsonAuxiliary::ToString(json);
            }

            bool VirtualEthernetInformationExtensions::FromJson(VirtualEthernetInformationExtensions& value, const ppp::string& json) noexcept {
                if (json.empty()) {
                    value.Clear();
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::VEthernetInformationExtensionsFromJsonTextEmpty);
                    return false;
                }
                return FromJson(value, JsonAuxiliary::FromString(json));
            }

            bool VirtualEthernetInformationExtensions::FromJson(VirtualEthernetInformationExtensions& value, const Json::Value& json) noexcept {
                value.Clear();
                if (!json.isObject()) {
                    ppp::diagnostics::SetLastErrorCode(ppp::diagnostics::ErrorCode::ProtocolDecodeFailed);
                    return false;
                }

                value.AssignedIPv6Mode = static_cast<Byte>(JsonAuxiliary::AsInt64(json["AssignedIPv6Mode"], 0));
                if (value.AssignedIPv6Mode != IPv6Mode_None &&
                    value.AssignedIPv6Mode != IPv6Mode_Nat66 &&
                    value.AssignedIPv6Mode != IPv6Mode_Gua) {
                    value.AssignedIPv6Mode = IPv6Mode_None;
                }
                value.AssignedIPv6AddressPrefixLength = static_cast<Byte>(JsonAuxiliary::AsInt64(json["AssignedIPv6AddressPrefixLength"], JsonAuxiliary::AsInt64(json["AssignedIPv6PrefixLength"], 0)));
                value.AssignedIPv6Flags = static_cast<Byte>(JsonAuxiliary::AsInt64(json["AssignedIPv6Flags"], 0));
                value.AssignedIPv6RoutePrefixLength = static_cast<Byte>(JsonAuxiliary::AsInt64(json["AssignedIPv6RoutePrefixLength"], 0));
                value.IPv6StatusCode = static_cast<Byte>(JsonAuxiliary::AsInt64(json["IPv6StatusCode"], 0));

                boost::system::error_code ec;
                boost::asio::ip::address address;

                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["AssignedIPv6Address"]), ec);
                if (!ec && address.is_v6()) {
                    value.AssignedIPv6Address = address;
                }
                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["AssignedIPv6Gateway"]), ec);
                if (!ec && address.is_v6()) {
                    value.AssignedIPv6Gateway = address;
                }
                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["AssignedIPv6RoutePrefix"]), ec);
                if (!ec && address.is_v6()) {
                    value.AssignedIPv6RoutePrefix = address;
                }
                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["RequestedIPv6Address"]), ec);
                if (!ec && address.is_v6()) {
                    value.RequestedIPv6Address = address;
                }
                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["AssignedIPv6Dns1"]), ec);
                if (!ec && address.is_v6()) {
                    value.AssignedIPv6Dns1 = address;
                }
                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["AssignedIPv6Dns2"]), ec);
                if (!ec && address.is_v6()) {
                    value.AssignedIPv6Dns2 = address;
                }

                value.IPv6StatusMessage = JsonAuxiliary::AsString(json["IPv6StatusMessage"]);

                ec.clear();
                address = StringToAddress(JsonAuxiliary::AsString(json["ClientExitIP"]), ec);
                if (!ec && !address.is_unspecified()) {
                    value.ClientExitIP = address;
                }

                if (json.isMember("client-ipv4-request") && json["client-ipv4-request"].isObject()) {
                    ClientIPv4Request::FromJson(value.ClientIPv4Req, json["client-ipv4-request"]);
                }
                if (json.isMember("client-ipv4") && json["client-ipv4"].isObject()) {
                    ClientIPv4Assignment::FromJson(value.ClientIPv4Assign, json["client-ipv4"]);
                }
                if (json.isMember("p2p") && json["p2p"].isObject()) {
                    P2PControlMessage::FromJson(value.P2P, json["p2p"]);
                }

                return value.HasAny();
            }

            // ---- P2P control ----

            void P2PEndpointCandidate::Clear() noexcept {
                endpoint.clear();
                source.clear();
            }

            bool P2PEndpointCandidate::HasAny() const noexcept {
                return !endpoint.empty();
            }

            void P2PEndpointCandidate::ToJson(Json::Value& json) const noexcept {
                if (!endpoint.empty()) {
                    json["endpoint"] = Json::Value(endpoint.c_str());
                }
                if (!source.empty()) {
                    json["source"] = Json::Value(source.c_str());
                }
            }

            bool P2PEndpointCandidate::FromJson(P2PEndpointCandidate& value, const Json::Value& json) noexcept {
                value.Clear();
                if (!json.isObject()) {
                    return false;
                }

                value.endpoint = JsonAuxiliary::AsString(json["endpoint"]);
                value.source = JsonAuxiliary::AsString(json["source"]);
                return value.HasAny();
            }

            namespace {
                static ppp::string P2PIPv4ToString(uint32_t vip) noexcept {
                    if (vip == 0) {
                        return ppp::string();
                    }
                    boost::asio::ip::address_v4 address(ntohl(vip));
                    std::string s = address.to_string();
                    return ppp::string(s.data(), s.size());
                }
            }

            void P2PControlMessage::Clear() noexcept {
                enabled = false;
                mode.clear();
                action.clear();
                virtual_ip = 0;
                peer_virtual_ip = 0;
                token.clear();
                reason.clear();
                candidates.clear();
            }

            bool P2PControlMessage::HasAny() const noexcept {
                return enabled ||
                    !mode.empty() ||
                    !action.empty() ||
                    virtual_ip != 0 ||
                    peer_virtual_ip != 0 ||
                    !token.empty() ||
                    !reason.empty() ||
                    !candidates.empty();
            }

            void P2PControlMessage::ToJson(Json::Value& json) const noexcept {
                json["enabled"] = enabled;
                if (!mode.empty()) {
                    json["mode"] = Json::Value(mode.c_str());
                }
                if (!action.empty()) {
                    json["action"] = Json::Value(action.c_str());
                }
                ppp::string vip = P2PIPv4ToString(virtual_ip);
                if (!vip.empty()) {
                    json["virtual-ip"] = Json::Value(vip.c_str());
                }
                ppp::string peer_vip = P2PIPv4ToString(peer_virtual_ip);
                if (!peer_vip.empty()) {
                    json["peer-virtual-ip"] = Json::Value(peer_vip.c_str());
                }
                if (!token.empty()) {
                    json["token"] = Json::Value(token.c_str());
                }
                if (!reason.empty()) {
                    json["reason"] = Json::Value(reason.c_str());
                }
                if (!candidates.empty()) {
                    Json::Value arr(Json::arrayValue);
                    for (const P2PEndpointCandidate& candidate : candidates) {
                        if (!candidate.HasAny()) {
                            continue;
                        }
                        Json::Value item;
                        candidate.ToJson(item);
                        arr.append(item);
                    }
                    json["candidates"] = arr;
                }
            }

            ppp::string P2PControlMessage::ToJson() const noexcept {
                Json::Value json;
                ToJson(json);
                return JsonAuxiliary::ToString(json);
            }

            bool P2PControlMessage::FromJson(P2PControlMessage& value, const Json::Value& json) noexcept {
                value.Clear();
                if (!json.isObject()) {
                    return false;
                }

                value.enabled = JsonAuxiliary::AsValue<bool>(json["enabled"]);
                value.mode = JsonAuxiliary::AsString(json["mode"]);
                value.action = JsonAuxiliary::AsString(json["action"]);
                value.token = JsonAuxiliary::AsString(json["token"]);
                value.reason = JsonAuxiliary::AsString(json["reason"]);

                ppp::string vip = JsonAuxiliary::AsString(json["virtual-ip"]);
                if (!vip.empty()) {
                    boost::system::error_code ec;
                    boost::asio::ip::address_v4 addr = boost::asio::ip::address_v4::from_string(vip.c_str(), ec);
                    if (!ec) {
                        value.virtual_ip = htonl(addr.to_ulong());
                    }
                }

                ppp::string peer_vip = JsonAuxiliary::AsString(json["peer-virtual-ip"]);
                if (!peer_vip.empty()) {
                    boost::system::error_code ec;
                    boost::asio::ip::address_v4 addr = boost::asio::ip::address_v4::from_string(peer_vip.c_str(), ec);
                    if (!ec) {
                        value.peer_virtual_ip = htonl(addr.to_ulong());
                    }
                }

                if (json.isMember("candidates") && json["candidates"].isArray()) {
                    const Json::Value& candidates_json = json["candidates"];
                    for (Json::ArrayIndex i = 0; i < candidates_json.size(); i++) {
                        P2PEndpointCandidate candidate;
                        if (P2PEndpointCandidate::FromJson(candidate, candidates_json[i])) {
                            value.candidates.emplace_back(std::move(candidate));
                        }
                    }
                }

                return value.HasAny();
            }

            // ---- ClientIPv4Request ----

            void ClientIPv4Request::Clear() noexcept {
                enabled = false;
                mode.clear();
                address.clear();
                gateway.clear();
                mask.clear();
            }

            bool ClientIPv4Request::HasAny() const noexcept {
                return enabled ||
                    !mode.empty() ||
                    !address.empty() ||
                    !gateway.empty() ||
                    !mask.empty();
            }

            void ClientIPv4Request::ToJson(Json::Value& json) const noexcept {
                if (!enabled && !HasAny()) {
                    return;
                }
                json["enabled"] = enabled;
                if (!mode.empty()) {
                    json["mode"] = Json::Value(mode.c_str());
                }
                if (!address.empty()) {
                    json["address"] = Json::Value(address.c_str());
                }
                if (!gateway.empty()) {
                    json["gateway"] = Json::Value(gateway.c_str());
                }
                if (!mask.empty()) {
                    json["mask"] = Json::Value(mask.c_str());
                }
            }

            ppp::string ClientIPv4Request::ToJson() const noexcept {
                Json::Value json;
                ToJson(json);
                return JsonAuxiliary::ToString(json);
            }

            bool ClientIPv4Request::FromJson(ClientIPv4Request& value, const ppp::string& json) noexcept {
                if (json.empty()) {
                    value.Clear();
                    return false;
                }
                return FromJson(value, JsonAuxiliary::FromString(json));
            }

            bool ClientIPv4Request::FromJson(ClientIPv4Request& value, const Json::Value& json) noexcept {
                value.Clear();
                if (!json.isObject()) {
                    return false;
                }

                value.enabled = true;
                value.mode = JsonAuxiliary::AsString(json["mode"]);
                value.address = JsonAuxiliary::AsString(json["address"]);
                value.gateway = JsonAuxiliary::AsString(json["gateway"]);
                value.mask = JsonAuxiliary::AsString(json["mask"]);
                return true;
            }

            // ---- ClientIPv4Assignment ----

            void ClientIPv4Assignment::Clear() noexcept {
                enabled = false;
                accepted = false;
                conflict = false;
                mode.clear();
                reason.clear();
                requested_address.clear();
                address.clear();
                gateway.clear();
                mask.clear();
            }

            bool ClientIPv4Assignment::HasAny() const noexcept {
                return enabled ||
                    accepted ||
                    conflict ||
                    !mode.empty() ||
                    !reason.empty() ||
                    !requested_address.empty() ||
                    !address.empty() ||
                    !gateway.empty() ||
                    !mask.empty();
            }

            void ClientIPv4Assignment::ToJson(Json::Value& json) const noexcept {
                json["enabled"] = enabled;
                json["accepted"] = accepted;
                json["conflict"] = conflict;
                if (!mode.empty()) {
                    json["mode"] = Json::Value(mode.c_str());
                }
                if (!reason.empty()) {
                    json["reason"] = Json::Value(reason.c_str());
                }
                if (!requested_address.empty()) {
                    json["requested-address"] = Json::Value(requested_address.c_str());
                }
                if (!address.empty()) {
                    json["address"] = Json::Value(address.c_str());
                }
                if (!gateway.empty()) {
                    json["gateway"] = Json::Value(gateway.c_str());
                }
                if (!mask.empty()) {
                    json["mask"] = Json::Value(mask.c_str());
                }
            }

            ppp::string ClientIPv4Assignment::ToJson() const noexcept {
                Json::Value json;
                ToJson(json);
                return JsonAuxiliary::ToString(json);
            }

            bool ClientIPv4Assignment::FromJson(ClientIPv4Assignment& value, const ppp::string& json) noexcept {
                if (json.empty()) {
                    value.Clear();
                    return false;
                }
                return FromJson(value, JsonAuxiliary::FromString(json));
            }

            bool ClientIPv4Assignment::FromJson(ClientIPv4Assignment& value, const Json::Value& json) noexcept {
                value.Clear();
                if (!json.isObject()) {
                    return false;
                }

                value.enabled = JsonAuxiliary::AsValue<bool>(json["enabled"]);
                value.accepted = JsonAuxiliary::AsValue<bool>(json["accepted"]);
                value.conflict = JsonAuxiliary::AsValue<bool>(json["conflict"]);
                value.mode = JsonAuxiliary::AsString(json["mode"]);
                value.reason = JsonAuxiliary::AsString(json["reason"]);
                value.requested_address = JsonAuxiliary::AsString(json["requested-address"]);
                value.address = JsonAuxiliary::AsString(json["address"]);
                value.gateway = JsonAuxiliary::AsString(json["gateway"]);
                value.mask = JsonAuxiliary::AsString(json["mask"]);
                return true;
            }
        }
    }
}