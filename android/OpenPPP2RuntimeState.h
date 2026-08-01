#pragma once

// ============================================================================
// OpenPPP2RuntimeState.h -- Android runtime snapshot adaptation layer.
//
// 背景
// -----
// libopenppp2.cpp 来自 Miaocchi 的 Android 客户端框架，其运行时快照机制
// （runtime lifecycle -> JSON snapshot -> Kotlin onRuntimeSnapshot）是安卓
// 框架与 Flutter UI 之间的状态接口，属于"安卓框架"的一部分，予以保留。
//
// 但本地 openppp2 主线的 ppp 库是旧版架构，没有 Miaocchi 的
// ppp/app/runtime 全套实现。为避免把 Miaocchi 的 ppp 侧架构整体搬进本地
// 仓库，此处提供一个精简的运行时快照适配层：
//
//   * 全部基于标准库实现（无 ppp 库内部依赖、无 Android 平台依赖）；
//   * 仅实现 libopenppp2.cpp 实际使用到的类型与函数；
//   * 快照 JSON 字段与 Kotlin/Flutter 消费端保持兼容。
//
// 序列化（SerializeRuntimeSnapshot）与"从本地 ppp 库对象构造运行时状态"
// （GetClientMuxRuntimeState / GetClientRuntimeReadiness）在配套的
// OpenPPP2RuntimeState.cpp 中实现。
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ppp {
    namespace app {
        namespace mux {

            // 精简版 mux 运行时状态：仅包含 Android 快照展示所需的字段。
            // 本地 ppp 库的 vmux 实现不提供 Miaocchi 的完整协商能力，
            // 因此上报模式固定为 compat 的简化表示。
            struct MuxRuntimeState final {
                std::string requested_mode;      ///< User/config preset: compat|flow|balance|stripe
                std::string effective_mode;      ///< Negotiated preset after capability intersection
                std::string receiver_ordering;   ///< compat|flow_v2
                std::string scheduler;           ///< competition|round_robin
                std::string pool_policy;         ///< fixed|adaptive
                bool turbo = false;              ///< flow turbo active
                bool reliability = false;        ///< reliability sub-protocol agreed
                bool fec = false;                ///< XOR parity FEC agreed
                std::uint16_t active_links = 0;
                std::string fallback_reason;
            };

            inline const char* MuxSchedulerName(const std::string& mode) noexcept {
                return mode == "stripe" ? "round_robin" : "competition";
            }

            inline const char* MuxPoolPolicyName(const std::string& mode, bool turbo) noexcept {
                return (mode == "flow" && turbo) ? "adaptive" : "fixed";
            }

            inline void FillMuxPresentation(MuxRuntimeState& state) noexcept {
                state.scheduler = MuxSchedulerName(state.effective_mode);
                state.pool_policy = MuxPoolPolicyName(state.effective_mode, state.turbo);
            }

        }
    }
}

namespace ppp {
    namespace app {
        namespace runtime {

            enum class RuntimePhase : std::uint8_t {
                Idle,
                Starting,
                PreparingHost,
                Connecting,
                Handshaking,
                ApplyingPolicy,
                Connected,
                Reconnecting,
                Stopping,
                Failed,
                Unknown,
            };

            inline const char* ToString(RuntimePhase phase) noexcept {
                switch (phase) {
                case RuntimePhase::Idle: return "idle";
                case RuntimePhase::Starting: return "starting";
                case RuntimePhase::PreparingHost: return "preparing_host";
                case RuntimePhase::Connecting: return "connecting";
                case RuntimePhase::Handshaking: return "handshaking";
                case RuntimePhase::ApplyingPolicy: return "applying_policy";
                case RuntimePhase::Connected: return "connected";
                case RuntimePhase::Reconnecting: return "reconnecting";
                case RuntimePhase::Stopping: return "stopping";
                case RuntimePhase::Failed: return "failed";
                default: return "unknown";
                }
            }

