// ============================================================================
// OpenPPP2RuntimeState.cpp -- Android runtime snapshot adaptation layer.
//
// 与 OpenPPP2RuntimeState.h 配套。此处实现：
//   * SerializeRuntimeSnapshot  -- 快照 JSON 序列化（Kotlin onRuntimeSnapshot
//                                  消费端兼容的字段集）；
//   * GetClientMuxRuntimeState  -- 从本地 VEthernetExchanger 构造 mux 状态；
//   * GetClientRuntimeReadiness -- 从本地 VEthernetNetworkSwitcher 构造就绪状态。
//
// 这些函数依赖本地 ppp 库的具体类型，因此放在 .cpp 中实现，避免头文件
// 与 ppp 库内部结构耦合。
// ============================================================================

#include <android/OpenPPP2RuntimeState.h>

#include <json/json.h>

#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/app/mux/vmux_net.h>

#include <algorithm>
#include <climits>
#include <string>
#include <utility>

namespace ppp {
    namespace app {
        namespace runtime {

            namespace detail {

                inline Json::String ToRuntimeJsonString(const std::string& value) noexcept {
                    return Json::String(value.data(), value.size());
                }

                inline std::string FromRuntimeJsonString(const Json::String& value) noexcept {
                    return std::string(value.data(), value.size());
                }

                inline void WriteRuntimeError(
                    Json::Value& root,
                    const RuntimeError& error) noexcept {
                    Json::Value value(Json::objectValue);
                    value["code"] = error.code;
                    value["severity"] = ToRuntimeJsonString(error.severity);
                    value["retryable"] = error.retryable;
                    value["user_message_key"] = ToRuntimeJsonString(error.user_message_key);
                    value["diagnostic_detail"] = ToRuntimeJsonString(error.diagnostic_detail);
                    root["last_error"] = std::move(value);
                }

                inline void WriteRuntimeTraffic(
                    Json::Value& root,
                    const RuntimeTraffic& traffic) noexcept {
                    Json::Value value(Json::objectValue);
                    value["rx_bytes"] = Json::UInt64(traffic.rx_bytes);
                    value["tx_bytes"] = Json::UInt64(traffic.tx_bytes);
                    root["traffic"] = std::move(value);
                }

            }

            std::string SerializeRuntimeSnapshot(const RuntimeSnapshot& snapshot) {
                Json::Value root(Json::objectValue);
                root["schema_version"] = snapshot.schema_version;
                root["generation"] = Json::UInt64(snapshot.generation);
                root["monotonic_ms"] = Json::UInt64(snapshot.monotonic_ms);
                root["phase"] = ToString(snapshot.phase);
                root["role"] = detail::ToRuntimeJsonString(snapshot.role);
                root["server"] = detail::ToRuntimeJsonString(snapshot.server);
                root["transport"] = detail::ToRuntimeJsonString(snapshot.transport);
                Json::Value capabilities(Json::arrayValue);
                for (const std::string& capability : snapshot.capabilities) {
                    capabilities.append(detail::ToRuntimeJsonString(capability));
                }
                root["capabilities"] = std::move(capabilities);
                root["requested_mux_mode"] = detail::ToRuntimeJsonString(snapshot.requested_mux_mode);
                root["effective_mux_mode"] = detail::ToRuntimeJsonString(snapshot.effective_mux_mode);
                root["mux_receiver_ordering"] = detail::ToRuntimeJsonString(snapshot.mux_receiver_ordering);
                root["mux_scheduler"] = detail::ToRuntimeJsonString(snapshot.mux_scheduler);
                root["mux_pool_policy"] = detail::ToRuntimeJsonString(snapshot.mux_pool_policy);
                root["mux_turbo"] = snapshot.mux_turbo;
                root["mux_active_links"] = snapshot.mux_active_links;
                root["mux_fallback_reason"] = detail::ToRuntimeJsonString(snapshot.mux_fallback_reason);
                detail::WriteRuntimeTraffic(root, snapshot.traffic);
                root["connected_monotonic_ms"] = Json::UInt64(snapshot.connected_monotonic_ms);
                detail::WriteRuntimeError(root, snapshot.last_error);

                Json::FastWriter writer;
                const Json::String encoded = writer.write(root);
                std::string json = detail::FromRuntimeJsonString(encoded);
                while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) {
                    json.pop_back();
                }
                return json;
            }

            ppp::app::mux::MuxRuntimeState GetClientMuxRuntimeState(
                const std::shared_ptr<ppp::app::client::VEthernetExchanger>& exchanger) {
                ppp::app::mux::MuxRuntimeState state;
                if (NULLPTR == exchanger) {
                    state.fallback_reason = "mux_inactive";
                    ppp::app::mux::FillMuxPresentation(state);
                    return state;
                }

                // 本地 vmux 网络建立后即认为 compat 会话处于活动状态；
                // 本地实现不提供 flow_v2/reliability/fec 协商信息，保持默认。
                const std::shared_ptr<::vmux::vmux_net>& mux = exchanger->GetMux();
                if (NULLPTR != mux && mux->is_established()) {
                    state.requested_mode = "compat";
                    state.effective_mode = "compat";
                    state.receiver_ordering = "compat";
                    state.active_links = 1;
                }
                else {
                    state.fallback_reason = "mux_inactive";
                }

                ppp::app::mux::FillMuxPresentation(state);
                return state;
            }

            RuntimeReadiness GetClientRuntimeReadiness(
                const std::shared_ptr<ppp::app::client::VEthernetNetworkSwitcher>& client) {
                ClientRuntimeReadinessFacts facts;
                if (NULLPTR != client) {
                    const std::shared_ptr<ppp::app::client::VEthernetExchanger>& exchanger =
                        client->GetExchanger();
                    facts.session_established =
                        NULLPTR != exchanger &&
                        exchanger->GetNetworkState() ==
                            ppp::app::client::VEthernetExchanger::NetworkState_Established;
                    facts.adapter_open = NULLPTR != client->GetTapNetworkInterface();

                    // 本地 ppp 库不跟踪 route/dns/policy 的独立就绪位，
                    // 按"不要求"处理，使会话建立 + 适配器打开即视为完全就绪。
                    facts.route_required = false;
                    facts.dns_required = false;
                    facts.policy_negotiated = true;
                }
                return BuildClientRuntimeReadiness(facts);
            }

        }
    }
}
