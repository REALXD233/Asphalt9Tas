// A9TAS v5 passive final-control write observer.
//
// This tool is deliberately observation-only. It uses x86_64 hardware data
// breakpoints to record writes to the two verified final control fields and
// never writes game data or single-steps game instructions. The resulting
// event stream is evidence for writer order/tid/frequency; it is NOT itself a
// game-tick stream.
//
// usage:
//   a9tas_hwbp_observer_v5 PID LIB_BASE_HEX DURATION_MS OUT_PATH
//   a9tas_hwbp_observer_v5 PID LIB_BASE_HEX DURATION_MS OUT_PATH FINAL_OWNER_HEX

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr char kMagic[8] = {'A', '9', 'H', 'E', 'V', '5', '\0', '\0'};
constexpr std::uint32_t kVersion = 5;
constexpr std::uintptr_t kInnerVtableRva = 0x7EED9E0;
constexpr std::uintptr_t kInnerAdjustSlotDelta = 0x2D8;
constexpr std::uintptr_t kC98Offset = 0xC98;
constexpr std::uintptr_t kC9COffset = 0xC9C;

// Exact bytes from the analyzed CN 600k libAsphalt9.so.  Every address used by
// this observer is build-specific, so a mismatch must stop before ptrace is
// ever armed.  These cover both final setters, the input update entry, and its
// active-registry dispatcher.
struct BuildSignature {
    std::uintptr_t rva;
    const std::uint8_t* bytes;
    std::size_t size;
    const char* name;
};

constexpr std::uint8_t kSetterC98Signature[] = {
    0x08, 0x00, 0x40, 0xF9, 0x29, 0x00, 0x40, 0xB9,
    0x08, 0x61, 0x0B, 0xD1, 0x08, 0x01, 0x40, 0xF9,
    0x08, 0x00, 0x08, 0x8B, 0x09, 0x99, 0x0C, 0xB9,
    0xC0, 0x03, 0x5F, 0xD6,
};
constexpr std::uint8_t kSetterC9CSignature[] = {
    0x08, 0x00, 0x40, 0xF9, 0x29, 0x00, 0x40, 0xB9,
    0x08, 0x81, 0x0B, 0xD1, 0x08, 0x01, 0x40, 0xF9,
    0x08, 0x00, 0x08, 0x8B, 0x09, 0x9D, 0x0C, 0xB9,
    0xC0, 0x03, 0x5F, 0xD6,
};
constexpr std::uint8_t kInputUpdateSignature[] = {
    0xFF, 0x43, 0x01, 0xD1, 0xF4, 0x1B, 0x00, 0xF9,
    0xF3, 0x7B, 0x04, 0xA9, 0xF4, 0x03, 0x01, 0xAA,
    0xF3, 0x03, 0x00, 0xAA, 0xCC, 0xFE, 0xFF, 0x97,
    0x68, 0x2E, 0x40, 0xF9, 0xE8, 0x00, 0x00, 0xB4,
};
constexpr std::uint8_t kActiveDispatchSignature[] = {
    0xFF, 0xC3, 0x00, 0xD1, 0xF5, 0x53, 0x01, 0xA9,
    0xF3, 0x7B, 0x02, 0xA9, 0x08, 0x20, 0x40, 0xB9,
    0xC8, 0x02, 0x00, 0x34, 0x14, 0xD4, 0x40, 0xA9,
    0xF3, 0x03, 0x01, 0xAA, 0xBF, 0x02, 0x14, 0xEB,
};
constexpr BuildSignature kBuildSignatures[] = {
    {0x36934B8, kSetterC98Signature, sizeof(kSetterC98Signature),
     "final C98 setter"},
    {0x36934E0, kSetterC9CSignature, sizeof(kSetterC9CSignature),
     "final C9C setter"},
    {0x386B1E0, kInputUpdateSignature, sizeof(kInputUpdateSignature),
     "input update"},
    {0x37D9860, kActiveDispatchSignature, sizeof(kActiveDispatchSignature),
     "active input dispatcher"},
};

