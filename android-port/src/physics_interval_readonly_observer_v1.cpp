// Read-only PhysicsContext interval identity and stability observer.
//
// This program opens /proc/PID/mem O_RDONLY. It performs no ptrace operation,
// callback installation, process write or game-method call.

#ifdef A9TAS_G8_PROFILE_OBSERVER
#include "g8_runtime_build_profile_v1.h"
#endif

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
#include <fcntl.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "remote_data_address_v1.h"

namespace {

#ifdef A9TAS_G8_PROFILE_OBSERVER
namespace build_profile = a9tas::g8_runtime_build_profile_v1;
#endif

constexpr char kIntervalMagic[8] = {'A', '9', 'P', 'I', 'O', '2', '\0', '\0'};
constexpr std::uint32_t kIntervalVersion = 2;
constexpr std::uintptr_t kPhysicsContextVtableRva = 0x8103830;
constexpr std::uintptr_t kDefaultStepOptionsVtableRva = 0x81039A0;
constexpr std::uintptr_t kDefaultStepOptionsGetterRva = 0x38B7C5C;
constexpr std::uintptr_t kBackendAdapterDispatchRva = 0x3AD49CC;
constexpr std::uintptr_t kPhysicsBackendWorldVtableRva = 0x9D5F0C0;
constexpr std::uintptr_t kContextBackendAdapterOffset = 0x120;
constexpr std::uintptr_t kContextStepOptionsOffset = 0x170;
constexpr std::uintptr_t kContextFixedIntervalOffset = 0x178;
constexpr std::uintptr_t kContextWorkerEnabledOffset = 0x1C8;
constexpr std::uintptr_t kContextWorkerModeOffset = 0x1C9;
constexpr std::uintptr_t kContextCompletionTokenOffset = 0x1D0;
constexpr std::uintptr_t kBackendAdapterWorldOffset = 0x120;
constexpr std::uintptr_t kWorldAccumulatorOffset = 0x188;

#ifdef A9TAS_G8_PROFILE_OBSERVER
struct RuntimeBuild {
    std::uintptr_t physics_context_vtable_rva{kPhysicsContextVtableRva};
    std::uintptr_t step_options_vtable_rva{0x7EED420};
    std::uint64_t image_size{0xA5D6E3C};
    bool profile_driven{};
};

RuntimeBuild g_runtime_build{};

bool LoadRuntimeBuildProfile(const char* path) {
    if (path == nullptr || *path == '\0') return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    build_profile::Profile profile{};
    std::size_t done = 0;
    while (done < sizeof(profile)) {
        const ssize_t count = read(fd,
            reinterpret_cast<std::uint8_t*>(&profile) + done,
            sizeof(profile) - done);
        if (count <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    const bool closed = close(fd) == 0;
    if (!closed || !build_profile::Valid(profile)) return false;
    g_runtime_build.physics_context_vtable_rva =
        static_cast<std::uintptr_t>(profile.physics_context_vtable_rva);
    g_runtime_build.step_options_vtable_rva =
        static_cast<std::uintptr_t>(profile.step_options_vtable_rva);
    g_runtime_build.image_size = profile.image_size;
    g_runtime_build.profile_driven = true;
    return true;
}
#endif

struct BuildSignature {
    std::uintptr_t rva;
    const std::uint8_t* bytes;
    std::size_t size;
};

constexpr std::uint8_t kPhysicsTimeCallbackSignature[] = {
    0xFF, 0xC3, 0x00, 0xD1, 0xF4, 0x0B, 0x00, 0xF9,
    0xF3, 0x7B, 0x02, 0xA9, 0xF4, 0x03, 0x00, 0xAA,
    0x00, 0xF4, 0x41, 0xF9, 0xF3, 0x03, 0x02, 0xAA,
    0x40, 0x00, 0x00, 0xB4, 0xF3, 0x94, 0x36, 0x94,
};
constexpr std::uint8_t kPhysicsChunkerSignature[] = {
    0xFF, 0x43, 0x01, 0xD1, 0xF7, 0x5B, 0x02, 0xA9,
    0xF5, 0x53, 0x03, 0xA9, 0xF3, 0x7B, 0x04, 0xA9,
    0x68, 0x6D, 0x03, 0xD0, 0x08, 0x61, 0x1E, 0x91,
};
constexpr BuildSignature kBuildSignatures[] = {
    {0x3791618, kPhysicsTimeCallbackSignature,
     sizeof(kPhysicsTimeCallbackSignature)},
    {0x37949C8, kPhysicsChunkerSignature, sizeof(kPhysicsChunkerSignature)},
};

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

std::uint64_t MonotonicNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    const std::uintptr_t raw_address = address;
    address = a9tas::remote_data_address_v1::Untag(address);
    if (address > UINTPTR_MAX - size) return false;
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count = pread(fd, cursor + done, size - done,
                                    static_cast<off_t>(address + done));
        if (count <= 0) {
            std::fprintf(stderr, "PhysicsContext read_failed raw=0x%" PRIxPTR
                " offset=0x%" PRIxPTR " size=%zu result=%zd errno=%d\n",
                raw_address, address + done, size - done, count, errno);
            return false;
        }
        done += static_cast<std::size_t>(count);
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
        char perms[5]{}, path_buffer[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %*llx %*s %*s %1023[^\n]", &begin, &end,
            perms, path_buffer);
        if (fields < 3) continue;
        Mapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), {}, ""};
        std::memcpy(mapping.perms, perms, 4);
        if (fields == 4) mapping.path = path_buffer;
        while (!mapping.path.empty() && mapping.path.front() == ' ')
            mapping.path.erase(0, 1);
        maps->push_back(mapping);
    }
    std::fclose(file);
    return !maps->empty();
}

