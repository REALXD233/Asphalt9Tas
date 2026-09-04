// Blocking replay player for the replay-block payload.
// The guest blocks at UpdatePerTick in WAITING state; this tool provides one
// frame per tick through the bridge and acks it. No manual start timing.
//
// usage: a9tas_replay_bridge_player PID REC_PATH
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kBridgeFile =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-replay-bridge.addr";
constexpr std::uint32_t kMagic = 0xA9B2D001;
constexpr const char* kRecMagic = "A9RBFREC";
constexpr std::uint32_t kWaiting = 1;
constexpr std::uint32_t kFrameReady = 2;
constexpr std::uint32_t kIdle = 0;

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
static_assert(sizeof(Frame) == 32, "frame ABI");

struct RecHeader {
    char magic[8];
    std::uint32_t count;
    std::uint32_t frame_size;
    std::uint64_t reserved[4];
};

std::uintptr_t ReadBridgeAddress() {
    FILE* file = std::fopen(kBridgeFile, "re");
    if (!file) return 0;
    char line[128]{};
    std::uintptr_t addr = 0;
    if (std::fgets(line, sizeof(line), file)) {
        if (std::sscanf(line, "bridge=0x%" PRIxPTR, &addr) != 1) addr = 0;
    }
    std::fclose(file);
    return addr;
}

bool PreadExact(int fd, std::uintptr_t address, void* out, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(out);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n =
            pread(fd, cursor + done, size - done, static_cast<off_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool PwriteExact(int fd, std::uintptr_t address, const void* in,
                 std::size_t size) {
    const auto* cursor = static_cast<const std::uint8_t*>(in);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n =
            pwrite(fd, cursor + done, size - done, static_cast<off_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::uint64_t MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PID REC_PATH\n", argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const char* rec_path = argv[2];
    if (pid <= 0) return 2;

    FILE* rec = std::fopen(rec_path, "rb");
    if (!rec) {
        std::perror("fopen rec");
        return 3;
    }
    RecHeader header{};
    if (std::fread(&header, sizeof(header), 1, rec) != 1 ||
        std::memcmp(header.magic, kRecMagic, 8) != 0 ||
        header.count == 0 || header.count > 1000000 ||
        header.frame_size < sizeof(Frame)) {
        std::fprintf(stderr, "bad recording header\n");
        std::fclose(rec);
        return 4;
    }
    std::vector<Frame> frames(header.count);
    for (std::uint32_t i = 0; i < header.count; ++i) {
        if (std::fread(&frames[i], header.frame_size, 1, rec) != 1) {
            std::fprintf(stderr, "short recording at frame %u\n", i);
            std::fclose(rec);
            return 5;
        }
    }
    std::fclose(rec);

    const std::uintptr_t bridge_addr = ReadBridgeAddress();
    if (!bridge_addr) {
        std::fprintf(stderr, "bridge address file missing\n");
        return 6;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem");
        return 7;
    }

    Bridge bridge{};
    if (!PreadExact(mem, bridge_addr, &bridge, sizeof(bridge)) ||
        bridge.magic != kMagic) {
        std::fprintf(stderr, "bad bridge\n");
        close(mem);
        return 8;
    }

    // Enable replay mode; guest will block on the next UpdatePerTick.
    std::uint32_t mode = 1;
    PwriteExact(mem, bridge_addr + offsetof(Bridge, mode), &mode,
                sizeof(mode));
    std::printf("PLAYER bridge=0x%" PRIxPTR " frames=%u mode=replay\n",
                bridge_addr, header.count);
    std::fflush(stdout);

    const std::uint64_t start = MonotonicMs();
    std::uint64_t served = 0;
    while (served < frames.size() && MonotonicMs() - start < 120000) {
        Bridge cur{};
        if (!PreadExact(mem, bridge_addr, &cur, sizeof(cur))) {
            std::fprintf(stderr, "bridge read failed\n");
            break;
        }
        if (cur.state != kWaiting) {
            usleep(200);
            continue;
        }
        // Find frame for this tick; tolerate a small offset when the
        // recording itself began on a later tick.
        const Frame* frame = nullptr;
        for (std::size_t i = served; i < frames.size(); ++i) {
            if (frames[i].tick == cur.waiting_tick) {
                frame = &frames[i];
                served = i + 1;
                break;
            }
        }
        if (!frame) {
            // No frame for this tick: reuse the previous/next closest frame.
            if (served < frames.size()) {
                frame = &frames[served];
                ++served;
            } else {
                frame = &frames.back();
                ++served;
            }
        }
        Frame packet = *frame;
        packet.tick = cur.waiting_tick;
        packet.monotonic_ms = MonotonicMs();
        if (!PwriteExact(mem, bridge_addr + offsetof(Bridge, frame), &packet,
                         sizeof(packet))) {
            std::fprintf(stderr, "frame write failed\n");
            break;
        }
        std::uint32_t ready = kFrameReady;
        if (!PwriteExact(mem, bridge_addr + offsetof(Bridge, state), &ready,
                         sizeof(ready))) {
            std::fprintf(stderr, "state write failed\n");
            break;
        }
        // Wait for the guest to copy the frame and ack back to idle.
        const std::uint64_t ack_deadline = MonotonicMs() + 3000;
        bool acked = false;
        while (MonotonicMs() < ack_deadline) {
            std::uint32_t state = 0;
            PreadExact(mem, bridge_addr + offsetof(Bridge, state), &state,
                       sizeof(state));
            if (state == kIdle) {
                acked = true;
                break;
            }
            usleep(100);
        }
        if (!acked) {
            std::fprintf(stderr, "guest ack timeout at tick=%" PRIu64 "\n",
                         cur.waiting_tick);
            break;
        }
        if ((served % 60) == 0) {
            std::printf("PLAY served=%" PRIu64 " tick=%" PRIu64
                        " steer=%08x brake=%08x\n",
                        served, cur.waiting_tick, packet.steer_bits,
                        packet.brake_bits);
            std::fflush(stdout);
        }
    }
    mode = 0;
    PwriteExact(mem, bridge_addr + offsetof(Bridge, mode), &mode,
                sizeof(mode));
    close(mem);
    std::printf("PLAYER_DONE served=%" PRIu64 " frames=%zu\n", served,
                frames.size());
    return served > 0 ? 0 : 1;
}
