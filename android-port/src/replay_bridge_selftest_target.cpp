// Offline self-test target for the replay bridge protocol. Simulates the
// guest side of the WAITING/FRAME_READY handshake without the game.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::uint32_t kMagic = 0xA9B2D001;
constexpr std::uint32_t kIdle = 0;
constexpr std::uint32_t kWaiting = 1;
constexpr std::uint32_t kFrameReady = 2;

struct Frame {
    std::uint64_t tick;
    std::uint64_t monotonic_ms;
    std::uint32_t steer_bits;
    std::uint32_t brake_bits;
    std::uint32_t valid;
    std::uint32_t reserved;
};

struct Bridge {
    std::uint32_t magic;
    std::uint32_t state;
    std::uint32_t mode;
    std::uint32_t reserved;
    std::uint64_t waiting_tick;
    std::uint64_t published_seq;
    std::uint64_t published_tick;
    std::uint32_t published_steer_bits;
    std::uint32_t published_brake_bits;
    std::uint32_t published_valid;
    std::uint32_t pad;
    Frame frame;
};
static_assert(offsetof(Bridge, waiting_tick) == 16, "bridge ABI");
static_assert(offsetof(Bridge, frame) == 56, "bridge ABI");
static_assert(sizeof(Bridge) == 88, "bridge ABI");

Bridge g_bridge{};

}  // namespace

int main() {
    g_bridge.magic = kMagic;
    g_bridge.state = kIdle;
    g_bridge.mode = 0;
    FILE* file = std::fopen(
        "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
        "a9tas-replay-bridge.addr", "w");
    if (!file) return 2;
    std::fprintf(file, "bridge=0x%llx\n",
                 static_cast<unsigned long long>(
                     reinterpret_cast<std::uintptr_t>(&g_bridge)));
    std::fclose(file);
    std::printf("bridge_selftest ready bridge=%p\n",
                reinterpret_cast<void*>(&g_bridge));
    std::fflush(stdout);

    std::uint64_t tick = 0;
    while (tick < 2000) {
        g_bridge.published_seq = g_bridge.published_seq + 1;
        g_bridge.published_tick = tick;
        g_bridge.published_steer_bits = static_cast<std::uint32_t>(tick);
        g_bridge.published_brake_bits = static_cast<std::uint32_t>(tick + 1000);
        g_bridge.published_valid = 1;

        if (g_bridge.mode == 1) {
            g_bridge.waiting_tick = tick;
            g_bridge.state = kWaiting;
            while (g_bridge.state != kFrameReady) {
                usleep(100);
            }
            // emulate the guest copying the frame then acking
            volatile Frame copied = g_bridge.frame;
            (void)copied;
            g_bridge.state = kIdle;
        }
        ++tick;
        usleep(16000);  // ~60Hz
    }
    std::printf("bridge_selftest done tick=%llu\n",
                static_cast<unsigned long long>(tick));
    return 0;
}