const Mapping* FindMapping(const std::vector<Mapping>& maps,
                           std::uintptr_t address, std::size_t size) {
    address = a9tas::remote_data_address_v1::Untag(address);
    if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
    const std::uintptr_t end = address + size;
    for (const auto& map : maps) {
        if (address >= map.begin && end <= map.end) return &map;
    }
    return nullptr;
}

bool IsWritable(const std::vector<Mapping>& maps, std::uintptr_t address,
                std::size_t size) {
    const Mapping* map = FindMapping(maps, address, size);
    return map && map->perms[0] == 'r' && map->perms[1] == 'w';
}

#ifdef A9TAS_G8_PROFILE_OBSERVER
bool IsGameReadonly(const std::vector<Mapping>& maps, std::uintptr_t address,
                    std::size_t size) {
    const Mapping* map = FindMapping(maps, address, size);
    return map && map->perms[0] == 'r' && map->perms[1] != 'w' &&
           map->path.find("libAsphalt9.so") != std::string::npos;
}
#endif

bool VerifyTargetBuild(pid_t pid, std::uintptr_t base) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    const Mapping* base_map = FindMapping(maps, base, 1);
    if (!base_map || base_map->perms[0] != 'r' ||
        base_map->path.find("libAsphalt9.so") == std::string::npos)
        return false;
#ifdef A9TAS_G8_PROFILE_OBSERVER
    if (g_runtime_build.profile_driven) {
        if (base > UINTPTR_MAX - g_runtime_build.image_size ||
            base > UINTPTR_MAX - g_runtime_build.physics_context_vtable_rva ||
            base > UINTPTR_MAX - g_runtime_build.step_options_vtable_rva)
            return false;
        return FindMapping(maps,
                           base + g_runtime_build.physics_context_vtable_rva,
                           sizeof(std::uintptr_t)) != nullptr &&
               FindMapping(maps,
                           base + g_runtime_build.step_options_vtable_rva,
                           sizeof(std::uintptr_t)) != nullptr;
    }
#endif
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
        if (!FindMapping(maps, address, signature.size) ||
            !ReadExact(mem, address, actual.data(), actual.size()) ||
            std::memcmp(actual.data(), signature.bytes, signature.size) != 0) {
            ok = false;
            break;
        }
    }
    close(mem);
    return ok;
}

