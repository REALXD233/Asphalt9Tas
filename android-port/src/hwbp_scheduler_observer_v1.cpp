// A9TAS passive main-loop/physics-boundary observer.
//
// This executable is intentionally observation-only. It resolves the main
// loop object by its build-specific vtable and arms x86_64 hardware *data*
// breakpoints on:
//   object + 0x150  accumulated physics time (8-byte writes)
//   final owner + 0xC98 / +0xC9C (4-byte final-control writes)
//   optional caller-supplied 4-byte state field (DR3)
// It never writes guest game memory and never patches/single-steps ARM code.
//
// usage:
//   a9tas_hwbp_scheduler_observer_v1 PID LIB_BASE_HEX DURATION_MS OUT_PATH
//   a9tas_hwbp_scheduler_observer_v1 PID LIB_BASE_HEX DURATION_MS OUT_PATH MAIN_OBJECT_HEX
//   a9tas_hwbp_scheduler_observer_v1 PID LIB_BASE_HEX DURATION_MS OUT_PATH MAIN_OBJECT_HEX FINAL_OWNER_HEX WATCH_ADDRESS_HEX


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

constexpr char kMagic[8] = {'A', '9', 'H', 'S', 'B', '2', '\0', '\0'};
constexpr std::uint32_t kVersion = 2;
constexpr std::uintptr_t kMainVtableRva = 0x7F3C1A8;
constexpr std::uintptr_t kEmbeddedTimeSourceVtableRva = 0x7F3C400;
constexpr std::uintptr_t kInnerVtableRva = 0x7EED9E0;
constexpr std::uintptr_t kInnerAdjustSlotDelta = 0x2D8;
constexpr std::uintptr_t kEmbeddedTimeSourceOffset = 0xB0;
constexpr std::uintptr_t kTimeScaleManagerOffset = 0xF8;
constexpr std::uintptr_t kAccumulatorOffset = 0x150;
constexpr std::uintptr_t kEnabledOffset = 0x158;
constexpr std::uintptr_t kPhaseSchedulerOffset = 0x178;
constexpr std::uintptr_t kTimeScaleValueOffset = 0x2D8;
constexpr std::uintptr_t kC98Offset = 0xC98;
constexpr std::uintptr_t kC9COffset = 0xC9C;
constexpr std::size_t kValidatedObjectSize = 0x1C50;

struct BuildSignature {
    std::uintptr_t rva;
    const std::uint8_t* bytes;
    std::size_t size;
    const char* name;
};

