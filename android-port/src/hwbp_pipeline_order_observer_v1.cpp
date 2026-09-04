// Host-only, observation-only proof of the asynchronous physics pipeline.
//
// DR0: PhysicsContext+0x1D0 completed-token collection (write/8)
// DR1: PhysicsContext+0x1A0 callback-list flags       (write/2)
// DR2: player CarPhysicsState+0xF64 publication       (write/4)
// DR3: inner PhysicsBackendWorld+0x188 accumulator    (write/4)
//
// The observer never patches guest code, single-steps ARM64, invokes a game
// function or writes gameplay state.  Its only target-process writes are the
// per-thread x86_64 debug registers required for hardware data breakpoints.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

namespace {

constexpr char kPipelineMagic[8] = {'A', '9', 'P', 'I', 'P', '1', '\0', '\0'};
constexpr std::uint32_t kPipelineVersion = 1;
constexpr std::uintptr_t kPhysicsContextVtableRva = 0x8103830;
constexpr std::uintptr_t kBackendAdapterDispatchRva = 0x3AD49CC;
constexpr std::uintptr_t kPhysicsBackendWorldVtableRva = 0x9D5F0C0;
constexpr std::uintptr_t kContextBackendAdapterOffset = 0x120;
constexpr std::uintptr_t kContextFixedIntervalOffset = 0x178;
constexpr std::uintptr_t kContextCallbackFlagsOffset = 0x1A0;
constexpr std::uintptr_t kContextWorkerEnabledOffset = 0x1C8;
constexpr std::uintptr_t kContextWorkerModeOffset = 0x1C9;
constexpr std::uintptr_t kContextCompletionTokenOffset = 0x1D0;
constexpr std::uintptr_t kBackendAdapterWorldOffset = 0x120;
constexpr std::uintptr_t kWorldAccumulatorOffset = 0x188;
constexpr std::uintptr_t kCarPhysicsF64Offset = 0xF64;

enum PipelineHeaderFlags : std::uint32_t {
    kPipelineClean = 1u << 0,
    kPipelineTargetVerified = 1u << 1,
    kPipelineContextExplicit = 1u << 2,
};

enum PipelineEventFlags : std::uint32_t {
    kPipelineHitCompletion = 1u << 0,
    kPipelineHitCallbackFlags = 1u << 1,
    kPipelineHitF64 = 1u << 2,
    kPipelineHitAccumulator = 1u << 3,
};

#pragma pack(push, 1)
struct PipelineHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t physics_context;
    std::uint64_t completion_address;
    std::uint64_t callback_flags_address;
    std::uint64_t car_physics_state;
    std::uint64_t f64_address;
    std::uint64_t backend_adapter;
    std::uint64_t inner_world;
    std::uint64_t accumulator_address;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t completion_hits;
    std::uint64_t callback_flag_hits;
    std::uint64_t f64_hits;
    std::uint64_t accumulator_hits;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t thread_additions;
    std::uint64_t unexpected_stops;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
};

struct PipelineEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::uint64_t completion_token;
    std::uint16_t callback_flags;
    std::uint16_t reserved16;
    std::uint32_t f64_bits;
    std::uint32_t accumulator_bits;
    std::uint32_t read_ok;
    std::uint32_t reserved32;
    std::uint32_t reserved32_2;
};
#pragma pack(pop)

static_assert(sizeof(PipelineHeader) == 192, "pipeline header ABI");
static_assert(sizeof(PipelineEvent) == 64, "pipeline event ABI");

bool PipelineWritable(const std::vector<Mapping>& maps,
                      std::uintptr_t address, std::size_t size) {
    const Mapping* map = FindMapping(maps, address, size);
    return map && map->perms[0] == 'r' && map->perms[1] == 'w';
}

