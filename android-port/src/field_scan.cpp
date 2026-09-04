// Field scan around the final owner: find the race tick counter.
//
// Samples final_owner +- 0x800 (4KB) at 50ms cadence into a raw log:
//   header: magic "A9FSCN1", base, final_owner, start_ms, count, interval_ms
//   samples: t_ms(u64) + 4096 bytes
// Post-analyze: find a u32 that increments ~once per 17ms during the race
// (and stays constant in the menu/lobby segment).
//
// usage: a9tas_field_scan PID LIB_BASE_HEX DURATION_MS OUT_PATH
#include <cinttypes>
#include <cstddef>
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

constexpr const char* kMagic = "A9FSCN1";
constexpr std::size_t kWindow = 0x20000;  // +-64KB around the owner start

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

bool ResolveFinalOwner(pid_t pid, std::uintptr_t base,
                       std::uintptr_t* final_owner) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;

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
        close(mem);
        return false;
    }
    std::int64_t adj = 0;
    if (inner_vtable < 0x2D8 ||
        !ReadExact(mem, inner_vtable - 0x2D8, &adj, sizeof(adj))) {
        close(mem);
        return false;
    }
    *final_owner = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(inner) + adj);
    close(mem);
    return true;
}

// Locate the GameplayInputController object: scan writable memory for a
// pointer equal to the controller vtable (lib+0x80c4da8 per kb scan).
bool ResolveController(pid_t pid, std::uintptr_t base,
                       std::uintptr_t* controller) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;

    const std::uintptr_t controller_vtable = base + 0x80C4DA8;
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
                if (value == controller_vtable) {
                    *controller = cursor + off;
                    close(mem);
                    return true;
                }
            }
            cursor += got;
        }
    }
    close(mem);
    return false;
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
    const auto base = static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 16));
    const long dur = std::strtol(argv[3], nullptr, 10);
    const char* out_path = argv[4];
    if (pid <= 0 || !base || dur <= 0 || dur > 300000) return 2;
    const auto duration_ms = static_cast<std::uint64_t>(dur);

    std::uintptr_t final_owner = 0;
    std::uintptr_t controller = 0;
    if (!ResolveFinalOwner(pid, base, &final_owner)) {
        std::fprintf(stderr, "final owner resolution failed\n");
        return 3;
    }
    ResolveController(pid, base, &controller);
    const std::uintptr_t start_addr = final_owner - 0x10000;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem");
        return 4;
    }

    FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::perror("fopen output");
        close(mem);
        return 5;
    }
    struct Header {
        char magic[8];
        std::uint64_t base;
        std::uint64_t final_owner;
        std::uint64_t start_addr;
        std::uint64_t start_ms;
        std::uint64_t count;
        std::uint32_t interval_ms;
        std::uint32_t window;
        std::uint64_t controller;
    } header{};
    std::memcpy(header.magic, kMagic, 8);
    header.base = base;
    header.final_owner = final_owner;
    header.start_addr = start_addr;
    header.start_ms = MonotonicMs();
    header.count = 0;
    header.interval_ms = 200;
    header.window = kWindow;
    header.controller = controller;
    if (std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    std::printf("FIELD_SCAN pid=%d base=0x%" PRIxPTR " owner=0x%" PRIxPTR
                " controller=0x%" PRIxPTR " window=%zu duration_ms=%" PRIu64
                "\n",
                static_cast<int>(pid), base, final_owner, controller, kWindow,
                duration_ms);
    std::fflush(stdout);

    const std::uint64_t start = MonotonicMs();
    std::uint64_t count = 0;
    std::vector<std::uint8_t> window(kWindow);
    std::vector<std::uint8_t> ctrl_window(controller ? kWindow : 0);
    while (MonotonicMs() - start < duration_ms) {
        if (!ReadExact(mem, start_addr, window.data(), kWindow)) {
            std::fprintf(stderr, "window read failed at %" PRIu64 "\n", count);
            break;
        }
        const std::uint64_t t_ms = MonotonicMs() - start;
        if (std::fwrite(&t_ms, sizeof(t_ms), 1, out) != 1 ||
            std::fwrite(window.data(), 1, kWindow, out) != kWindow) {
            std::fprintf(stderr, "sample write failed at %" PRIu64 "\n", count);
            break;
        }
        if (controller &&
            (!ReadExact(mem, controller - 0x400, ctrl_window.data(),
                        kWindow) ||
             std::fwrite(&t_ms, sizeof(t_ms), 1, out) != 1 ||
             std::fwrite(ctrl_window.data(), 1, kWindow, out) != kWindow)) {
            std::fprintf(stderr, "controller sample write failed at %" PRIu64
                                 "\n",
                         count);
            break;
        }
        ++count;
        if ((count % 100) == 0) {
            std::printf("SCAN t_ms=%" PRIu64 " samples=%" PRIu64 "\n", t_ms,
                        count);
            std::fflush(stdout);
        }
        usleep(200000 - 5000);
    }
    std::fseek(out, 0, SEEK_SET);
    header.count = count;
    std::fwrite(&header, sizeof(header), 1, out);
    std::fclose(out);
    close(mem);
    std::printf("FIELD_SCAN_DONE samples=%" PRIu64 " path=%s\n", count,
                out_path);
    return 0;
}
