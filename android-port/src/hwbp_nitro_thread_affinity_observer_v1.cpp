// Observation-only NitroService thread-affinity probe.
//
// DR0: MainTimeSource+0x150 fixed-delta accumulator (write/8)
// DR1: NitroState+0x188 active byte                (write/1)
// DR2: NitroState+0x18C mode                       (write/4)
// DR3: NitroState+0x1AC encrypted scalar head      (write/4)
//
// The probe never writes target memory, invokes a game function, sends input,
// or patches guest code. Its only target-process writes are per-thread x86_64
// debug registers used for hardware data breakpoints.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#ifndef A9TAS_NITRO_AFFINITY_ENTRY
#define A9TAS_NITRO_AFFINITY_ENTRY main
#endif

namespace {

constexpr char kNitroAffinityMagic[8] = {
    'A', '9', 'N', 'T', 'A', '1', '\0', '\0',
};
constexpr std::uint32_t kNitroAffinityVersion = 1;
constexpr std::uintptr_t kServiceOffset = 0xCB8;
constexpr std::uintptr_t kServiceVtableRva = 0x7EE8A90;
constexpr std::uintptr_t kServiceActivateSlot = 0x108;
constexpr std::uintptr_t kServiceActivateRva = 0x3674E50;
constexpr std::uintptr_t kNitroStateOffset = 8;
constexpr std::uintptr_t kOptionalOffset = 0x180;
constexpr std::uintptr_t kOptionalValueOffset = 0x184;
constexpr std::uintptr_t kActiveOffset = 0x188;
constexpr std::uintptr_t kModeOffset = 0x18C;
constexpr std::uintptr_t kEncryptedOffset = 0x1AC;
constexpr std::uintptr_t kGatesOffset = 0x1B8;

enum HeaderFlag : std::uint32_t {
    kTargetVerified = 1u << 0,
    kCleanDetach = 1u << 1,
    kCapturedDelta = 1u << 2,
    kCapturedNitro = 1u << 3,
};

enum EventFlag : std::uint32_t {
    kHitDelta = 1u << 0,
    kHitActive = 1u << 1,
    kHitMode = 1u << 2,
    kHitEncrypted = 1u << 3,
};

#pragma pack(push, 1)
struct NitroSnapshot {
    std::uint8_t optional_180;
    std::uint8_t active_188;
    std::uint8_t gates_1b8_1bc[5];
    std::uint8_t reserved;
    std::uint32_t optional_value_184;
    std::uint32_t mode_18c;
    std::uint32_t encrypted_1ac_1b4[3];
};

struct AffinityHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t main_object;
    std::uint64_t delta_address;
    std::uint64_t final_owner;
    std::uint64_t service_object;
    std::uint64_t state_address;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t delta_hits;
    std::uint64_t active_hits;
    std::uint64_t mode_hits;
    std::uint64_t encrypted_hits;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t thread_additions;
    std::uint64_t unexpected_stops;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    NitroSnapshot initial_snapshot;
    NitroSnapshot final_snapshot;
};

struct AffinityEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::int64_t delta_value;
    NitroSnapshot snapshot;
    std::uint32_t read_ok;
};
#pragma pack(pop)

static_assert(sizeof(NitroSnapshot) == 28, "nitro snapshot ABI");
static_assert(sizeof(AffinityHeader) == 224, "nitro affinity header ABI");
static_assert(sizeof(AffinityEvent) == 72, "nitro affinity event ABI");

bool Writable(const std::vector<Mapping>& maps, std::uintptr_t address,
              std::size_t size) {
    const Mapping* mapping = FindMapping(maps, address, size);
    return mapping && mapping->perms[0] == 'r' && mapping->perms[1] == 'w';
}

bool ReadSnapshot(int mem, std::uintptr_t state, NitroSnapshot* output) {
    std::memset(output, 0, sizeof(*output));
    return ReadExact(mem, state + kOptionalOffset, &output->optional_180,
                     sizeof(output->optional_180)) &&
           ReadExact(mem, state + kOptionalValueOffset,
                     &output->optional_value_184,
                     sizeof(output->optional_value_184)) &&
           ReadExact(mem, state + kActiveOffset, &output->active_188,
                     sizeof(output->active_188)) &&
           ReadExact(mem, state + kModeOffset, &output->mode_18c,
                     sizeof(output->mode_18c)) &&
           ReadExact(mem, state + kEncryptedOffset,
                     output->encrypted_1ac_1b4,
                     sizeof(output->encrypted_1ac_1b4)) &&
           ReadExact(mem, state + kGatesOffset, output->gates_1b8_1bc,
                     sizeof(output->gates_1b8_1bc));
}

bool ResolveNitroService(int mem, const std::vector<Mapping>& maps,
                         std::uintptr_t base, std::uintptr_t owner,
                         std::uintptr_t* service_out) {
    std::uintptr_t service = 0;
    std::uintptr_t vtable = 0;
    std::uintptr_t dispatch = 0;
    if (!Writable(maps, owner + kServiceOffset, sizeof(service)) ||
        !ReadExact(mem, owner + kServiceOffset, &service, sizeof(service)) ||
        service == 0 || (service & 7u) != 0 ||
        !Writable(maps, service, sizeof(vtable)) ||
        !ReadExact(mem, service, &vtable, sizeof(vtable)) ||
        vtable != base + kServiceVtableRva ||
        !ReadExact(mem, vtable + kServiceActivateSlot, &dispatch,
                   sizeof(dispatch)) ||
        dispatch != base + kServiceActivateRva)
        return false;
    *service_out = service;
    return true;
}