constexpr std::uint8_t kPhysicsTimeCallbackSignature[] = {
    0xFF, 0xC3, 0x00, 0xD1, 0xF4, 0x0B, 0x00, 0xF9,
    0xF3, 0x7B, 0x02, 0xA9, 0xF4, 0x03, 0x00, 0xAA,
    0x00, 0xF4, 0x41, 0xF9, 0xF3, 0x03, 0x02, 0xAA,
    0x40, 0x00, 0x00, 0xB4, 0xF3, 0x94, 0x36, 0x94,
};
constexpr std::uint8_t kInputPhaseSignature[] = {
    0xFF, 0x43, 0x03, 0xD1, 0xF7, 0x5B, 0x0A, 0xA9,
    0xF5, 0x53, 0x0B, 0xA9, 0xF3, 0x7B, 0x0C, 0xA9,
    0x0C, 0x66, 0x00, 0x94, 0xE0, 0xE3, 0x00, 0x91,
    0xE8, 0x4F, 0x00, 0xF9, 0x4A, 0xC9, 0x4D, 0x94,
};
constexpr std::uint8_t kPhysicsChunkerSignature[] = {
    0xFF, 0x43, 0x01, 0xD1, 0xF7, 0x5B, 0x02, 0xA9,
    0xF5, 0x53, 0x03, 0xA9, 0xF3, 0x7B, 0x04, 0xA9,
    0x68, 0x6D, 0x03, 0xD0, 0x08, 0x61, 0x1E, 0x91,
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
constexpr BuildSignature kBuildSignatures[] = {
    {0x3791618, kPhysicsTimeCallbackSignature,
     sizeof(kPhysicsTimeCallbackSignature), "physics time callback"},
    {0x37918D0, kInputPhaseSignature, sizeof(kInputPhaseSignature),
     "input phase callback"},
    {0x37949C8, kPhysicsChunkerSignature, sizeof(kPhysicsChunkerSignature),
     "physics time chunker"},
    {0x36934B8, kSetterC98Signature, sizeof(kSetterC98Signature),
     "final C98 setter"},
    {0x36934E0, kSetterC9CSignature, sizeof(kSetterC9CSignature),
     "final C9C setter"},
};

enum HeaderFlags : std::uint32_t {
    kHeaderClean = 1u << 0,
    kObjectWasExplicit = 1u << 1,
    kTargetSignatureVerified = 1u << 2,
    kFinalOwnerWasExplicit = 1u << 3,
    kStateWatchEnabled = 1u << 4,
};

enum EventFlags : std::uint32_t {
    kHitAccumulator = 1u << 0,
    kHitC98 = 1u << 1,
    kHitC9C = 1u << 2,
    kHitStateWatch = 1u << 3,
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
    std::uint64_t main_object;
    std::uint64_t accumulator_address;
    std::uint64_t final_owner;
    std::uint64_t c98_address;
    std::uint64_t c9c_address;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t accumulator_hits;
    std::uint64_t c98_hits;
    std::uint64_t c9c_hits;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t thread_additions;
    std::uint64_t unexpected_stops;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    std::uint64_t state_watch_address;
    std::uint64_t state_watch_hits;
};

struct TraceEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::int64_t accumulator;
    std::uint32_t enabled;
    std::uint32_t time_scale_bits;
    std::uint32_t c98_bits;
    std::uint32_t c9c_bits;
    std::uint32_t read_ok;
    std::uint32_t state_watch_bits;
    // Version 2 context is captured only for DR3 state-watch hits. It lets a
    // later offline pass recover the host worker/task path without injecting
    // code or mutating guest/native game state.
    std::uint64_t rsp;
    std::uint64_t rbp;
    std::uint64_t gpr[14];  // rax,rbx,rcx,rdx,rsi,rdi,r8..r15
    std::uint32_t stack_read_ok;
    std::uint32_t reserved;
    std::uint64_t stack_words[32];
};
#pragma pack(pop)

static_assert(sizeof(TraceHeader) == 176, "trace header ABI");
static_assert(sizeof(TraceEvent) == 456, "trace event ABI");

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
        std::fprintf(stderr,
                     "cannot parse /proc/%d/maps while verifying base=0x%" PRIxPTR
                     "\n",
                     static_cast<int>(pid), base);
        return false;
    }
    const Mapping* base_map = FindMapping(maps, base, 1);
    if (!base_map || base_map->perms[0] != 'r' ||
        base_map->path.find("libAsphalt9.so") == std::string::npos) {
        std::fprintf(stderr,
                     "base=0x%" PRIxPTR
                     " is not in readable libAsphalt9.so mapping; parsed_maps=%zu\n",
                     base, maps.size());
        for (const auto& map : maps) {
            if (map.path.find("libAsphalt9.so") == std::string::npos) continue;
            std::fprintf(stderr,
                         "parsed libAsphalt9 mapping=0x%" PRIxPTR
                         "-0x%" PRIxPTR " perms=%s path=%s\n",
                         map.begin, map.end, map.perms, map.path.c_str());
        }
        return false;
    }
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
    const int mem = open(path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;
    bool ok = true;
    for (const auto& signature : kBuildSignatures) {
        if (base > UINTPTR_MAX - signature.rva) {
            ok = false;
            break;
        }
        const std::uintptr_t address = base + signature.rva;
        std::vector<std::uint8_t> actual(signature.size);
        const Mapping* map = FindMapping(maps, address, signature.size);
        if (!map || map->perms[0] != 'r' ||
            !ReadExact(mem, address, actual.data(), actual.size()) ||
            std::memcmp(actual.data(), signature.bytes, signature.size) != 0) {
            std::fprintf(stderr, "signature mismatch: %s rva=0x%" PRIxPTR "\n",
                         signature.name, signature.rva);
            ok = false;
        }
    }
    close(mem);
    return ok;
}