bool ValidatePhysicsContext(int mem, const std::vector<Mapping>& maps,
                            std::uintptr_t base, std::uintptr_t context,
                            std::uintptr_t* adapter_out,
                            std::uintptr_t* world_out) {
    if (context > UINTPTR_MAX - kContextCompletionTokenOffset - 8 ||
        !IsWritable(maps, context, kContextCompletionTokenOffset + 8))
        { std::fprintf(stderr, "PhysicsContext reject=context_mapping address=0x%" PRIxPTR "\n", context); return false; }
    std::uintptr_t vtable = 0, adapter = 0, adapter_vtable = 0;
    std::uintptr_t dispatch = 0, world = 0, world_vtable = 0;
    float interval = 0.0f;
    std::uint8_t worker_enabled = 0, worker_mode = 0;
    if (!ReadExact(mem, context, &vtable, sizeof(vtable)) ||
        !ReadExact(mem, context + kContextFixedIntervalOffset, &interval,
                   sizeof(interval)) ||
        !ReadExact(mem, context + kContextWorkerEnabledOffset,
                   &worker_enabled, sizeof(worker_enabled)) ||
        !ReadExact(mem, context + kContextWorkerModeOffset, &worker_mode,
                   sizeof(worker_mode)) ||
        !ReadExact(mem, context + kContextBackendAdapterOffset, &adapter,
                   sizeof(adapter)) || adapter == 0 ||
        !ReadExact(mem, adapter, &adapter_vtable, sizeof(adapter_vtable)) ||
        !ReadExact(mem, adapter_vtable + 0x60, &dispatch, sizeof(dispatch)) ||
        !ReadExact(mem, adapter + kBackendAdapterWorldOffset, &world,
                   sizeof(world)) || world == 0 ||
        !ReadExact(mem, world, &world_vtable, sizeof(world_vtable)))
        { std::fprintf(stderr, "PhysicsContext reject=pointer_chain address=0x%" PRIxPTR " errno=%d\n", context, errno); return false; }
#ifdef A9TAS_G8_PROFILE_OBSERVER
    const bool backend_identity = g_runtime_build.profile_driven
        ? IsGameReadonly(maps, dispatch, sizeof(std::uint32_t)) &&
              IsGameReadonly(maps, world_vtable, sizeof(std::uintptr_t))
        : dispatch == base + kBackendAdapterDispatchRva &&
              world_vtable == base + kPhysicsBackendWorldVtableRva;
    if (vtable != base + g_runtime_build.physics_context_vtable_rva ||
        !backend_identity ||
#else
    if (vtable != base + kPhysicsContextVtableRva ||
        dispatch != base + kBackendAdapterDispatchRva ||
        world_vtable != base + kPhysicsBackendWorldVtableRva ||
#endif
        !std::isfinite(interval) || interval <= 0.0f || interval > 1.0f ||
        worker_enabled > 1 || worker_mode > 1 ||
        !IsWritable(maps, adapter, sizeof(std::uintptr_t)) ||
        !IsWritable(maps, world + kWorldAccumulatorOffset,
                    sizeof(std::uint32_t)))
        { std::fprintf(stderr, "PhysicsContext reject=fields address=0x%" PRIxPTR
            " interval=%g worker=%u,%u adapter=0x%" PRIxPTR
            " world=0x%" PRIxPTR " dispatch=0x%" PRIxPTR "\n",
            context, interval, worker_enabled, worker_mode, adapter, world, dispatch);
          return false; }
    *adapter_out = adapter;
    *world_out = world;
    return true;
}

bool ResolvePhysicsContext(pid_t pid, int mem, std::uintptr_t base,
                           std::uintptr_t explicit_context,
                           std::uintptr_t* context_out,
                           std::uintptr_t* adapter_out,
                           std::uintptr_t* world_out) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    if (explicit_context != 0) {
        if (!ValidatePhysicsContext(mem, maps, base, explicit_context,
                                    adapter_out, world_out))
            return false;
        *context_out = explicit_context;
        return true;
    }
#ifdef A9TAS_G8_PROFILE_OBSERVER
    const std::uintptr_t expected_vtable =
        base + g_runtime_build.physics_context_vtable_rva;
#else
    const std::uintptr_t expected_vtable = base + kPhysicsContextVtableRva;
#endif
    std::vector<std::uintptr_t> candidates;
    std::size_t hits = 0, read_failures = 0;
    const long native_page_size = sysconf(_SC_PAGESIZE);
    const std::uintptr_t page_size = native_page_size > 0
        ? static_cast<std::uintptr_t>(native_page_size) : 4096u;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const auto& map : maps) {
        if (map.perms[0] != 'r' || map.perms[1] != 'w' ||
            map.path == "[vvar]" || map.path == "[vdso]")
            continue;
        for (std::uintptr_t cursor = map.begin; cursor < map.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buffer.size(), map.end - cursor));
            const ssize_t got = pread(mem, buffer.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                // One unreadable page must not hide the rest of a 1 MiB chunk.
                ++read_failures;
                cursor += std::min<std::uintptr_t>(
                    page_size - cursor % page_size, map.end - cursor);
                continue;
            }
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <=
                     static_cast<std::size_t>(got);
                 offset += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                if (value != expected_vtable) continue;
                ++hits;
                const std::uintptr_t candidate = cursor + offset;
                std::uintptr_t adapter = 0, world = 0;
                if (ValidatePhysicsContext(mem, maps, base, candidate,
                                           &adapter, &world))
                    candidates.push_back(candidate);
            }
            cursor += static_cast<std::uintptr_t>(got);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) {
        std::fprintf(stderr, "PhysicsContext candidates=%zu; require 1; vtable_hits=%zu read_failures=%zu\n",
                     candidates.size(), hits, read_failures);
        return false;
    }
    if (!ValidatePhysicsContext(mem, maps, base, candidates.front(),
                                adapter_out, world_out))
        return false;
    *context_out = candidates.front();
    return true;
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