enum HeaderFlags : std::uint32_t {
    kHeaderClean = 1u << 0,
    kOwnerWasExplicit = 1u << 1,
    kTargetSignatureVerified = 1u << 2,
};

enum EventFlags : std::uint32_t {
    kHitC98 = 1u << 0,
    kHitC9C = 1u << 1,
};

#pragma pack(push, 1)
struct TraceHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t final_owner;
    std::uint64_t c98_address;
    std::uint64_t c9c_address;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t c98_hits;
    std::uint64_t c9c_hits;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t thread_additions;
    std::uint64_t unexpected_stops;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    std::uint8_t reserved[24];
};

struct TraceEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::uint32_t c98_bits;
    std::uint32_t c9c_bits;
    std::uint32_t read_ok;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(TraceHeader) == 160, "trace header ABI");
static_assert(sizeof(TraceEvent) == 48, "trace event ABI");

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

struct TracedThread {
    pid_t tid{};
    bool live{};
    bool stopped{};
};

std::uint64_t MonotonicNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, cursor + done, size - done,
                                static_cast<off_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool ReadMaps(pid_t pid, std::vector<Mapping>* maps) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{}, pathbuf[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %*llx %*s %*s %1023[^\n]", &begin, &end,
            perms, pathbuf);
        if (fields < 3) continue;
        Mapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), {}, ""};
        std::memcpy(mapping.perms, perms, 4);
        if (fields == 4) mapping.path = pathbuf;
        while (!mapping.path.empty() && mapping.path.front() == ' ')
            mapping.path.erase(0, 1);
        maps->push_back(mapping);
    }
    std::fclose(file);
    return !maps->empty();
}

const Mapping* FindMapping(const std::vector<Mapping>& maps,
                           std::uintptr_t address, std::size_t size) {
    if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
    const std::uintptr_t end = address + size;
    for (const auto& map : maps) {
        if (address >= map.begin && end <= map.end) return &map;
    }
    return nullptr;
}

bool VerifyTargetBuild(pid_t pid, std::uintptr_t base) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) {
        std::perror("read maps for target verification");
        return false;
    }
    const Mapping* base_map = FindMapping(maps, base, 1);
    if (!base_map || base_map->perms[0] != 'r' ||
        base_map->path.find("libAsphalt9.so") == std::string::npos) {
        std::fprintf(stderr,
                     "library base is not inside a readable libAsphalt9.so mapping\n");
        return false;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem for target verification");
        return false;
    }
    bool ok = true;
    for (const auto& signature : kBuildSignatures) {
        if (base > UINTPTR_MAX - signature.rva) {
            ok = false;
            break;
        }
        const std::uintptr_t address = base + signature.rva;
        const Mapping* mapping = FindMapping(maps, address, signature.size);
        std::vector<std::uint8_t> actual(signature.size);
        if (!mapping || mapping->perms[0] != 'r' ||
            !ReadExact(mem, address, actual.data(), actual.size()) ||
            std::memcmp(actual.data(), signature.bytes, signature.size) != 0) {
            std::fprintf(stderr,
                         "target signature mismatch: %s rva=0x%" PRIxPTR "\n",
                         signature.name, signature.rva);
            ok = false;
        }
    }
    close(mem);
    return ok;
}

bool ValidateOwner(int mem, const std::vector<Mapping>& maps,
                   std::uintptr_t owner, std::uint64_t* pair) {
    if (owner > UINTPTR_MAX - kC9COffset - sizeof(std::uint32_t)) return false;
    const auto* map = FindMapping(maps, owner + kC98Offset,
                                  2 * sizeof(std::uint32_t));
    if (!map || map->perms[0] != 'r' || map->perms[1] != 'w') return false;
    if (!ReadExact(mem, owner + kC98Offset, pair, sizeof(*pair))) return false;
    std::uint32_t low = static_cast<std::uint32_t>(*pair);
    std::uint32_t high = static_cast<std::uint32_t>(*pair >> 32);
    float c98 = 0.0f, c9c = 0.0f;
    std::memcpy(&c98, &low, sizeof(c98));
    std::memcpy(&c9c, &high, sizeof(c9c));
    return std::isfinite(c98) && std::isfinite(c9c) &&
           std::fabs(c98) <= 8.0f && std::fabs(c9c) <= 8.0f;
}

