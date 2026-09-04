// Final-field replayer: reads an A9FRECV1 recording and writes the sampled
// C9C/C98 floats back into the mode-agnostic final owner of the CURRENT
// process with a tight refresh loop. No game code is patched.
//
// usage: a9tas_final_replay PID LIB_BASE_HEX REC_PATH [START_DELAY_MS]
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

bool WriteExact(int fd, std::uintptr_t address, const void* input,
                std::size_t size) {
    const auto* cursor = static_cast<const std::uint8_t*>(input);
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
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: %s PID LIB_BASE_HEX REC_PATH [START_DELAY_MS]\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto base =
        static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    const char* rec_path = argv[3];
    const long start_delay_long =
        argc == 5 ? std::strtol(argv[4], nullptr, 10) : 0;
    if (pid <= 0 || !base || start_delay_long < 0 || start_delay_long > 30000)
        return 2;
    const auto start_delay_ms = static_cast<std::uint64_t>(start_delay_long);

    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return 3;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open /proc/pid/mem");
        return 4;
    }

    // Locate the final owner in THIS process (ASLR-safe).
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

    FILE* rec = std::fopen(rec_path, "rb");
    if (!rec) {
        std::perror("fopen rec");
        close(mem);
        return 7;
    }
    RecHeader header{};
    if (std::fread(&header, sizeof(header), 1, rec) != 1 ||
        std::memcmp(header.magic, kMagic, 8) != 0 ||
        header.count == 0 || header.count > 500000) {
        std::fprintf(stderr, "bad recording header\n");
        std::fclose(rec);
        close(mem);
        return 8;
    }
    std::vector<RecSample> samples(header.count);
    if (std::fread(samples.data(), sizeof(RecSample), header.count, rec) !=
        header.count) {
        std::fprintf(stderr, "short recording\n");
        std::fclose(rec);
        close(mem);
        return 9;
    }
    std::fclose(rec);

    std::printf("FINAL_REPLAY pid=%d base=0x%" PRIxPTR
                " final_owner=0x%" PRIxPTR " samples=%u interval_ms=%u\n",
                static_cast<int>(pid), base, final_owner, header.count,
                header.interval_ms);
    std::fflush(stdout);

    // Optional read-only pre-delay (0 by default for aligned playback).
    const std::uint64_t start = MonotonicMs();
    while (MonotonicMs() - start < start_delay_ms) {
        std::uint32_t c9c = 0, c98 = 0;
        ReadExact(mem, final_owner + 0xC9C, &c9c, sizeof(c9c));
        ReadExact(mem, final_owner + 0xC98, &c98, sizeof(c98));
        std::printf("PRE t_ms=%" PRIu64 " c9c_bits=%08x c98_bits=%08x\n",
                    MonotonicMs() - start, c9c, c98);
        std::fflush(stdout);
        usleep(100000);
    }

    // Playback: hold each sample's two floats with a tight refresh loop.
    const std::uint64_t play_start = MonotonicMs();
    std::uint64_t writes = 0;
    for (std::uint32_t i = 0; i < header.count; ++i) {
        const std::uint64_t sample_end =
            play_start + static_cast<std::uint64_t>(samples[i].t_ms);
        while (MonotonicMs() < sample_end) {
            if (!WriteExact(mem, final_owner + 0xC9C, &samples[i].c9c_bits,
                            sizeof(samples[i].c9c_bits)) ||
                !WriteExact(mem, final_owner + 0xC98, &samples[i].c98_bits,
                            sizeof(samples[i].c98_bits))) {
                std::fprintf(stderr, "write failed at sample %u\n", i);
                close(mem);
                return 10;
            }
            ++writes;
        }
        if ((i % 200) == 0) {
            std::printf("PLAY t_ms=%" PRIu64 " sample=%u writes=%" PRIu64
                        " C9C=%.9g C98=%.9g\n",
                        MonotonicMs() - play_start, i, writes,
                        static_cast<double>(*reinterpret_cast<float*>(
                            const_cast<std::uint32_t*>(&samples[i].c9c_bits))),
                        static_cast<double>(*reinterpret_cast<float*>(
                            const_cast<std::uint32_t*>(&samples[i].c98_bits))));
            std::fflush(stdout);
        }
    }
    std::printf("PLAYBACK_END t_ms=%" PRIu64 " writes=%" PRIu64 "\n",
                MonotonicMs() - play_start, writes);
    std::fflush(stdout);

    // 3s recovery observation, read-only.
    const std::uint64_t post_start = MonotonicMs();
    while (MonotonicMs() - post_start < 3000) {
        std::uint32_t c9c = 0, c98 = 0;
        ReadExact(mem, final_owner + 0xC9C, &c9c, sizeof(c9c));
        ReadExact(mem, final_owner + 0xC98, &c98, sizeof(c98));
        float c9c_f = 0.0F, c98_f = 0.0F;
        std::memcpy(&c9c_f, &c9c, 4);
        std::memcpy(&c98_f, &c98, 4);
        std::printf("POST t_ms=%" PRIu64 " C9C=%.9g C98=%.9g\n",
                    MonotonicMs() - post_start, static_cast<double>(c9c_f),
                    static_cast<double>(c98_f));
        std::fflush(stdout);
        usleep(100000);
    }
    close(mem);
    std::printf("FINAL_REPLAY_DONE pid=%d\n", static_cast<int>(pid));
    return 0;
}