enum HeaderFlag : std::uint32_t {
    kClean = 1u << 0,
    kTargetVerified = 1u << 1,
    kContextExplicit = 1u << 2,
    kInlineDefaultForAllSamples = 1u << 3,
    kIdentityStable = 1u << 4,
    kIntervalStable = 1u << 5,
};

enum SampleFlag : std::uint32_t {
    kReadOk = 1u << 0,
    kContextIdentityOk = 1u << 1,
    kBackendIdentityOk = 1u << 2,
    kInlineDefaultOptions = 1u << 3,
    kIntervalFinite = 1u << 4,
    kStepOptionsHeadReadOk = 1u << 5,
};

#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t sample_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t physics_context;
    std::uint64_t backend_adapter;
    std::uint64_t backend_world;
    std::uint64_t start_ns;
    std::uint64_t requested_duration_ms;
    std::uint64_t requested_sample_ms;
    std::uint64_t sample_count;
    std::uint64_t read_errors;
    std::uint64_t identity_failures;
    std::uint64_t alternate_options_samples;
    std::uint64_t interval_change_samples;
    std::uint32_t initial_interval_bits;
    std::uint32_t reserved;
};

struct Sample {
    std::uint64_t monotonic_ns;
    std::uint64_t step_options;
    std::uint64_t step_options_vptr;
    std::uint64_t step_options_getter;
    std::uint64_t step_options_word8;
    std::uint64_t backend_adapter;
    std::uint64_t backend_world;
    std::uint32_t interval_bits;
    std::uint32_t accumulator_bits;
    std::uint32_t flags;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 136, "physics interval observer header ABI");
static_assert(sizeof(Sample) == 72, "physics interval observer sample ABI");