            inline RuntimePhase ParseRuntimePhase(const std::string& value) noexcept {
                if (value == "idle") return RuntimePhase::Idle;
                if (value == "starting") return RuntimePhase::Starting;
                if (value == "preparing_host") return RuntimePhase::PreparingHost;
                if (value == "connecting") return RuntimePhase::Connecting;
                if (value == "handshaking") return RuntimePhase::Handshaking;
                if (value == "applying_policy") return RuntimePhase::ApplyingPolicy;
                if (value == "connected") return RuntimePhase::Connected;
                if (value == "reconnecting") return RuntimePhase::Reconnecting;
                if (value == "stopping") return RuntimePhase::Stopping;
                if (value == "failed") return RuntimePhase::Failed;
                if (value == "unknown") return RuntimePhase::Unknown;
                return RuntimePhase::Unknown;
            }

            struct RuntimeError final {
                std::uint32_t code = 0;
                std::string severity;
                bool retryable = false;
                std::string user_message_key;
                std::string diagnostic_detail;

                bool HasError() const noexcept {
                    return code != 0 || !diagnostic_detail.empty();
                }
            };

            struct RuntimeTraffic final {
                std::uint64_t rx_bytes = 0;
                std::uint64_t tx_bytes = 0;
            };

            struct RuntimeSnapshot final {
                static constexpr std::uint32_t SchemaVersion = 1;

                std::uint32_t schema_version = SchemaVersion;
                std::uint64_t generation = 0;
                std::uint64_t monotonic_ms = 0;
                RuntimePhase phase = RuntimePhase::Idle;
                std::string role;
                std::string server;
                std::string transport;
                std::vector<std::string> capabilities;
                std::string requested_mux_mode;
                std::string effective_mux_mode;
                std::string mux_receiver_ordering;
                std::string mux_scheduler;       ///< competition|round_robin
                std::string mux_pool_policy;     ///< fixed|adaptive
                bool mux_turbo = false;
                std::uint16_t mux_active_links = 0;
                std::string mux_fallback_reason;
                RuntimeTraffic traffic;
                std::uint64_t connected_monotonic_ms = 0;
                RuntimeError last_error;
            };

            struct RuntimeReadiness final {
                bool session = false;
                bool adapter = false;
                bool route = false;
                bool dns = false;
                bool policy = false;

                bool IsFullyReady() const noexcept {
                    return session && adapter && route && dns && policy;
                }
            };

            struct ClientRuntimeReadinessFacts final {
                bool session_established = false;
                bool adapter_open = false;
                bool route_required = true;
                bool route_applied = false;
                bool dns_required = true;
                bool dns_configured = false;
                bool dns_session_active = false;
                bool policy_negotiated = false;
            };

            inline RuntimeReadiness BuildClientRuntimeReadiness(
                const ClientRuntimeReadinessFacts& facts) noexcept {
                RuntimeReadiness readiness;
                readiness.session = facts.session_established;
                readiness.adapter = facts.adapter_open;
                readiness.route = !facts.route_required || facts.route_applied;
                readiness.dns = !facts.dns_required ||
                    (facts.dns_configured && facts.dns_session_active);
                readiness.policy = facts.policy_negotiated;
                return readiness;
            }

            inline RuntimeReadiness BuildServerRuntimeReadiness(bool active) noexcept {
                return RuntimeReadiness{active, active, active, active, active};
            }

            inline RuntimePhase GateConnectedPhase(
                RuntimePhase requested,
                const RuntimeReadiness& readiness) noexcept {
                if (requested == RuntimePhase::Connected && !readiness.IsFullyReady()) {
                    return RuntimePhase::ApplyingPolicy;
                }
                return requested;
            }

            // ------------------------------------------------------------------
            // 精简版停止协调器：单一 generation 生命周期内的停止语义。
            // ------------------------------------------------------------------
            class RuntimeStopCoordinator final {
            public:
                bool BeginGeneration(std::uint64_t generation) noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    if (generation == 0 || (has_generation_ && generation <= generation_)) {
                        return false;
                    }
                    generation_ = generation;
                    has_generation_ = true;
                    stopping_ = false;
                    completed_ = false;
                    return true;
                }

