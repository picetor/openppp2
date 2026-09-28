#pragma once

#include <cstdint>
#include <mutex>
#include <atomic>

namespace ppp::app::client {

// Shared by all outbounds; one request/consumption per core runtime.
class LinkRestartRequest final {
public:
    void Request() noexcept {
        int expected = 0;
        state_.compare_exchange_strong(expected, 1);
    }
    bool Consume() noexcept {
        int expected = 1;
        return state_.compare_exchange_strong(expected, 2);
    }
private:
    std::atomic<int> state_{0}; // idle, pending, consumed
};

// Counts failed connection attempts within one primary-role generation.
// The caller publishes a restart request when Failed() first returns true.
// That request must survive later success or a change of primary outbound.
class LinkRestartPolicy final {
public:
    explicit LinkRestartPolicy(bool primary) noexcept : primary_(primary) {}

    std::uint64_t BeginAttempt() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    void SetPrimary(bool primary) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (primary_ != primary) {
            primary_ = primary;
            ++generation_;
            failures_ = 0;
        }
    }

    void Established(std::uint64_t generation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_) failures_ = 0;
    }

    bool Failed(std::uint64_t generation, int limit) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (limit <= 0 || !primary_ || generation != generation_ || requested_) return false;
        if (++failures_ < limit) return false;
        requested_ = true;
        return true;
    }

private:
    std::mutex mutex_;
    std::uint64_t generation_ = 0;
    int failures_ = 0;
    bool primary_;
    bool requested_ = false;
};

} // namespace ppp::app::client