bool ResolveFinalOwner(pid_t pid, std::uintptr_t base,
                       std::uintptr_t explicit_owner,
                       std::uintptr_t* final_owner) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) {
        std::perror("read maps");
        return false;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem for owner resolution");
        return false;
    }

    if (explicit_owner != 0) {
        std::uint64_t pair = 0;
        const bool valid = ValidateOwner(mem, maps, explicit_owner, &pair);
        close(mem);
        if (!valid) {
            std::fprintf(stderr,
                         "explicit final owner failed rw/value validation\n");
            return false;
        }
        *final_owner = explicit_owner;
        return true;
    }

    if (base > UINTPTR_MAX - kInnerVtableRva) {
        close(mem);
        return false;
    }
    const std::uintptr_t inner_vtable = base + kInnerVtableRva;
    const Mapping* vt_map = FindMapping(maps, inner_vtable, sizeof(std::int64_t));
    if (!vt_map || vt_map->perms[0] != 'r' ||
        inner_vtable < kInnerAdjustSlotDelta) {
        std::fprintf(stderr, "inner vtable address is not readable\n");
        close(mem);
        return false;
    }
    std::int64_t adjustment = 0;
    if (!ReadExact(mem, inner_vtable - kInnerAdjustSlotDelta, &adjustment,
                   sizeof(adjustment))) {
        std::fprintf(stderr, "inner adjustment slot read failed\n");
        close(mem);
        return false;
    }

    std::vector<std::uintptr_t> candidates;
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
                if (value != inner_vtable) continue;
                const std::uintptr_t inner = cursor + off;
                const auto signed_inner = static_cast<std::intptr_t>(inner);
                if ((adjustment > 0 &&
                     signed_inner > INTPTR_MAX - adjustment) ||
                    (adjustment < 0 &&
                     signed_inner < INTPTR_MIN - adjustment))
                    continue;
                const auto signed_owner = signed_inner + adjustment;
                if (signed_owner <= 0) continue;
                const auto owner = static_cast<std::uintptr_t>(signed_owner);
                std::uint64_t pair = 0;
                if (ValidateOwner(mem, maps, owner, &pair))
                    candidates.push_back(owner);
            }
            cursor += static_cast<std::uintptr_t>(got);
        }
    }
    close(mem);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) {
        std::fprintf(stderr, "final owner candidates=%zu (required exactly 1)\n",
                     candidates.size());
        for (std::uintptr_t candidate : candidates)
            std::fprintf(stderr, "  candidate=0x%" PRIxPTR "\n", candidate);
        return false;
    }
    *final_owner = candidates.front();
    return true;
}

std::vector<pid_t> ListThreads(pid_t pid) {
    std::vector<pid_t> result;
    const std::string path = "/proc/" + std::to_string(pid) + "/task";
    DIR* dir = opendir(path.c_str());
    if (!dir) return result;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (value > 0 && end != entry->d_name && *end == '\0')
            result.push_back(static_cast<pid_t>(value));
    }
    closedir(dir);
    std::sort(result.begin(), result.end());
    return result;
}

TracedThread* FindThread(std::vector<TracedThread>* threads, pid_t tid) {
    for (auto& thread : *threads) {
        if (thread.tid == tid) return &thread;
    }
    return nullptr;
}

bool PokeDebug(pid_t tid, int index, unsigned long value) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    return ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset),
                  reinterpret_cast<void*>(value)) != -1;
}

