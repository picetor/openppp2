#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace ppp {
    namespace ethernet {
        template<class TDriver, class TByte>
        class DelayedPacketSlot final {
        public:
            enum class State : unsigned char {
                Empty,
                Ready,
                Replayed,
                Closed
            };

        public:
            bool StoreOnce(const std::shared_ptr<TDriver>& driver,
                const std::shared_ptr<TByte>& packet, int packet_size) noexcept {
                if (!packet || packet_size < 1) {
                    return false;
                }

                std::lock_guard<std::mutex> scope(mutex_);
                if (state_ != State::Empty) {
                    return false;
                }

                driver_ = driver;
                packet_ = packet;
                packet_size_ = packet_size;
                state_ = State::Ready;
                return true;
            }

            bool Take(std::shared_ptr<TDriver>& driver,
                std::shared_ptr<TByte>& packet, int& packet_size) noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                if (state_ != State::Ready || !packet_ || packet_size_ < 1) {
                    return false;
                }

                driver = std::move(driver_);
                packet = std::move(packet_);
                packet_size = packet_size_;
                packet_size_ = 0;
                state_ = State::Replayed;
                return true;
            }

            State Close() noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                State previous = state_;
                driver_.reset();
                packet_.reset();
                packet_size_ = 0;
                state_ = State::Closed;
                return previous;
            }

            State GetState() const noexcept {
                std::lock_guard<std::mutex> scope(mutex_);
                return state_;
            }

        private:
            mutable std::mutex mutex_;
            State state_ = State::Empty;
            std::shared_ptr<TDriver> driver_;
            std::shared_ptr<TByte> packet_;
            int packet_size_ = 0;
        };
    }
}