bool ValidatePhysicsContext(int mem, const std::vector<Mapping>& maps,
                            std::uintptr_t base, std::uintptr_t context,
                            std::uintptr_t* adapter_out,
                            std::uintptr_t* world_out) {
    if (context > UINTPTR_MAX - kContextCompletionTokenOffset - 8 ||
        !PipelineWritable(maps, context,
                          kContextCompletionTokenOffset + 8))
        return false;
    std::uintptr_t vtable = 0;
    std::uintptr_t adapter = 0;
    std::uintptr_t adapter_vtable = 0;
    std::uintptr_t dispatch = 0;
    std::uintptr_t world = 0;
    std::uintptr_t world_vtable = 0;
    float interval = 0.0f;
    std::uint8_t worker_enabled = 0;
    std::uint8_t worker_mode = 0;
    if (!ReadExact(mem, context, &vtable, sizeof(vtable)) ||
        !ReadExact(mem, context + kContextFixedIntervalOffset, &interval,
                   sizeof(interval)) ||
        !ReadExact(mem, context + kContextWorkerEnabledOffset,
                   &worker_enabled, sizeof(worker_enabled)) ||
        !ReadExact(mem, context + kContextWorkerModeOffset, &worker_mode,
                   sizeof(worker_mode)) ||
        !ReadExact(mem, context + kContextBackendAdapterOffset, &adapter,
                   sizeof(adapter)) ||
        adapter == 0 || !ReadExact(mem, adapter, &adapter_vtable,
                                   sizeof(adapter_vtable)) ||
        !ReadExact(mem, adapter_vtable + 0x60, &dispatch,
                   sizeof(dispatch)) ||
        !ReadExact(mem, adapter + kBackendAdapterWorldOffset, &world,
                   sizeof(world)) ||
        world == 0 || !ReadExact(mem, world, &world_vtable,
                                 sizeof(world_vtable)))
        return false;
    if (vtable != base + kPhysicsContextVtableRva ||
        dispatch != base + kBackendAdapterDispatchRva ||
        world_vtable != base + kPhysicsBackendWorldVtableRva ||
        !std::isfinite(interval) || interval <= 0.0f || interval > 1.0f ||
        worker_enabled > 1 || worker_mode > 1 ||
        !PipelineWritable(maps, adapter, sizeof(std::uintptr_t)) ||
        !PipelineWritable(maps, world + kWorldAccumulatorOffset,
                          sizeof(std::uint32_t)))
        return false;
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
                                    adapter_out, world_out)) {
            std::fprintf(stderr, "explicit PhysicsContext failed validation\n");
            return false;
        }
        *context_out = explicit_context;
        return true;
    }
    const std::uintptr_t expected_vtable = base + kPhysicsContextVtableRva;
    std::vector<std::uintptr_t> candidates;
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
                cursor += want;
                continue;
            }
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <=
                     static_cast<std::size_t>(got);
                 offset += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                if (value != expected_vtable) continue;
                const std::uintptr_t candidate = cursor + offset;
                std::uintptr_t adapter = 0;
                std::uintptr_t world = 0;
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
        std::fprintf(stderr,
                     "PhysicsContext candidates=%zu (required exactly 1)\n",
                     candidates.size());
        for (const auto candidate : candidates)
            std::fprintf(stderr, "  candidate=0x%" PRIxPTR "\n", candidate);
        return false;
    }
    if (!ValidatePhysicsContext(mem, maps, base, candidates.front(),
                                adapter_out, world_out))
        return false;
    *context_out = candidates.front();
    return true;
}

unsigned long PipelineDr7() {
    // Local write breakpoints. Length encoding: 8=2, 2=1, 4=3, 4=3.
    return 1UL | (1UL << 16) | (2UL << 18) |
           (1UL << 2) | (1UL << 20) | (1UL << 22) |
           (1UL << 4) | (1UL << 24) | (3UL << 26) |
           (1UL << 6) | (1UL << 28) | (3UL << 30);
}

}  // namespace