bool PeekDebug(pid_t tid, int index, unsigned long* value) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    errno = 0;
    const long result = ptrace(PTRACE_PEEKUSER, tid,
                               reinterpret_cast<void*>(offset), nullptr);
    if (result == -1 && errno != 0) return false;
    *value = static_cast<unsigned long>(result);
    return true;
}

bool StopThread(pid_t tid) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) return false;
    int status = 0;
    return waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
}

bool ContinueThread(pid_t tid, int signal = 0) {
    return ptrace(PTRACE_CONT, tid, nullptr,
                  reinterpret_cast<void*>(static_cast<intptr_t>(signal))) != -1;
}

bool ClearAndDetach(pid_t tid, bool already_stopped) {
    if (!already_stopped && !StopThread(tid)) return false;
    bool ok = true;
    ok = PokeDebug(tid, 7, 0) && ok;
    ok = PokeDebug(tid, 6, 0) && ok;
    ok = PokeDebug(tid, 0, 0) && ok;
    ok = PokeDebug(tid, 1, 0) && ok;
    ok = ptrace(PTRACE_DETACH, tid, nullptr, nullptr) != -1 && ok;
    return ok;
}

unsigned long DualWrite4Dr7() {
    return 1UL | (1UL << 16) | (3UL << 18) |  // DR0 local/write/len4
           (1UL << 2) | (1UL << 20) | (3UL << 22);  // DR1
}

bool AttachOne(pid_t tid, std::uintptr_t c98_address,
               std::uintptr_t c9c_address) {
    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1) return false;
    if (!StopThread(tid)) {
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }
    if (!PokeDebug(tid, 0, c98_address) ||
        !PokeDebug(tid, 1, c9c_address) || !PokeDebug(tid, 6, 0) ||
        !PokeDebug(tid, 7, DualWrite4Dr7())) {
        ClearAndDetach(tid, true);
        return false;
    }
    if (!ContinueThread(tid)) {
        ClearAndDetach(tid, true);
        return false;
    }
    return true;
}

std::size_t AttachNewThreads(pid_t pid, std::uintptr_t c98_address,
                             std::uintptr_t c9c_address,
                             std::vector<TracedThread>* threads,
                             std::uint64_t* failures) {
    std::size_t added = 0;
    for (pid_t tid : ListThreads(pid)) {
        if (FindThread(threads, tid) != nullptr) continue;
        if (!AttachOne(tid, c98_address, c9c_address)) {
            ++*failures;
            continue;
        }
        threads->push_back({tid, true, false});
        ++added;
    }
    return added;
}