bool ReadSample(int mem, std::uintptr_t base, std::uintptr_t context,
                std::uintptr_t expected_adapter, std::uintptr_t expected_world,
                Sample* sample) {
    std::uintptr_t context_vptr = 0;
    std::uintptr_t step_options = 0;
    std::uintptr_t step_options_vptr = 0;
    std::uintptr_t step_options_getter = 0;
    std::uint64_t step_options_word8 = 0;
    std::uintptr_t adapter = 0;
    std::uintptr_t adapter_vptr = 0;
    std::uintptr_t dispatch = 0;
    std::uintptr_t world = 0;
    std::uintptr_t world_vptr = 0;
    std::uint32_t interval_bits = 0;
    std::uint32_t accumulator_bits = 0;
    sample->monotonic_ns = MonotonicNs();
    if (sample->monotonic_ns == 0 ||
        !ReadExact(mem, context, &context_vptr, sizeof(context_vptr)) ||
        !ReadExact(mem, context + kContextStepOptionsOffset, &step_options,
                   sizeof(step_options)) ||
        !ReadExact(mem, context + kContextFixedIntervalOffset, &interval_bits,
                   sizeof(interval_bits)) ||
        !ReadExact(mem, context + kContextBackendAdapterOffset, &adapter,
                   sizeof(adapter)) ||
        adapter == 0 ||
        !ReadExact(mem, adapter, &adapter_vptr, sizeof(adapter_vptr)) ||
        !ReadExact(mem, adapter_vptr + 0x60, &dispatch, sizeof(dispatch)) ||
        !ReadExact(mem, adapter + kBackendAdapterWorldOffset, &world,
                   sizeof(world)) ||
        world == 0 ||
        !ReadExact(mem, world, &world_vptr, sizeof(world_vptr)) ||
        !ReadExact(mem, world + kWorldAccumulatorOffset, &accumulator_bits,
                   sizeof(accumulator_bits))) {
        return false;
    }

    sample->flags = kReadOk;
    sample->step_options = step_options;
    if (step_options != 0 &&
        ReadExact(mem, step_options, &step_options_vptr,
                  sizeof(step_options_vptr)) &&
        ReadExact(mem, step_options + 8, &step_options_word8,
                  sizeof(step_options_word8)) &&
        ReadExact(mem, step_options_vptr + 0x10, &step_options_getter,
                  sizeof(step_options_getter))) {
        sample->flags |= kStepOptionsHeadReadOk;
    }
    sample->step_options_vptr = step_options_vptr;
    sample->step_options_getter = step_options_getter;
    sample->step_options_word8 = step_options_word8;
    sample->backend_adapter = adapter;
    sample->backend_world = world;
    sample->interval_bits = interval_bits;
    sample->accumulator_bits = accumulator_bits;
#ifdef A9TAS_G8_PROFILE_OBSERVER
    if (context_vptr == base + g_runtime_build.physics_context_vtable_rva)
        sample->flags |= kContextIdentityOk;
    const bool backend_identity = g_runtime_build.profile_driven
        ? adapter == expected_adapter && world == expected_world
        : adapter == expected_adapter && world == expected_world &&
              dispatch == base + kBackendAdapterDispatchRva &&
              world_vptr == base + kPhysicsBackendWorldVtableRva;
    if (backend_identity)
        sample->flags |= kBackendIdentityOk;
#else
    if (context_vptr == base + kPhysicsContextVtableRva)
        sample->flags |= kContextIdentityOk;
    if (adapter == expected_adapter && world == expected_world &&
        dispatch == base + kBackendAdapterDispatchRva &&
        world_vptr == base + kPhysicsBackendWorldVtableRva)
        sample->flags |= kBackendIdentityOk;
#endif
    if (step_options == 0) sample->flags |= kInlineDefaultOptions;
    float interval = 0.0f;
    std::memcpy(&interval, &interval_bits, sizeof(interval));
    if (std::isfinite(interval) && interval > 0.0f && interval <= 1.0f)
        sample->flags |= kIntervalFinite;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef A9TAS_G8_PROFILE_OBSERVER
    if (argc < 6 || argc > 8) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS SAMPLE_MS "
                     "OUT_PATH [PHYSICS_CONTEXT_HEX] [BUILD_PROFILE]\n",
                     argv[0]);
        return 2;
    }
#else
    if (argc < 6 || argc > 7) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS SAMPLE_MS "
                     "OUT_PATH [PHYSICS_CONTEXT_HEX]\n",
                     argv[0]);
        return 2;
    }
#endif
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t sample_value = 0, context_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        !ParseUnsigned(argv[4], 10, &sample_value) ||
#ifdef A9TAS_G8_PROFILE_OBSERVER
        (argc >= 7 && !ParseUnsigned(argv[6], 16, &context_value)) ||
#else
        (argc == 7 && !ParseUnsigned(argv[6], 16, &context_value)) ||