#ifndef A9TAS_PIPELINE_ORDER_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 5 || argc > 6) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "[PHYSICS_CONTEXT_HEX]\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t context_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        (argc == 6 && !ParseUnsigned(argv[5], 16, &context_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 || duration_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
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
    a9tas::vehicle_state_v1::Layout vehicle{};
    if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle)) {
        std::fprintf(stderr, "player CarPhysicsState resolution failed\n");
        close(mem);
        return 3;
    }
    const std::uintptr_t completion =
        context + kContextCompletionTokenOffset;
    const std::uintptr_t callback_flags =
        context + kContextCallbackFlagsOffset;
    const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
    const std::uintptr_t accumulator = world + kWorldAccumulatorOffset;
    std::vector<Mapping> maps;
    if ((completion & 7u) != 0 || (callback_flags & 1u) != 0 ||
        (f64 & 3u) != 0 || (accumulator & 3u) != 0 ||
        !ReadMaps(pid, &maps) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, accumulator, 4)) {
        std::fprintf(stderr, "pipeline watch address validation failed\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }
    PipelineHeader header{};
    std::memcpy(header.magic, kPipelineMagic, sizeof(kPipelineMagic));
    header.version = kPipelineVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(PipelineEvent);
    header.flags = kPipelineTargetVerified;
    if (context_value) header.flags |= kPipelineContextExplicit;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.physics_context = context;
    header.completion_address = completion;
    header.callback_flags_address = callback_flags;
    header.car_physics_state = vehicle.physics_base;
    header.f64_address = f64;
    header.backend_adapter = adapter;
    header.inner_world = world;
    header.accumulator_address = accumulator;
    header.start_ns = MonotonicNs();
    if (header.start_ns == 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    std::vector<TracedThread> threads;
    std::uint64_t failures = 0;
    const std::size_t initial = AttachNewThreads(
        pid, completion, callback_flags, f64, accumulator, &threads,
        &failures, PipelineDr7());
    header.initial_threads = static_cast<std::uint32_t>(initial);
    header.ptrace_errors += failures;
    if (initial == 0) {
        std::fclose(out);
        close(mem);
        return 7;
    }
    std::printf(
        "PIPELINE_ORDER_V1 pid=%d context=0x%" PRIxPTR
        " completion=0x%" PRIxPTR " callback_flags=0x%" PRIxPTR
        " car_state=0x%" PRIxPTR " f64=0x%" PRIxPTR
        " adapter=0x%" PRIxPTR " world=0x%" PRIxPTR
        " accumulator=0x%" PRIxPTR " threads=%zu duration_ms=%" PRIu64
        " write_scope=debug-registers-only guest_patch=none\n",
        static_cast<int>(pid), context, completion, callback_flags,
        vehicle.physics_base, f64, adapter, world, accumulator,
        threads.size(), duration_ms);
    std::fflush(stdout);

    const std::uint64_t deadline_ns =
        header.start_ns + duration_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = header.start_ns + 250000000ULL;
    std::uint32_t accounted_threads = 0;
    bool output_ok = true;
    while (MonotonicNs() < deadline_ns) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t new_failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, completion, callback_flags, f64, accumulator, &threads,
                &new_failures, PipelineDr7());
            header.thread_additions += added;
            header.ptrace_errors += new_failures;
            next_rescan_ns = now_ns + 250000000ULL;
        }
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno != ECHILD) ++header.ptrace_errors;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        TracedThread* tracked = FindThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked && tracked->live) {
                tracked->live = false;
                tracked->stopped = false;
                ++accounted_threads;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        if (signal == SIGTRAP && have_dr6 && (dr6 & 15UL)) {
            PipelineEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if (dr6 & 1UL) {
                event.flags |= kPipelineHitCompletion;
                ++header.completion_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kPipelineHitCallbackFlags;
                ++header.callback_flag_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kPipelineHitF64;
                ++header.f64_hits;
            }
            if (dr6 & 8UL) {
                event.flags |= kPipelineHitAccumulator;
                ++header.accumulator_hits;
            }
            user_regs_struct regs{};
            if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1)
                ++header.ptrace_errors;
            else
                event.rip = static_cast<std::uint64_t>(regs.rip);
            const bool read_ok =
                ReadExact(mem, completion, &event.completion_token,
                          sizeof(event.completion_token)) &&
                ReadExact(mem, callback_flags, &event.callback_flags,
                          sizeof(event.callback_flags)) &&
                ReadExact(mem, f64, &event.f64_bits,
                          sizeof(event.f64_bits)) &&
                ReadExact(mem, accumulator, &event.accumulator_bits,
                          sizeof(event.accumulator_bits));
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

    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped))
            ++header.ptrace_errors;
        else
            ++accounted_threads;
        thread.live = false;
        thread.stopped = false;
    }
    header.final_threads = accounted_threads;
    if (output_ok && header.event_count > 0 && header.read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0 &&
        header.final_threads ==
            header.initial_threads + header.thread_additions)
        header.flags |= kPipelineClean;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);
    std::printf(
        "PIPELINE_ORDER_V1_DONE events=%" PRIu64
        " completion=%" PRIu64 " callback_flags=%" PRIu64
        " f64=%" PRIu64 " accumulator=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " unexpected_stops=%" PRIu64 " clean=%u path=%s\n",
        header.event_count, header.completion_hits,
        header.callback_flag_hits, header.f64_hits, header.accumulator_hits,
        header.read_errors, header.ptrace_errors, header.unexpected_stops,
        (header.flags & kPipelineClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
#endif