bool ValidateMainObject(int mem, const std::vector<Mapping>& maps,
                        std::uintptr_t base, std::uintptr_t object) {
    if (object > UINTPTR_MAX - kValidatedObjectSize) return false;
    const Mapping* object_map = FindMapping(maps, object, kValidatedObjectSize);
    if (!object_map || object_map->perms[0] != 'r' ||
        object_map->perms[1] != 'w')
        return false;

    std::uintptr_t primary_vtable = 0;
    std::uintptr_t embedded_vtable = 0;
    std::uintptr_t manager = 0;
    std::uintptr_t scheduler = 0;
    std::uint8_t enabled = 0;
    std::int64_t accumulator = 0;
    if (!ReadExact(mem, object, &primary_vtable, sizeof(primary_vtable)) ||
        !ReadExact(mem, object + kEmbeddedTimeSourceOffset, &embedded_vtable,
                   sizeof(embedded_vtable)) ||
        !ReadExact(mem, object + kTimeScaleManagerOffset, &manager,
                   sizeof(manager)) ||
        !ReadExact(mem, object + kPhaseSchedulerOffset, &scheduler,
                   sizeof(scheduler)) ||
        !ReadExact(mem, object + kEnabledOffset, &enabled, sizeof(enabled)) ||
        !ReadExact(mem, object + kAccumulatorOffset, &accumulator,
                   sizeof(accumulator)))
        return false;
    if (primary_vtable != base + kMainVtableRva ||
        embedded_vtable != base + kEmbeddedTimeSourceVtableRva ||
        enabled > 1 || manager == 0 || scheduler == 0)
        return false;
    if (!FindMapping(maps, scheduler, sizeof(std::uintptr_t)) ||
        !FindMapping(maps, manager + kTimeScaleValueOffset,
                     sizeof(std::uint32_t)))
        return false;

    std::uint32_t scale_bits = 0;
    if (!ReadExact(mem, manager + kTimeScaleValueOffset, &scale_bits,
                   sizeof(scale_bits)))
        return false;
    float scale = 0.0f;
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    return std::isfinite(scale) && scale >= 0.0f && scale <= 64.0f &&
           accumulator > INT64_MIN;
}

bool ResolveMainObject(pid_t pid, std::uintptr_t base,
                       std::uintptr_t explicit_object,
                       std::uintptr_t* main_object) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
    const int mem = open(path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;
    if (explicit_object != 0) {
        const bool valid = ValidateMainObject(mem, maps, base, explicit_object);
        close(mem);
        if (!valid)
            std::fprintf(stderr, "explicit main object failed validation\n");
        if (valid) *main_object = explicit_object;
        return valid;
    }

    const std::uintptr_t expected_vtable = base + kMainVtableRva;
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
                if (value != expected_vtable) continue;
                const std::uintptr_t candidate = cursor + off;
                if (ValidateMainObject(mem, maps, base, candidate))
                    candidates.push_back(candidate);
            }
            cursor += static_cast<std::uintptr_t>(got);
        }
    }
    close(mem);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) {
        std::fprintf(stderr, "main object candidates=%zu (required exactly 1)\n",
                     candidates.size());
        for (std::uintptr_t candidate : candidates)
            std::fprintf(stderr, "  candidate=0x%" PRIxPTR "\n", candidate);
        return false;
    }
    *main_object = candidates.front();
    return true;
}

