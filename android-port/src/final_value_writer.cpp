// Final-field single-value override experiment (host-side, read/write).
// Writes ONE float into the mode-agnostic final owner at a chosen offset
// (default 0xC9C steering) with a 2ms refresh loop, then observes recovery.
// No game code is patched; only /proc/pid/mem is used.
//
// usage: a9tas_final_writer PID LIB_BASE_HEX FIELD_OFFSET_HEX VALUE_FLOAT HOLD_MS
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

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
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

bool FindMapping(const std::vector<Mapping>& maps, std::uintptr_t address,
                 std::size_t size, const Mapping** out) {
    for (const auto& m : maps) {
        if (address >= m.begin && address + size <= m.end) {
            *out = &m;
            return true;
        }
    }
    return false;
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
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX FIELD_OFFSET_HEX VALUE_FLOAT "
                     "HOLD_MS\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto base =
        static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    const auto field_off =
        static_cast<std::uintptr_t>(std::strtoull(argv[3], nullptr, 16));
    const float target_value = std::strtof(argv[4], nullptr);
    const long hold_ms_long = std::strtol(argv[5], nullptr, 10);
    if (pid <= 0 || !base || hold_ms_long <= 0 || hold_ms_long > 10000)
        return 2;
    const auto hold_ms = static_cast<std::uint64_t>(hold_ms_long);

    const std::vector<Mapping> maps = [&] {
        std::vector<Mapping> out;
        if (!ReadMaps(pid, &out)) std::_Exit(3);
        return out;
    }();

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open /proc/pid/mem");
        return 4;
    }

    // Mode-agnostic scan: find an object whose first qword is the inner
    // downstream vtable (lib+0x7EED9E0), then apply the same -0x2D8
    // adjustment used by the vtable+0x2A8 dispatch path.
    const std::uintptr_t inner_vtable_rva = 0x7EED9E0;
    const std::uintptr_t inner_vtable = base + inner_vtable_rva;
    std::uintptr_t inner = 0;
    std::vector<std::uint8_t> buffer(1u << 20);
    std::uint64_t scanned = 0;
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
            scanned += static_cast<std::uint64_t>(got);
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
        std::fprintf(stderr,
                     "inner vtable 0x%" PRIxPTR " not found (scanned=%" PRIu64
                     ")\n",
                     inner_vtable, scanned);
        close(mem);
        return 5;
    }

    std::int64_t adj = 0;
    if (inner_vtable < 0x2D8 ||
        !ReadExact(mem, inner_vtable - 0x2D8, &adj, sizeof(adj))) {
        std::fprintf(stderr, "vtable adjustment read failed\n");
        close(mem);
        return 6;
    }
    const auto final_owner = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(inner) + adj);
    const std::uintptr_t field = final_owner + field_off;
    const Mapping* field_map = nullptr;
    if (!FindMapping(maps, field, sizeof(float), &field_map)) {
        std::fprintf(stderr, "field 0x%" PRIxPTR " not mapped\n", field);
        close(mem);
        return 7;
    }

    float baseline = 0.0F;
    if (!ReadExact(mem, field, &baseline, sizeof(baseline))) {
        std::fprintf(stderr, "baseline read failed\n");
        close(mem);
        return 8;
    }
    std::printf("FINAL_WRITER pid=%d base=0x%" PRIxPTR
                " inner=0x%" PRIxPTR " final_owner=0x%" PRIxPTR
                " field_off=0x%" PRIxPTR " field=0x%" PRIxPTR
                " target=%.9g baseline=%.9g hold_ms=%" PRIu64 "\n",
                static_cast<int>(pid), base, inner, final_owner, field_off,
                field, static_cast<double>(target_value),
                static_cast<double>(baseline), hold_ms);
    std::fflush(stdout);

    // 1s pre-write observation.
    const std::uint64_t start = MonotonicMs();
    while (MonotonicMs() - start < 1000) {
        float value = 0.0F;
        ReadExact(mem, field, &value, sizeof(value));
        std::printf("PRE t_ms=%" PRIu64 " value=%.9g\n",
                    MonotonicMs() - start, static_cast<double>(value));
        std::fflush(stdout);
        usleep(100000);
    }

    // Hold loop: tight continuous refresh, no sleep. The game's own tick
    // writer overwrites this field roughly every 17ms, so the host writer
    // must win the race as often as possible.
    const std::uint64_t hold_start = MonotonicMs();
    std::uint64_t writes = 0;
    while (MonotonicMs() - hold_start < hold_ms) {
        if (!WriteExact(mem, field, &target_value, sizeof(target_value))) {
            std::fprintf(stderr, "write failed after %" PRIu64 " writes\n",
                         writes);
            close(mem);
            return 9;
        }
        ++writes;
    }
    std::printf("HOLD_END t_ms=%" PRIu64 " writes=%" PRIu64
                " target=%.9g\n",
                MonotonicMs() - hold_start, writes,
                static_cast<double>(target_value));
    std::fflush(stdout);

    // 3s post-write observation without any further writes.
    const std::uint64_t post_start = MonotonicMs();
    while (MonotonicMs() - post_start < 3000) {
        float value = 0.0F;
        ReadExact(mem, field, &value, sizeof(value));
        std::printf("POST t_ms=%" PRIu64 " value=%.9g\n",
                    MonotonicMs() - post_start, static_cast<double>(value));
        std::fflush(stdout);
        usleep(100000);
    }
    close(mem);
    std::printf("FINAL_WRITER_DONE pid=%d field=0x%" PRIxPTR "\n",
                static_cast<int>(pid), field);
    return 0;
}