bool ParseUnsigned(const char* value, int base, std::uint64_t* out) {
    if (!value || *value == '\0' || *value == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, base);
    if (errno != 0 || end == value || *end != '\0') return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "[FINAL_OWNER_HEX]\n",
                     argv[0]);
        return 2;
    }

    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        (argc == 6 && !ParseUnsigned(argv[5], 16, &owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 || duration_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto explicit_owner = static_cast<std::uintptr_t>(owner_value);
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);

    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr,
                     "unsupported libAsphalt9.so build; no thread was attached\n");
        return 3;
    }

    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base, explicit_owner, &final_owner)) return 3;
    if (final_owner > UINTPTR_MAX - kC9COffset - sizeof(std::uint32_t))
        return 3;
    const std::uintptr_t c98_address = final_owner + kC98Offset;
    const std::uintptr_t c9c_address = final_owner + kC9COffset;
    if ((c98_address & 3u) != 0 || (c9c_address & 3u) != 0) {
        std::fprintf(stderr, "control addresses are not 4-byte aligned\n");
        return 3;
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem for observation");
        return 4;
    }
    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        std::perror("open output");
        close(mem);
        return 5;
    }

    TraceHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(TraceEvent);
    header.flags |= kTargetSignatureVerified;
    if (explicit_owner != 0) header.flags |= kOwnerWasExplicit;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.final_owner = final_owner;
    header.c98_address = c98_address;
    header.c9c_address = c9c_address;
    header.start_ns = MonotonicNs();
    if (header.start_ns == 0 || std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fprintf(stderr, "failed to write trace header\n");
        std::fclose(out);
        close(mem);
        return 6;
    }

    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
    const std::size_t initial_added = AttachNewThreads(
        pid, c98_address, c9c_address, &threads, &attach_failures);
    header.initial_threads = static_cast<std::uint32_t>(initial_added);
    header.ptrace_errors += attach_failures;
    if (initial_added == 0) {
        std::fprintf(stderr, "no threads armed\n");
        std::fclose(out);
        close(mem);
        return 7;
    }

    std::printf("HWBP_OBS_V5 pid=%d base=0x%" PRIxPTR
                " owner=0x%" PRIxPTR " c98=0x%" PRIxPTR
                " c9c=0x%" PRIxPTR " threads=%zu duration_ms=%" PRIu64
                " write_scope=none\n",
                static_cast<int>(pid), base, final_owner, c98_address,
                c9c_address, threads.size(), duration_ms);
    std::fflush(stdout);

    const std::uint64_t deadline_ns =
        header.start_ns + duration_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = header.start_ns + 250000000ULL;
    bool output_ok = true;
    while (MonotonicNs() < deadline_ns) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, c98_address, c9c_address, &threads, &failures);
            header.thread_additions += added;
            header.ptrace_errors += failures;
            next_rescan_ns = now_ns + 250000000ULL;
        }

        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno != ECHILD) ++header.ptrace_errors;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        TracedThread* tracked = FindThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked) {
                tracked->live = false;
                tracked->stopped = false;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        if (signal == SIGTRAP && have_dr6 && (dr6 & 3UL) != 0) {
            TraceEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if ((dr6 & 1UL) != 0) {
                event.flags |= kHitC98;
                ++header.c98_hits;
            }
            if ((dr6 & 2UL) != 0) {
                event.flags |= kHitC9C;
                ++header.c9c_hits;
            }
            user_regs_struct regs{};
            if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1) {
                ++header.ptrace_errors;
            } else {
                event.rip = static_cast<std::uint64_t>(regs.rip);
            }
            std::uint64_t pair = 0;
            if (ReadExact(mem, c98_address, &pair, sizeof(pair))) {
                event.c98_bits = static_cast<std::uint32_t>(pair);
                event.c9c_bits = static_cast<std::uint32_t>(pair >> 32);
                event.read_ok = 1;
            } else {
                ++header.read_errors;
            }
            if (std::fwrite(&event, sizeof(event), 1, out) != 1) {
                output_ok = false;
                break;
            }
            ++header.event_count;
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
                ++header.ptrace_errors;
            } else if (tracked) {
                tracked->stopped = false;
            }
        } else {
            ++header.unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver)) {
                ++header.ptrace_errors;
            } else if (tracked) {
                tracked->stopped = false;
            }
        }
    }

    std::uint32_t live_count = 0;
    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped)) {
            ++header.ptrace_errors;
        } else {
            thread.live = false;
            thread.stopped = false;
        }
        ++live_count;
    }
    header.final_threads = live_count;
    if (output_ok && header.event_count > 0 && header.read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0)
        header.flags |= kHeaderClean;

    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);

    std::printf("HWBP_OBS_V5_DONE events=%" PRIu64 " c98=%" PRIu64
                " c9c=%" PRIu64 " read_errors=%" PRIu64
                " ptrace_errors=%" PRIu64 " thread_additions=%" PRIu64
                " unexpected_stops=%" PRIu64 " clean=%u path=%s\n",
                header.event_count, header.c98_hits, header.c9c_hits,
                header.read_errors, header.ptrace_errors,
                header.thread_additions, header.unexpected_stops,
                (header.flags & kHeaderClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