#endif
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 || duration_value > 30000 ||
        sample_value < 1 || sample_value > duration_value) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

#ifdef A9TAS_G8_PROFILE_OBSERVER
    if (argc == 8 && !LoadRuntimeBuildProfile(argv[7])) {
        std::fprintf(stderr, "invalid runtime build profile\n");
        return 2;
    }
#endif

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no process memory opened\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;

    std::uintptr_t context = 0, adapter = 0, world = 0;
    if (!ResolvePhysicsContext(pid, mem, base,
                               static_cast<std::uintptr_t>(context_value),
                               &context, &adapter, &world)) {
        close(mem);
        return 3;
    }
    FILE* out = std::fopen(argv[5], "wb");
    if (!out) {
        close(mem);
        return 5;
    }

    Header header{};
    std::memcpy(header.magic, kIntervalMagic, sizeof(kIntervalMagic));
    header.version = kIntervalVersion;
    header.header_size = sizeof(header);
    header.sample_size = sizeof(Sample);
    header.flags = kTargetVerified | kInlineDefaultForAllSamples |
                   kIdentityStable | kIntervalStable;
    if (context_value != 0) header.flags |= kContextExplicit;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.physics_context = context;
    header.backend_adapter = adapter;
    header.backend_world = world;
    header.start_ns = MonotonicNs();
    header.requested_duration_ms = duration_value;
    header.requested_sample_ms = sample_value;
    if (header.start_ns == 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    const std::uint64_t deadline =
        header.start_ns + duration_value * 1000000ULL;
    bool output_ok = true;
    while (MonotonicNs() < deadline) {
        Sample sample{};
        if (!ReadSample(mem, base, context, adapter, world, &sample)) {
            ++header.read_errors;
            header.flags &= ~(kIdentityStable | kIntervalStable |
                              kInlineDefaultForAllSamples);
        } else {
            const std::uint32_t identity_mask =
                kContextIdentityOk | kBackendIdentityOk;
            if ((sample.flags & identity_mask) != identity_mask) {
                ++header.identity_failures;
                header.flags &= ~kIdentityStable;
            }
            if ((sample.flags & kInlineDefaultOptions) == 0) {
                ++header.alternate_options_samples;
                header.flags &= ~kInlineDefaultForAllSamples;
            }
            if (header.sample_count == 0) {
                header.initial_interval_bits = sample.interval_bits;
            } else if (sample.interval_bits != header.initial_interval_bits) {
                ++header.interval_change_samples;
                header.flags &= ~kIntervalStable;
            }
            if ((sample.flags & kIntervalFinite) == 0)
                header.flags &= ~kIntervalStable;
            if (std::fwrite(&sample, sizeof(sample), 1, out) != 1) {
                output_ok = false;
                break;
            }
            ++header.sample_count;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(sample_value));
    }

    if (output_ok) header.flags |= kClean;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0)
        output_ok = false;
    std::fclose(out);
    close(mem);

    float interval = 0.0f;
    std::memcpy(&interval, &header.initial_interval_bits, sizeof(interval));
    std::printf(
        "PHYSICS_INTERVAL_READONLY_V1 pid=%d context=0x%" PRIxPTR
        " adapter=0x%" PRIxPTR " world=0x%" PRIxPTR
        " samples=%" PRIu64 " read_errors=%" PRIu64
        " identity_failures=%" PRIu64 " alternate_options=%" PRIu64
        " interval_changes=%" PRIu64 " interval_bits=0x%08x"
        " interval=%.9g default_options_vtable=0x%" PRIxPTR
        " default_options_getter=0x%" PRIxPTR
        " clean=%u ptrace=0 game_writes=0\n",
        static_cast<int>(pid), context, adapter, world, header.sample_count,
        header.read_errors, header.identity_failures,
        header.alternate_options_samples, header.interval_change_samples,
        header.initial_interval_bits, static_cast<double>(interval),
        base + kDefaultStepOptionsVtableRva,
        base + kDefaultStepOptionsGetterRva,
        output_ok ? 1u : 0u);
    return output_ok ? 0 : 6;
}
