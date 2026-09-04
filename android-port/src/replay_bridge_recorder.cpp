// Bridge recorder for the replay-block payload.
// Polls the guest bridge published sequence and writes per-tick frames.
//
// usage: a9tas_replay_bridge_recorder PID OUT_PATH MAX_FRAMES
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* kBridgeFile =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-replay-bridge.addr";
constexpr std::uint32_t kMagic = 0xA9B2D001;
constexpr const char* kRecMagic = "A9RBFREC";

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
    struct FramePlaceholder {
        std::uint64_t tick;
        std::uint64_t monotonic_ms;
        std::uint32_t steer_bits;
        std::uint32_t brake_bits;
        std::uint32_t valid;
        std::uint32_t reserved;
    } frame;
};

struct Frame {
    std::uint64_t tick;
    std::uint64_t monotonic_ms;
    std::uint32_t steer_bits;
    std::uint32_t brake_bits;
    std::uint32_t valid;
    std::uint32_t reserved;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s PID OUT_PATH MAX_FRAMES\n", argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const char* out_path = argv[2];
    const long max_frames_long = std::strtol(argv[3], nullptr, 10);
    if (pid <= 0 || max_frames_long <= 0 || max_frames_long > 1000000)
        return 2;
    const std::uint32_t max_frames =
        static_cast<std::uint32_t>(max_frames_long);

    const std::uintptr_t bridge_addr = ReadBridgeAddress();
    if (!bridge_addr) {
        std::fprintf(stderr, "bridge address file missing\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem");
        return 4;
    }

    // Set record mode explicitly and clear any stale replay state.
    std::uint32_t mode = 0;
    PwriteExact(mem, bridge_addr + offsetof(Bridge, mode), &mode,
                sizeof(mode));

    FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::perror("fopen output");
        close(mem);
        return 5;
    }
    RecHeader header{};
    std::memcpy(header.magic, kRecMagic, 8);
    header.count = 0;
    header.frame_size = sizeof(Frame);
    if (std::fwrite(&header, sizeof(header), 1, out) != 1) {
        close(mem);
        std::fclose(out);
        return 6;
    }

    Bridge bridge{};
    if (!PreadExact(mem, bridge_addr, &bridge, offsetof(Bridge, frame)) ||
        bridge.magic != kMagic) {
        std::fprintf(stderr, "bad bridge magic\n");
        close(mem);
        std::fclose(out);
        return 7;
    }
    std::uint64_t last_seq = bridge.published_seq;
    std::uint32_t count = 0;
    std::printf("RECORDER bridge=0x%" PRIxPTR " last_seq=%" PRIu64
                " max_frames=%u\n",
                bridge_addr, last_seq, max_frames);
    std::fflush(stdout);

    while (count < max_frames) {
        Bridge cur{};
        if (!PreadExact(mem, bridge_addr, &cur, offsetof(Bridge, frame)) ||
            cur.magic != kMagic) {
            std::fprintf(stderr, "bridge read failed\n");
            break;
        }
        if (cur.published_seq != last_seq && cur.published_valid) {
            Frame frame{};
            frame.tick = cur.published_tick;
            frame.monotonic_ms = 0;
            frame.steer_bits = cur.published_steer_bits;
            frame.brake_bits = cur.published_brake_bits;
            frame.valid = 1;
            if (std::fwrite(&frame, sizeof(frame), 1, out) != 1) {
                std::fprintf(stderr, "frame write failed\n");
                break;
            }
            ++count;
            last_seq = cur.published_seq;
            if ((count % 60) == 0) {
                std::printf("REC frames=%u tick=%" PRIu64 " seq=%" PRIu64 "\n",
                            count, frame.tick, last_seq);
                std::fflush(stdout);
            }
        }
        usleep(500);
    }

    std::fseek(out, 0, SEEK_SET);
    header.count = count;
    std::fwrite(&header, sizeof(header), 1, out);
    std::fflush(out);
    std::fclose(out);
    close(mem);
    std::printf("RECORDER_DONE frames=%u path=%s\n", count, out_path);
    return 0;
}
