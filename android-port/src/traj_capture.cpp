// Trajectory sampler: capture racer transform/velocity during a replay run
// for consistency verification against the recording's result state.
//
// usage: a9tas_traj_capture PID TRANSFORM_ADDR_HEX [VELOCITY_ADDR_HEX] DURATION_MS OUT_PATH
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kMagic = "A9TRAJV1";

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n =
            pread(fd, cursor + done, size - done, static_cast<off_t>(address + done));
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
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: %s PID TRANSFORM_ADDR_HEX [VELOCITY_ADDR_HEX] "
                     "DURATION_MS OUT_PATH\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto transform_addr =
        static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    std::uintptr_t velocity_addr = 0;
    const char* duration_arg = nullptr;
    const char* out_path = nullptr;
    if (argc == 5) {
        duration_arg = argv[3];
        out_path = argv[4];
    } else {
        velocity_addr = std::strtoull(argv[3], nullptr, 16);
        duration_arg = argv[4];
        out_path = argv[5];
    }
    const long dur = std::strtol(duration_arg, nullptr, 10);
    if (pid <= 0 || !transform_addr || dur <= 0 || dur > 300000) return 2;
    const auto duration_ms = static_cast<std::uint64_t>(dur);

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem");
        return 3;
    }
    FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::perror("fopen output");
        close(mem);
        return 4;
    }
    struct Header {
        char magic[8];
        std::uint64_t transform_addr;
        std::uint64_t velocity_addr;
        std::uint64_t start_ms;
        std::uint64_t count;
        std::uint32_t interval_ms;
        std::uint32_t reserved;
    } header{};
    std::memcpy(header.magic, kMagic, 8);
    header.transform_addr = transform_addr;
    header.velocity_addr = velocity_addr;
    header.start_ms = MonotonicMs();
    header.interval_ms = 50;
    std::fwrite(&header, sizeof(header), 1, out);

    std::printf("TRAJ pid=%d transform=0x%" PRIxPTR " velocity=0x%" PRIxPTR
                " duration_ms=%" PRIu64 "\n",
                static_cast<int>(pid), transform_addr, velocity_addr,
                duration_ms);
    std::fflush(stdout);

    const std::uint64_t start = MonotonicMs();
    std::uint64_t count = 0;
    while (MonotonicMs() - start < duration_ms) {
        float transform[16]{};
        float velocity[3]{};
        const std::uint64_t t_ms = MonotonicMs() - start;
        if (ReadExact(mem, transform_addr, transform, sizeof(transform))) {
            if (velocity_addr)
                ReadExact(mem, velocity_addr, velocity, sizeof(velocity));
            if (std::fwrite(&t_ms, sizeof(t_ms), 1, out) != 1 ||
                std::fwrite(transform, sizeof(transform), 1, out) != 1 ||
                std::fwrite(velocity, sizeof(velocity), 1, out) != 1)
                break;
            ++count;
        }
        usleep(48000);
    }
    std::fseek(out, 0, SEEK_SET);
    header.count = count;
    std::fwrite(&header, sizeof(header), 1, out);
    std::fclose(out);
    close(mem);
    std::printf("TRAJ_DONE samples=%" PRIu64 " path=%s\n", count, out_path);
    return 0;
}
