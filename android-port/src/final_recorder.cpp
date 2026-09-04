// Final-field recorder: samples C9C/C98 of the mode-agnostic final owner at
// a fixed 5ms cadence into a binary file. Read-only with respect to control
// values (it never writes them).
//
// usage: a9tas_final_recorder PID LIB_BASE_HEX DURATION_MS OUT_PATH
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kMagic = "A9FRECV1";

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

struct RecHeader {
    char magic[8];
    std::uint64_t base;
    std::uint64_t inner;
    std::uint64_t final_owner;
    std::uint64_t monotonic_start_ms;
    std::uint32_t interval_ms;
    std::uint32_t count;
    std::uint32_t c9c_off;
    std::uint32_t c98_off;
    std::uint64_t reserved[4];
};

struct RecSample {
    std::uint64_t t_ms;
    std::uint32_t c9c_bits;
    std::uint32_t c98_bits;
};

bool ReadMaps(pid_t pid, std::vector<Mapping>* maps) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{}, pathbuf[1024]{};
        const int fields =
            std::sscanf(line, "%llx-%llx %4s %*llx %*s %*s %1023[^\n]",
                        &begin, &end, perms, pathbuf);
        Mapping m{static_cast<std::uintptr_t>(begin),
                  static_cast<std::uintptr_t>(end), {}, ""};
        std::memcpy(m.perms, perms, 4);
        if (fields == 5) m.path = pathbuf;
        while (!m.path.empty() && m.path.front() == ' ') m.path.erase(0, 1);
        maps->push_back(m);
    }
    std::fclose(file);
    return !maps->empty();
}

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
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto base =
        static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    const long duration_long = std::strtol(argv[3], nullptr, 10);
    const char* out_path = argv[4];
    if (pid <= 0 || !base || duration_long <= 0 || duration_long > 120000)
        return 2;
    const auto duration_ms = static_cast<std::uint64_t>(duration_long);

    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return 3;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open /proc/pid/mem");
        return 4;
    }

    const std::uintptr_t inner_vtable = base + 0x7EED9E0;
    std::uintptr_t inner = 0;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const auto& map : maps) {
        if (map.perms[0] != 'r' || map.perms[1] != 'w') continue;
        if (map.path == "[vvar]" || map.path == "[vdso]") continue;
        for (std::uintptr_t cursor = map.begin; cursor < map.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buffer.size(), map.end - cursor));
            const ssize_t got = pread(mem, buffer.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                cursor += want;
                continue;
            }
            for (std::size_t off = 0;
                 off + sizeof(std::uintptr_t) <= static_cast<std::size_t>(got);
                 off += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + off, sizeof(value));
                if (value == inner_vtable) {
                    inner = cursor + off;
                    break;
                }
            }
            if (inner) break;
            cursor += got;
        }
        if (inner) break;
    }
    if (!inner) {
        std::fprintf(stderr, "inner vtable not found\n");
        close(mem);
        return 5;
    }
    std::int64_t adj = 0;
    if (inner_vtable < 0x2D8 ||
        !ReadExact(mem, inner_vtable - 0x2D8, &adj, sizeof(adj))) {
        std::fprintf(stderr, "adjustment read failed\n");
        close(mem);
        return 6;
    }
    const auto final_owner = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(inner) + adj);

    FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::perror("fopen output");
        close(mem);
        return 7;
    }
    RecHeader header{};
    std::memcpy(header.magic, kMagic, 8);
    header.base = base;
    header.inner = inner;
    header.final_owner = final_owner;
    header.monotonic_start_ms = MonotonicMs();
    header.interval_ms = 5;
    header.count = static_cast<std::uint32_t>(duration_ms / 5);
    header.c9c_off = 0xC9C;
    header.c98_off = 0xC98;
    if (std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::perror("header write");
        std::fclose(out);
        close(mem);
        return 8;
    }

    std::printf("FINAL_RECORDER pid=%d base=0x%" PRIxPTR
                " inner=0x%" PRIxPTR " final_owner=0x%" PRIxPTR
                " duration_ms=%" PRIu64 " samples=%u interval_ms=5\n",
                static_cast<int>(pid), base, inner, final_owner, duration_ms,
                header.count);
    std::fflush(stdout);

    const std::uint64_t start = MonotonicMs();
    for (std::uint32_t i = 0; i < header.count; ++i) {
        const std::uint64_t target_ms = start + i * 5ULL;
        for (;;) {
            const std::uint64_t now = MonotonicMs();
            if (now >= target_ms) break;
            usleep(1000);
        }
        RecSample sample{};
        sample.t_ms = MonotonicMs() - start;
        std::uint32_t c9c = 0, c98 = 0;
        ReadExact(mem, final_owner + 0xC9C, &c9c, sizeof(c9c));
        ReadExact(mem, final_owner + 0xC98, &c98, sizeof(c98));
        sample.c9c_bits = c9c;
        sample.c98_bits = c98;
        if (std::fwrite(&sample, sizeof(sample), 1, out) != 1) {
            std::fprintf(stderr, "sample write failed at %u\n", i);
            break;
        }
        if ((i % 200) == 0) {
            float c9c_f = 0.0F, c98_f = 0.0F;
            std::memcpy(&c9c_f, &c9c, 4);
            std::memcpy(&c98_f, &c98, 4);
            std::printf("REC t_ms=%" PRIu64 " C9C=%.9g C98=%.9g\n",
                        sample.t_ms, static_cast<double>(c9c_f),
                        static_cast<double>(c98_f));
            std::fflush(stdout);
        }
    }
    std::fflush(out);
    std::fclose(out);
    close(mem);
    std::printf("FINAL_RECORDER_DONE path=%s\n", out_path);
    return 0;
}