bool ValidateFinalOwner(int mem, const std::vector<Mapping>& maps,
                        std::uintptr_t owner) {
    if (owner > UINTPTR_MAX - kC9COffset - sizeof(std::uint32_t)) return false;
    const Mapping* map = FindMapping(maps, owner + kC98Offset,
                                     2 * sizeof(std::uint32_t));
    if (!map || map->perms[0] != 'r' || map->perms[1] != 'w') return false;
    std::uint64_t pair = 0;
    if (!ReadExact(mem, owner + kC98Offset, &pair, sizeof(pair))) return false;
    const std::uint32_t low = static_cast<std::uint32_t>(pair);
    const std::uint32_t high = static_cast<std::uint32_t>(pair >> 32);
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
    if (!ReadMaps(pid, &maps)) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
    const int mem = open(path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;
    if (explicit_owner != 0) {
        const bool valid = ValidateFinalOwner(mem, maps, explicit_owner);
        close(mem);
        if (!valid)
            std::fprintf(stderr, "explicit final owner failed validation\n");
        if (valid) *final_owner = explicit_owner;
        return valid;
    }

    const std::uintptr_t inner_vtable = base + kInnerVtableRva;
    if (!FindMapping(maps, inner_vtable, sizeof(std::int64_t)) ||
        inner_vtable < kInnerAdjustSlotDelta) {
        close(mem);
        return false;
    }
    std::int64_t adjustment = 0;
    if (!ReadExact(mem, inner_vtable - kInnerAdjustSlotDelta, &adjustment,
                   sizeof(adjustment))) {
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
                if (ValidateFinalOwner(mem, maps, owner))
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
    for (auto& thread : *threads)
        if (thread.tid == tid) return &thread;
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

bool ClearDebugRegistersExact(pid_t tid) {
    bool cleared = true;
    cleared = PokeDebug(tid, 7, 0) && cleared;
    cleared = PokeDebug(tid, 6, 0) && cleared;
    cleared = PokeDebug(tid, 0, 0) && cleared;
    cleared = PokeDebug(tid, 1, 0) && cleared;
    cleared = PokeDebug(tid, 2, 0) && cleared;
    cleared = PokeDebug(tid, 3, 0) && cleared;
    if (!cleared) return false;
    for (int index = 0; index <= 7; ++index) {
        if (index == 4 || index == 5) continue;
        unsigned long observed = ~0UL;
        if (!PeekDebug(tid, index, &observed) || observed != 0) return false;
    }
    return true;
}

bool StopThread(pid_t tid, bool debug_registers_may_be_active = true) {
    const auto clear_and_detach_stopped = [tid](int deliver) {
        if (!ClearDebugRegistersExact(tid)) return false;
        return ptrace(PTRACE_DETACH, tid, nullptr,
                      reinterpret_cast<void*>(
                          static_cast<intptr_t>(deliver))) != -1;
    };
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) {
        if (!debug_registers_may_be_active)
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }
    for (;;) {
        int status = 0;
        pid_t waited = 0;
        do {
            waited = waitpid(tid, &status, __WALL);
        } while (waited == -1 && errno == EINTR);
        if (waited != tid || WIFEXITED(status) || WIFSIGNALED(status))
            return false;
        if (!WIFSTOPPED(status)) continue;

        const int signal = WSTOPSIG(status);
        const unsigned int event =
            static_cast<unsigned int>(status) >> 16;
        if (signal == SIGTRAP && event == PTRACE_EVENT_STOP) return true;

        siginfo_t info{};
        const bool have_info =
            ptrace(PTRACE_GETSIGINFO, tid, nullptr, &info) != -1;
        unsigned long dr6 = 0;
        const bool hardware_breakpoint =
            signal == SIGTRAP && event == 0 && have_info &&
            info.si_signo == SIGTRAP && info.si_code == 4 &&
            PeekDebug(tid, 6, &dr6) && (dr6 & 15UL) != 0;

        int deliver = 0;
        if (hardware_breakpoint) {
            if (!PokeDebug(tid, 6, 0)) {
                clear_and_detach_stopped(0);
                return false;
            }
        } else if (event == 0 && have_info) {
            // A real signal won the race with PTRACE_INTERRUPT.  Preserve its
            // semantics, then continue waiting for our own EVENT_STOP.
            deliver = signal;
        } else if (event == 0) {
            // GETSIGINFO fails for a group-stop.  Detaching with signal 0
            // leaves the already-established group-stop in force; callers may
            // retry later, but must not resume or suppress it.
            clear_and_detach_stopped(0);
            return false;
        }

        if (ptrace(PTRACE_CONT, tid, nullptr,
                   reinterpret_cast<void*>(static_cast<intptr_t>(deliver))) ==
            -1) {
            // If this is still a signal-delivery-stop, preserve that signal on
            // the best-effort detach instead of the historical signal-0 path.
            clear_and_detach_stopped(deliver);
            return false;
        }
    }
}

bool ContinueThread(pid_t tid, int signal = 0) {
    return ptrace(PTRACE_CONT, tid, nullptr,
                  reinterpret_cast<void*>(static_cast<intptr_t>(signal))) != -1;
}

bool ClearAndDetach(pid_t tid, bool already_stopped) {
    if (!already_stopped && !StopThread(tid, true)) return false;
    if (!ClearDebugRegistersExact(tid)) return false;
    return ptrace(PTRACE_DETACH, tid, nullptr, nullptr) != -1;
}

unsigned long BoundaryDr7(bool state_watch_enabled = false) {
    // DR0: accumulator, local/write/8. DR1/DR2: controls, local/write/4.
    // Optional DR3: one 4-byte state component, local/write/4.
    unsigned long dr7 = 1UL | (1UL << 16) | (2UL << 18) |
                        (1UL << 2) | (1UL << 20) | (3UL << 22) |
                        (1UL << 4) | (1UL << 24) | (3UL << 26);
    if (state_watch_enabled)
        dr7 |= (1UL << 6) | (1UL << 28) | (3UL << 30);
    return dr7;
}

[[maybe_unused]] unsigned long AccumulatorOnlyDr7() {
    // Retry lifecycle gate: observe only PRE/POST_PHYSICS. Old-race control
    // addresses are deliberately disabled until the replacement owner is
    // uniquely resolved at a stopped PRE_PHYSICS event.
    return 1UL | (1UL << 16) | (2UL << 18);
}

bool AttachOne(pid_t tid, std::uintptr_t accumulator_address,
               std::uintptr_t c98_address, std::uintptr_t c9c_address,
               std::uintptr_t state_watch_address,
               unsigned long dr7) {
    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1) return false;
    if (!StopThread(tid, false)) return false;
    if (!PokeDebug(tid, 0, accumulator_address) ||
        !PokeDebug(tid, 1, c98_address) || !PokeDebug(tid, 2, c9c_address) ||
        !PokeDebug(tid, 3, state_watch_address) ||
        !PokeDebug(tid, 6, 0) ||
        !PokeDebug(tid, 7, dr7)) {
        ClearAndDetach(tid, true);
        return false;
    }
    if (!ContinueThread(tid)) {
        ClearAndDetach(tid, true);
        return false;
    }
    return true;
}

// Pre-resume arming variant: program the same watchpoints but deliberately
// leave the seized thread stopped.  This removes the run-then-interrupt race
// that exists when a paused game still emits zero-delta writes.
bool AttachOneStopped(pid_t tid, std::uintptr_t accumulator_address,
                      std::uintptr_t c98_address,
                      std::uintptr_t c9c_address,
                      std::uintptr_t state_watch_address,
                      unsigned long dr7) {
    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1) return false;
    if (!StopThread(tid, false)) return false;
    if (!PokeDebug(tid, 0, accumulator_address) ||
        !PokeDebug(tid, 1, c98_address) ||
        !PokeDebug(tid, 2, c9c_address) ||
        !PokeDebug(tid, 3, state_watch_address) || !PokeDebug(tid, 6, 0) ||
        !PokeDebug(tid, 7, dr7)) {
        ClearAndDetach(tid, true);
        return false;
    }
    return true;
}