                bool TryBeginStop(std::uint64_t generation) noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    if (!has_generation_ || generation != generation_ || stopping_ || completed_) {
                        return false;
                    }
                    stopping_ = true;
                    return true;
                }

                void CompleteStop(std::uint64_t generation, bool success) noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    if (!has_generation_ || generation != generation_ || !stopping_) {
                        return;
                    }
                    stopping_ = false;
                    completed_ = true;
                }

                bool IsStopping(std::uint64_t generation) const noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    return has_generation_ && generation == generation_ && stopping_;
                }

                bool IsCompleted(std::uint64_t generation) const noexcept {
                    std::lock_guard<std::mutex> scope(mutex_);
                    return has_generation_ && generation == generation_ && completed_;
                }

            private:
                mutable std::mutex mutex_;
                std::uint64_t generation_ = 0;
                bool has_generation_ = false;
                bool stopping_ = false;
                bool completed_ = false;
            };

            // ------------------------------------------------------------------
            // 精简版快照发布器：保存最新快照并通知订阅者。
            // ------------------------------------------------------------------
            class RuntimeSnapshotPublisher final {
            public:
                using Listener = std::function<void(const RuntimeSnapshot&)>;

                bool Publish(RuntimeSnapshot snapshot) noexcept {
                    RuntimeSnapshot published;
                    std::vector<Listener> listeners;
                    try {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (has_latest_ &&
                            (snapshot.generation < latest_.generation ||
                             (snapshot.generation == latest_.generation &&
                              snapshot.monotonic_ms <= latest_.monotonic_ms))) {
                            return false;
                        }

                        latest_ = std::move(snapshot);
                        has_latest_ = true;
                        published = latest_;
                        listeners.reserve(listeners_.size());
                        for (const auto& item : listeners_) {
                            listeners.emplace_back(item.second);
                        }
                    }
                    catch (...) {
                        return false;
                    }

                    for (const Listener& listener : listeners) {
                        if (!listener) {
                            continue;
                        }
                        try {
                            listener(published);
                        }
                        catch (...) {
                        }
                    }
                    return true;
                }

                std::uint64_t Subscribe(Listener listener) noexcept {
                    if (!listener) {
                        return 0;
                    }
                    try {
                        std::lock_guard<std::mutex> scope(mutex_);
                        std::uint64_t token = next_listener_token_++;
                        if (token == 0) {
                            token = next_listener_token_++;
                        }
                        listeners_.emplace(token, std::move(listener));
                        return token;
                    }
                    catch (...) {
                        return 0;
                    }
                }

                void Unsubscribe(std::uint64_t token) noexcept {
                    if (token == 0) {
                        return;
                    }
                    std::lock_guard<std::mutex> scope(mutex_);
                    listeners_.erase(token);
                }

                RuntimeSnapshot GetLatest() const noexcept {
                    try {
                        std::lock_guard<std::mutex> scope(mutex_);
                        return latest_;
                    }
                    catch (...) {
                        return RuntimeSnapshot();
                    }
                }

            private:
                mutable std::mutex mutex_;
                RuntimeSnapshot latest_;
                std::unordered_map<std::uint64_t, Listener> listeners_;
                std::uint64_t next_listener_token_ = 1;
                bool has_latest_ = false;
            };

            // ------------------------------------------------------------------
            // 精简版运行时生命周期：generation 驱动的状态机。
            // 与 Miaocchi 版本相比移除了 P2P 状态（本地 ppp 库无此概念）。
            // ------------------------------------------------------------------
            class RuntimeLifecycle final {
            public:
                using Listener = RuntimeSnapshotPublisher::Listener;

                std::uint64_t Begin(RuntimeSnapshot seed, std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        const std::uint64_t generation = ++generation_;
                        stop_coordinator_.BeginGeneration(generation);
                        readiness_ = RuntimeReadiness();
                        requested_phase_ = RuntimePhase::Starting;

                        snapshot = std::move(seed);
                        snapshot.generation = generation;
                        snapshot.monotonic_ms = NextTimestamp(now);
                        snapshot.phase = RuntimePhase::Starting;
                        snapshot.connected_monotonic_ms = 0;
                        current_ = snapshot;
                    }
                    publisher_.Publish(snapshot);
                    return snapshot.generation;
                }

                bool Transition(
                    std::uint64_t generation,
                    RuntimePhase phase,
                    std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (generation == 0 || generation != generation_ ||
                            stop_coordinator_.IsStopping(generation) ||
                            stop_coordinator_.IsCompleted(generation)) {
                            return false;
                        }
                        requested_phase_ = phase;
                        current_.phase = GateConnectedPhase(phase, readiness_);
                        current_.monotonic_ms = NextTimestamp(now);
                        StampConnectedAt();
                        snapshot = current_;
                    }
                    return publisher_.Publish(std::move(snapshot));
                }

                bool UpdateReadiness(
                    std::uint64_t generation,
                    RuntimeReadiness readiness,
                    std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (generation == 0 || generation != generation_ ||
                            stop_coordinator_.IsStopping(generation) ||
                            stop_coordinator_.IsCompleted(generation)) {
                            return false;
                        }
                        readiness_ = readiness;
                        if (requested_phase_ == RuntimePhase::Connected &&
                            current_.phase != RuntimePhase::Stopping &&
                            current_.phase != RuntimePhase::Idle &&
                            current_.phase != RuntimePhase::Failed) {
                            current_.phase = GateConnectedPhase(requested_phase_, readiness_);
                        }
                        current_.monotonic_ms = NextTimestamp(now);
                        StampConnectedAt();
                        snapshot = current_;
                    }
                    return publisher_.Publish(std::move(snapshot));
                }

                bool UpdateMuxState(
                    std::uint64_t generation,
                    const ppp::app::mux::MuxRuntimeState& state,
                    std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (generation == 0 || generation != generation_ ||
                            stop_coordinator_.IsStopping(generation) ||
                            stop_coordinator_.IsCompleted(generation)) {
                            return false;
                        }
                        if (current_.requested_mux_mode == state.requested_mode &&
                            current_.effective_mux_mode == state.effective_mode &&
                            current_.mux_receiver_ordering == state.receiver_ordering &&
                            current_.mux_scheduler == state.scheduler &&
                            current_.mux_pool_policy == state.pool_policy &&
                            current_.mux_turbo == state.turbo &&
                            current_.mux_active_links == state.active_links &&
                            current_.mux_fallback_reason == state.fallback_reason) {
                            return true;
                        }
                        current_.requested_mux_mode = state.requested_mode;
                        current_.effective_mux_mode = state.effective_mode;
                        current_.mux_receiver_ordering = state.receiver_ordering;
                        current_.mux_scheduler = state.scheduler;
                        current_.mux_pool_policy = state.pool_policy;
                        current_.mux_turbo = state.turbo;
                        current_.mux_active_links = state.active_links;
                        current_.mux_fallback_reason = state.fallback_reason;
                        current_.monotonic_ms = NextTimestamp(now);
                        snapshot = current_;
                    }
                    return publisher_.Publish(std::move(snapshot));
                }

                bool UpdateTraffic(
                    std::uint64_t generation,
                    const RuntimeTraffic& traffic,
                    std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (generation == 0 || generation != generation_ ||
                            stop_coordinator_.IsStopping(generation) ||
                            stop_coordinator_.IsCompleted(generation)) {
                            return false;
                        }
                        if (current_.traffic.rx_bytes == traffic.rx_bytes &&
                            current_.traffic.tx_bytes == traffic.tx_bytes) {
                            return true;
                        }
                        current_.traffic = traffic;
                        current_.monotonic_ms = NextTimestamp(now);
                        snapshot = current_;
                    }
                    return publisher_.Publish(std::move(snapshot));
                }

                bool TryBeginStop(std::uint64_t generation, std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (generation == 0 || generation != generation_ ||
                            !stop_coordinator_.TryBeginStop(generation)) {
                            return false;
                        }
                        requested_phase_ = RuntimePhase::Stopping;
                        current_.phase = RuntimePhase::Stopping;
                        current_.monotonic_ms = NextTimestamp(now);
                        StampConnectedAt();
                        snapshot = current_;
                    }
                    return publisher_.Publish(std::move(snapshot));
                }

                bool CompleteStop(
                    std::uint64_t generation,
                    bool success,
                    RuntimeError error,
                    std::uint64_t now) noexcept {
                    RuntimeSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> scope(mutex_);
                        if (generation == 0 || generation != generation_ ||
                            !stop_coordinator_.IsStopping(generation)) {
                            return false;
                        }
                        stop_coordinator_.CompleteStop(generation, success);
                        requested_phase_ = success ? RuntimePhase::Idle : RuntimePhase::Failed;
                        current_.phase = requested_phase_;
                        current_.mux_active_links = 0;
                        current_.last_error = success ? RuntimeError() : std::move(error);
                        current_.monotonic_ms = NextTimestamp(now);
                        StampConnectedAt();
                        snapshot = current_;
                    }
                    return publisher_.Publish(std::move(snapshot));
                }

                RuntimeSnapshot GetSnapshot() const noexcept {
                    return publisher_.GetLatest();
                }

                std::uint64_t Subscribe(Listener listener) noexcept {
                    return publisher_.Subscribe(std::move(listener));
                }

                void Unsubscribe(std::uint64_t token) noexcept {
                    publisher_.Unsubscribe(token);
                }

            private:
                // 记录进入 Connected 的时刻，供 UI 渲染存活时长。
                void StampConnectedAt() noexcept {
                    if (current_.phase != RuntimePhase::Connected) {
                        current_.connected_monotonic_ms = 0;
                    }
                    else if (current_.connected_monotonic_ms == 0) {
                        current_.connected_monotonic_ms = current_.monotonic_ms;
                    }
                }

                std::uint64_t NextTimestamp(std::uint64_t now) noexcept {
                    const std::uint64_t minimum = current_.monotonic_ms + 1;
                    return std::max(now, minimum);
                }

                mutable std::mutex mutex_;
                RuntimeSnapshotPublisher publisher_;
                RuntimeStopCoordinator stop_coordinator_;
                RuntimeSnapshot current_;
                RuntimeReadiness readiness_;
                RuntimePhase requested_phase_ = RuntimePhase::Idle;
                std::uint64_t generation_ = 0;
            };

            // 序列化快照为 JSON（Kotlin 侧 onRuntimeSnapshot 消费）。
            // 实现在 OpenPPP2RuntimeState.cpp。
            std::string SerializeRuntimeSnapshot(const RuntimeSnapshot& snapshot);

            // ------------------------------------------------------------------
            // 从本地 ppp 库对象构造运行时状态（Android 适配入口）。
            // 实现在 OpenPPP2RuntimeState.cpp，避免头文件依赖 ppp 库内部类型。
            // 前置声明必须位于 ppp::app::client 命名空间（顶层），否则
            // 全限定名 ppp::app::client::VEthernetExchanger 会被内层
            // namespace ppp 块劫持为 runtime::ppp::app::client。
            // ------------------------------------------------------------------
        }
    }
}

namespace ppp {
    namespace app {
        namespace client {
            class VEthernetExchanger;
            class VEthernetNetworkSwitcher;
        }
    }
}

namespace ppp {
    namespace app {
        namespace runtime {

            ppp::app::mux::MuxRuntimeState GetClientMuxRuntimeState(
                const std::shared_ptr<ppp::app::client::VEthernetExchanger>& exchanger);

            RuntimeReadiness GetClientRuntimeReadiness(
                const std::shared_ptr<ppp::app::client::VEthernetNetworkSwitcher>& client);

        }
    }
}
