#include "../ppp/ethernet/DelayedPacketSlot.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
    struct FakeTap final {
        int id = 0;
    };

    typedef ppp::ethernet::DelayedPacketSlot<FakeTap, unsigned char> Slot;

    std::shared_ptr<unsigned char> MakePacket(unsigned char marker) {
        std::shared_ptr<unsigned char> packet(
            new unsigned char[4], std::default_delete<unsigned char[]>());
        packet.get()[0] = marker;
        packet.get()[1] = 0x22;
        packet.get()[2] = 0x33;
        packet.get()[3] = 0x44;
        return packet;
    }
}

int main() {
    {
        Slot slot;
        auto tap = std::make_shared<FakeTap>();
        auto first = MakePacket(0x11);
        auto replacement = MakePacket(0x99);
        assert(slot.StoreOnce(tap, first, 4));
        assert(!slot.StoreOnce(tap, replacement, 4));

        std::shared_ptr<FakeTap> taken_tap;
        std::shared_ptr<unsigned char> taken_packet;
        int taken_size = 0;
        assert(slot.Take(taken_tap, taken_packet, taken_size));
        assert(taken_tap == tap);
        assert(taken_size == 4);
        assert(taken_packet.get()[0] == 0x11);
        assert(!slot.Take(taken_tap, taken_packet, taken_size));
        assert(slot.Close() == Slot::State::Replayed);
        assert(!slot.StoreOnce(tap, first, 4));
    }

    for (int iteration = 0; iteration < 10000; ++iteration) {
        Slot slot;
        auto tap = std::make_shared<FakeTap>();
        auto packet = MakePacket(static_cast<unsigned char>(iteration));
        assert(slot.StoreOnce(tap, packet, 4));

        std::atomic<bool> start{false};
        bool take_succeeded = false;
        Slot::State close_previous = Slot::State::Empty;
        std::thread take_thread([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::shared_ptr<FakeTap> out_tap;
            std::shared_ptr<unsigned char> out_packet;
            int out_size = 0;
            take_succeeded = slot.Take(out_tap, out_packet, out_size);
            if (take_succeeded) {
                assert(out_tap == tap);
                assert(out_size == 4);
                assert(out_packet.get()[0] == static_cast<unsigned char>(iteration));
            }
        });
        std::thread close_thread([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            close_previous = slot.Close();
        });
        start.store(true, std::memory_order_release);
        take_thread.join();
        close_thread.join();

        const bool close_consumed_ready = close_previous == Slot::State::Ready;
        assert(static_cast<int>(take_succeeded) + static_cast<int>(close_consumed_ready) == 1);
        assert(slot.GetState() == Slot::State::Closed);
    }

    {
        Slot slot;
        auto tap = std::make_shared<FakeTap>();
        std::atomic<int> stored{0};
        std::vector<std::thread> writers;
        for (int i = 0; i < 10; ++i) {
            writers.emplace_back([&, i]() {
                if (slot.StoreOnce(tap, MakePacket(static_cast<unsigned char>(i)), 4)) {
                    stored.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread& writer : writers) {
            writer.join();
        }
        assert(stored.load(std::memory_order_relaxed) == 1);
    }

    std::cout << "delayed SYN slot scenarios passed\n";
    return 0;
}