unsigned long NitroAffinityDr7() {
    // Local write breakpoints. Length encoding: 8=2, 1=0, 4=3, 4=3.
    return 1UL | (1UL << 16) | (2UL << 18) |
           (1UL << 2) | (1UL << 20) |
           (1UL << 4) | (1UL << 24) | (3UL << 26) |
           (1UL << 6) | (1UL << 28) | (3UL << 30);
}

}  // namespace

int A9TAS_NITRO_AFFINITY_ENTRY(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "[MAIN_OBJECT_HEX] [FINAL_OWNER_HEX]\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t main_value = 0, owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        (argc >= 6 && !ParseUnsigned(argv[5], 16, &main_value)) ||
        (argc == 7 && !ParseUnsigned(argv[6], 16, &owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 1000 ||
        duration_value > 60000) {
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

    std::uintptr_t main_object = 0, final_owner = 0;
    if (!ResolveMainObject(pid, base,
                           static_cast<std::uintptr_t>(main_value),
                           &main_object) ||
        !ResolveFinalOwner(pid, base,
                           static_cast<std::uintptr_t>(owner_value),
                           &final_owner)) {
        std::fprintf(stderr, "scheduler/vehicle owner resolution failed\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;
    std::vector<Mapping> maps;
    std::uintptr_t service = 0;
    const std::uintptr_t delta = main_object + kAccumulatorOffset;
    if (!ReadMaps(pid, &maps) ||
        !ResolveNitroService(mem, maps, base, final_owner, &service)) {
        std::fprintf(stderr, "NitroService validation failed\n");
        close(mem);
        return 3;
    }
    const std::uintptr_t state = service + kNitroStateOffset;
    const std::uintptr_t active = state + kActiveOffset;
    const std::uintptr_t mode = state + kModeOffset;
    const std::uintptr_t encrypted = state + kEncryptedOffset;
    if ((delta & 7u) != 0 || (mode & 3u) != 0 ||
        (encrypted & 3u) != 0 || !Writable(maps, delta, 8) ||
        !Writable(maps, active, 1) || !Writable(maps, mode, 4) ||
        !Writable(maps, encrypted, 4) ||
        !Writable(maps, state + kGatesOffset, 5)) {
        std::fprintf(stderr, "nitro affinity watch validation failed\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb+");
    if (!out) {
        close(mem);
        return 5;
    }
    AffinityHeader header{};
    std::memcpy(header.magic, kNitroAffinityMagic,
                sizeof(kNitroAffinityMagic));
    header.version = kNitroAffinityVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(AffinityEvent);
    header.flags = kTargetVerified;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.main_object = main_object;
    header.delta_address = delta;
    header.final_owner = final_owner;
    header.service_object = service;
    header.state_address = state;
    header.start_ns = MonotonicNs();
    if (!ReadSnapshot(mem, state, &header.initial_snapshot) ||
        std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    std::vector<TracedThread> threads;
    std::uint64_t failures = 0;
    const std::size_t initial = AttachNewThreads(
        pid, delta, active, mode, encrypted, &threads, &failures,
        NitroAffinityDr7());
    header.initial_threads = static_cast<std::uint32_t>(initial);
    header.ptrace_errors += failures;
    if (initial == 0 || failures != 0) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        std::fclose(out);
        close(mem);
        return 7;
    }
    std::printf(
        "NITRO_THREAD_AFFINITY_V1 pid=%d main=0x%" PRIxPTR
        " delta=0x%" PRIxPTR " owner=0x%" PRIxPTR
        " service=0x%" PRIxPTR " state=0x%" PRIxPTR
        " threads=%zu duration_ms=%" PRIu64
        " write_scope=debug-registers-only guest_patch=none\n",
        static_cast<int>(pid), main_object, delta, final_owner, service,
        state, threads.size(), duration_ms);
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
                pid, delta, active, mode, encrypted, &threads,
                &new_failures, NitroAffinityDr7());
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
            AffinityEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if (dr6 & 1UL) {
                event.flags |= kHitDelta;
                ++header.delta_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kHitActive;
                ++header.active_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kHitMode;
                ++header.mode_hits;
            }
            if (dr6 & 8UL) {
                event.flags |= kHitEncrypted;
                ++header.encrypted_hits;
            }
            user_regs_struct regs{};
            if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1)
                ++header.ptrace_errors;
            else
                event.rip = static_cast<std::uint64_t>(regs.rip);
            const bool read_ok =
                ReadExact(mem, delta, &event.delta_value,
                          sizeof(event.delta_value)) &&
                ReadSnapshot(mem, state, &event.snapshot);
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
    if (header.delta_hits != 0) header.flags |= kCapturedDelta;
    if (header.active_hits + header.mode_hits + header.encrypted_hits != 0)
        header.flags |= kCapturedNitro;
    if (output_ok && header.read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0 &&
        header.final_threads ==
            header.initial_threads + header.thread_additions)
        header.flags |= kCleanDetach;
    if (!ReadSnapshot(mem, state, &header.final_snapshot))
        ++header.read_errors;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);
    std::printf(
        "NITRO_THREAD_AFFINITY_V1_DONE events=%" PRIu64
        " delta=%" PRIu64 " active=%" PRIu64 " mode=%" PRIu64
        " encrypted=%" PRIu64 " read_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " clean=%u captured_nitro=%u path=%s\n",
        header.event_count, header.delta_hits, header.active_hits,
        header.mode_hits, header.encrypted_hits, header.read_errors,
        header.ptrace_errors, header.unexpected_stops,
        (header.flags & kCleanDetach) != 0,
        (header.flags & kCapturedNitro) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