std::size_t AttachNewThreads(pid_t pid, std::uintptr_t accumulator_address,
                             std::uintptr_t c98_address,
                             std::uintptr_t c9c_address,
                             std::uintptr_t state_watch_address,
                             std::vector<TracedThread>* threads,
                             std::uint64_t* failures,
                             unsigned long dr7) {
    std::size_t added = 0;
    for (pid_t tid : ListThreads(pid)) {
        if (FindThread(threads, tid)) continue;
        if (!AttachOne(tid, accumulator_address, c98_address, c9c_address,
                       state_watch_address, dr7)) {
            ++*failures;
            continue;
        }
        threads->push_back({tid, true, false});
        ++added;
    }
    return added;
}

[[maybe_unused]] std::size_t AttachNewThreadsStopped(
    pid_t pid, std::uintptr_t accumulator_address,
    std::uintptr_t c98_address, std::uintptr_t c9c_address,
    std::uintptr_t state_watch_address, std::vector<TracedThread>* threads,
    std::uint64_t* failures, unsigned long dr7) {
    std::size_t added = 0;
    for (pid_t tid : ListThreads(pid)) {
        if (FindThread(threads, tid)) continue;
        if (!AttachOneStopped(tid, accumulator_address, c98_address,
                              c9c_address, state_watch_address, dr7)) {
            ++*failures;
            continue;
        }
        threads->push_back({tid, true, true});
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
    if (argc < 5 || argc > 8) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "[MAIN_OBJECT_HEX] [FINAL_OWNER_HEX] "
                     "[WATCH_ADDRESS_HEX]\n", argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t object_value = 0, owner_value = 0, watch_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        (argc >= 6 && !ParseUnsigned(argv[5], 16, &object_value)) ||
        (argc >= 7 && !ParseUnsigned(argv[6], 16, &owner_value)) ||
        (argc == 8 && !ParseUnsigned(argv[7], 16, &watch_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 || duration_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto explicit_object = static_cast<std::uintptr_t>(object_value);
    const auto explicit_owner = static_cast<std::uintptr_t>(owner_value);
    const auto state_watch_address = static_cast<std::uintptr_t>(watch_value);
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);

    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }
    std::uintptr_t main_object = 0;
    if (!ResolveMainObject(pid, base, explicit_object, &main_object)) return 3;
    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base, explicit_owner, &final_owner)) return 3;
    if ((main_object + kAccumulatorOffset) % 8 != 0) {
        std::fprintf(stderr, "accumulator address is not 8-byte aligned\n");
        return 3;
    }
    const std::uintptr_t accumulator_address = main_object + kAccumulatorOffset;
    const std::uintptr_t enabled_address = main_object + kEnabledOffset;
    const std::uintptr_t c98_address = final_owner + kC98Offset;
    const std::uintptr_t c9c_address = final_owner + kC9COffset;
    if ((c98_address & 3u) != 0 || (c9c_address & 3u) != 0) {
        std::fprintf(stderr, "final-control addresses are not 4-byte aligned\n");
        return 3;
    }
    if (state_watch_address != 0 && (state_watch_address & 3u) != 0) {
        std::fprintf(stderr, "state-watch address is not 4-byte aligned\n");
        return 3;
    }
    if (state_watch_address != 0) {
        std::vector<Mapping> maps;
        if (!ReadMaps(pid, &maps)) return 3;
        const Mapping* watch_map =
            FindMapping(maps, state_watch_address, sizeof(std::uint32_t));
        if (!watch_map || watch_map->perms[0] != 'r' ||
            watch_map->perms[1] != 'w') {
            std::fprintf(stderr, "state-watch address is not readable/writable\n");
            return 3;
        }
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;
    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }

    TraceHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(TraceEvent);
    header.flags = kTargetSignatureVerified;
    if (explicit_object) header.flags |= kObjectWasExplicit;
    if (explicit_owner) header.flags |= kFinalOwnerWasExplicit;
    if (state_watch_address) header.flags |= kStateWatchEnabled;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.main_object = main_object;
    header.accumulator_address = accumulator_address;
    header.final_owner = final_owner;
    header.c98_address = c98_address;
    header.c9c_address = c9c_address;
    header.state_watch_address = state_watch_address;
    header.start_ns = MonotonicNs();
    if (header.start_ns == 0 || std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    std::vector<TracedThread> threads;
    std::uint64_t failures = 0;
    const std::size_t initial = AttachNewThreads(
        pid, accumulator_address, c98_address, c9c_address,
        state_watch_address, &threads, &failures,
        BoundaryDr7(state_watch_address != 0));
    header.initial_threads = static_cast<std::uint32_t>(initial);
    header.ptrace_errors += failures;
    if (initial == 0) {
        std::fclose(out);
        close(mem);
        return 7;
    }
    std::printf("HWBP_SCHED_V1 pid=%d base=0x%" PRIxPTR
                " object=0x%" PRIxPTR " accumulator=0x%" PRIxPTR
                " owner=0x%" PRIxPTR " c98=0x%" PRIxPTR
                " c9c=0x%" PRIxPTR " threads=%zu duration_ms=%" PRIu64
                " state_watch=0x%" PRIxPTR
                " write_scope=debug-registers-only\n",
                static_cast<int>(pid), base, main_object, accumulator_address,
                final_owner, c98_address, c9c_address, threads.size(),
                duration_ms, state_watch_address);
    std::fflush(stdout);

    const std::uint64_t deadline_ns = header.start_ns + duration_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = header.start_ns + 250000000ULL;
    bool output_ok = true;
    while (MonotonicNs() < deadline_ns) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t new_failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, accumulator_address, c98_address, c9c_address,
                state_watch_address, &threads, &new_failures,
                BoundaryDr7(state_watch_address != 0));
            header.thread_additions += added;
            header.ptrace_errors += new_failures;
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
        if (signal == SIGTRAP && have_dr6 && (dr6 & 15UL)) {
            TraceEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if (dr6 & 1UL) {
                event.flags |= kHitAccumulator;
                ++header.accumulator_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kHitC98;
                ++header.c98_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kHitC9C;
                ++header.c9c_hits;
            }
            if (dr6 & 8UL) {
                event.flags |= kHitStateWatch;
                ++header.state_watch_hits;
            }
            user_regs_struct regs{};
            if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1)
                ++header.ptrace_errors;
            else {
                event.rip = static_cast<std::uint64_t>(regs.rip);
                if (event.flags & kHitStateWatch) {
                    event.rsp = static_cast<std::uint64_t>(regs.rsp);
                    event.rbp = static_cast<std::uint64_t>(regs.rbp);
                    event.gpr[0] = static_cast<std::uint64_t>(regs.rax);
                    event.gpr[1] = static_cast<std::uint64_t>(regs.rbx);
                    event.gpr[2] = static_cast<std::uint64_t>(regs.rcx);
                    event.gpr[3] = static_cast<std::uint64_t>(regs.rdx);
                    event.gpr[4] = static_cast<std::uint64_t>(regs.rsi);
                    event.gpr[5] = static_cast<std::uint64_t>(regs.rdi);
                    event.gpr[6] = static_cast<std::uint64_t>(regs.r8);
                    event.gpr[7] = static_cast<std::uint64_t>(regs.r9);
                    event.gpr[8] = static_cast<std::uint64_t>(regs.r10);
                    event.gpr[9] = static_cast<std::uint64_t>(regs.r11);
                    event.gpr[10] = static_cast<std::uint64_t>(regs.r12);
                    event.gpr[11] = static_cast<std::uint64_t>(regs.r13);
                    event.gpr[12] = static_cast<std::uint64_t>(regs.r14);
                    event.gpr[13] = static_cast<std::uint64_t>(regs.r15);
                    event.stack_read_ok =
                        ReadExact(mem, static_cast<std::uintptr_t>(regs.rsp),
                                  event.stack_words,
                                  sizeof(event.stack_words))
                            ? 1u
                            : 0u;
                    if (!event.stack_read_ok) ++header.read_errors;
                }
            }

            std::uintptr_t manager = 0;
            std::uint8_t enabled = 0;
            std::uint64_t controls = 0;
            bool read_ok = ReadExact(mem, accumulator_address,
                                     &event.accumulator,
                                     sizeof(event.accumulator)) &&
                           ReadExact(mem, enabled_address, &enabled,
                                     sizeof(enabled)) &&
                           ReadExact(mem, main_object + kTimeScaleManagerOffset,
                                     &manager, sizeof(manager)) &&
                           manager != 0 &&
                           ReadExact(mem, manager + kTimeScaleValueOffset,
                                     &event.time_scale_bits,
                                     sizeof(event.time_scale_bits)) &&
                           ReadExact(mem, c98_address, &controls,
                                     sizeof(controls)) &&
                           (state_watch_address == 0 ||
                            ReadExact(mem, state_watch_address,
                                      &event.state_watch_bits,
                                      sizeof(event.state_watch_bits)));
            event.enabled = enabled;
            event.c98_bits = static_cast<std::uint32_t>(controls);
            event.c9c_bits = static_cast<std::uint32_t>(controls >> 32);
            event.read_ok = read_ok ? 1u : 0u;
            if (!read_ok) ++header.read_errors;
            if (std::fwrite(&event, sizeof(event), 1, out) != 1) {
                output_ok = false;
                break;
            }
            ++header.event_count;
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++header.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            ++header.unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++header.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

    std::uint32_t live_count = 0;
    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped))
            ++header.ptrace_errors;
        else {
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

    std::printf("HWBP_SCHED_V1_DONE events=%" PRIu64
                " accumulator=%" PRIu64 " c98=%" PRIu64
                " c9c=%" PRIu64
                " state_watch=%" PRIu64
                " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
                " thread_additions=%" PRIu64 " unexpected_stops=%" PRIu64
                " clean=%u path=%s\n",
                header.event_count, header.accumulator_hits,
                header.c98_hits, header.c9c_hits, header.state_watch_hits,
                header.read_errors,
                header.ptrace_errors,
                header.thread_additions, header.unexpected_stops,
                (header.flags & kHeaderClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
