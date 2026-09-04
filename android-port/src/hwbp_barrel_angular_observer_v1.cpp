// Host-only, observation-only classifier for the separate barrel/angular path.
//
// DR0: player NativePhysicsBody angular +0x0 (write/4)
// DR1: player NativePhysicsBody angular +0x8 (write/4)
// DR2: player NativePhysicsBody angular +0xC auxiliary word (write/4)
//
// Every hit records the complete 16-byte angular storage and 32 host stack
// words.  Houdini exposes active guest return PCs in this stack view, allowing
// the parser to distinguish the final stabilization-setter return at 0x369E45C
// from worker-phase and unrelated angular writers.  The earlier 0x369BC7C
// setter is part of the main stunt-state update, while the later call is in the
// separate post-update stabilization chain that structurally matches the
// upstream BarrelYaw hook.  No game value or guest instruction is modified,
// and no guest function is invoked or single-stepped.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

namespace {

constexpr char kBarrelMagic[8] = {
    'A', '9', 'B', 'A', 'V', '1', '\0', '\0'};
constexpr std::uint32_t kBarrelVersion = 1;
constexpr std::uintptr_t kCandidateBranchRva = 0x369E44C;
constexpr std::uintptr_t kCandidateReturnRva = 0x369E45C;
constexpr std::uintptr_t kAngularThunkRva = 0x36A5FE4;
constexpr std::uintptr_t kAngularSetterRva = 0x4CC5C34;
constexpr std::uintptr_t kAngularGetterRva = 0x4CC5C64;
constexpr std::uintptr_t kWorkerPhaseReturnRvas[3] = {
    0x5E080A0, 0x5E080AC, 0x5E080B8};

constexpr std::uint8_t kCandidateBranchSignature[16] = {
    0xe8, 0x02, 0x40, 0xf9, 0xe1, 0x83, 0x01, 0x91,
    0xe0, 0x03, 0x17, 0xaa, 0xe3, 0x1e, 0x00, 0x94,
};
constexpr std::uint8_t kAngularThunkSignature[8] = {
    0x08, 0x51, 0x40, 0xf9, 0x00, 0x01, 0x1f, 0xd6,
};
constexpr std::uint8_t kAngularSetterSignature[16] = {
    0x08, 0x48, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xf9,
    0x2a, 0x08, 0x40, 0xb9, 0x0b, 0x81, 0x05, 0x91,
};
constexpr std::uint8_t kAngularGetterSignature[16] = {
    0x08, 0x48, 0x40, 0xf9, 0x00, 0x61, 0x41, 0xbd,
    0x01, 0x65, 0x41, 0xbd, 0x02, 0x69, 0x41, 0xbd,
};

enum BarrelHeaderFlags : std::uint32_t {
    kBarrelClean = 1u << 0,
    kBarrelTargetVerified = 1u << 1,
};

enum BarrelEventFlags : std::uint32_t {
    kBarrelHitAngularHead = 1u << 0,
    kBarrelHitAngularTail = 1u << 1,
    kBarrelHitAngularAux = 1u << 2,
};

#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t native_body;
    std::uint64_t angular_address;
    std::uint64_t angular_tail_address;
    std::uint64_t angular_aux_address;
    std::uint64_t candidate_return;
    std::uint64_t worker_phase_return_0;
    std::uint64_t worker_phase_return_1;
    std::uint64_t worker_phase_return_2;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t angular_head_hits;
    std::uint64_t angular_tail_hits;
    std::uint64_t angular_aux_hits;
    std::uint64_t value_read_errors;
    std::uint64_t stack_read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t thread_additions;
    std::uint64_t unexpected_stops;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
};

struct Event {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::uint64_t rsp;
    std::uint32_t angular_bits[4];
    std::uint32_t value_read_ok;
    std::uint32_t stack_read_ok;
    std::uint64_t stack_words[32];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 192, "barrel-angular header ABI");
static_assert(sizeof(Event) == 320, "barrel-angular event ABI");

bool WritableAddress(const std::vector<Mapping>& maps,
                     std::uintptr_t address, std::size_t size) {
    const Mapping* map = FindMapping(maps, address, size);
    return map && map->perms[0] == 'r' && map->perms[1] == 'w';
}

template <std::size_t N>
bool VerifySignature(int mem, std::uintptr_t base, std::uintptr_t rva,
                     const std::uint8_t (&expected)[N]) {
    if (base > UINTPTR_MAX - rva) return false;
    std::uint8_t actual[N]{};
    return ReadExact(mem, base + rva, actual, sizeof(actual)) &&
           std::memcmp(actual, expected, sizeof(actual)) == 0;
}

unsigned long AngularDr7() {
    // Local DR0/DR1/DR2, write-only, four-byte length. DR3 remains disabled.
    return 1UL | (1UL << 16) | (3UL << 18) |
           (1UL << 2) | (1UL << 20) | (3UL << 22) |
           (1UL << 4) | (1UL << 24) | (3UL << 26);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
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
    if (!VerifySignature(mem, base, kCandidateBranchRva,
                         kCandidateBranchSignature) ||
        !VerifySignature(mem, base, kAngularThunkRva,
                         kAngularThunkSignature) ||
        !VerifySignature(mem, base, kAngularSetterRva,
                         kAngularSetterSignature) ||
        !VerifySignature(mem, base, kAngularGetterRva,
                         kAngularGetterSignature)) {
        std::fprintf(stderr, "barrel/angular target signature mismatch\n");
        close(mem);
        return 3;
    }

    a9tas::vehicle_state_v1::Layout vehicle{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                       &backend)) {
        std::fprintf(stderr, "vehicle/backend resolution failed at stage=%u\n",
                     backend.failure_stage);
        close(mem);
        return 3;
    }
    const std::uintptr_t angular = backend.native_angular_address;
    if (angular > UINTPTR_MAX - 16) {
        close(mem);
        return 3;
    }
    const std::uintptr_t angular_tail = angular + 8;
    const std::uintptr_t angular_aux = angular + 12;
    std::vector<Mapping> maps;
    if ((angular & 3u) != 0 || !ReadMaps(pid, &maps) ||
        !WritableAddress(maps, angular, 16)) {
        std::fprintf(stderr, "angular watch address validation failed\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }
    Header header{};
    std::memcpy(header.magic, kBarrelMagic, sizeof(kBarrelMagic));
    header.version = kBarrelVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(Event);
    header.flags = kBarrelTargetVerified;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.native_body = backend.native_body;
    header.angular_address = angular;
    header.angular_tail_address = angular_tail;
    header.angular_aux_address = angular_aux;
    header.candidate_return = base + kCandidateReturnRva;
    header.worker_phase_return_0 = base + kWorkerPhaseReturnRvas[0];
    header.worker_phase_return_1 = base + kWorkerPhaseReturnRvas[1];
    header.worker_phase_return_2 = base + kWorkerPhaseReturnRvas[2];
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
        pid, angular, angular_tail, angular_aux, 0, &threads, &failures,
        AngularDr7());
    header.initial_threads = static_cast<std::uint32_t>(initial);
    header.ptrace_errors += failures;
    if (initial == 0) {
        std::fclose(out);
        close(mem);
        return 7;
    }
    std::printf(
        "BARREL_ANGULAR_V1 pid=%d native=0x%" PRIxPTR
        " angular=0x%" PRIxPTR " tail=0x%" PRIxPTR
        " aux=0x%" PRIxPTR " candidate_return=0x%" PRIx64
        " threads=%zu duration_ms=%" PRIu64
        " write_scope=debug-registers-only guest_patch=none\n",
        static_cast<int>(pid), backend.native_body, angular, angular_tail,
        angular_aux, header.candidate_return, threads.size(), duration_ms);
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
                pid, angular, angular_tail, angular_aux, 0, &threads,
                &new_failures, AngularDr7());
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
        if (signal == SIGTRAP && have_dr6 && (dr6 & 7UL)) {
            Event event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if (dr6 & 1UL) {
                event.flags |= kBarrelHitAngularHead;
                ++header.angular_head_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kBarrelHitAngularTail;
                ++header.angular_tail_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kBarrelHitAngularAux;
                ++header.angular_aux_hits;
            }
            user_regs_struct registers{};
            const bool have_registers =
                ptrace(PTRACE_GETREGS, tid, nullptr, &registers) != -1;
            if (!have_registers) {
                ++header.ptrace_errors;
            } else {
                event.rip = static_cast<std::uint64_t>(registers.rip);
                event.rsp = static_cast<std::uint64_t>(registers.rsp);
            }
            const bool values_ok =
                ReadExact(mem, angular, event.angular_bits,
                          sizeof(event.angular_bits));
            event.value_read_ok = values_ok ? 1u : 0u;
            if (!values_ok) ++header.value_read_errors;
            const bool stack_ok = have_registers &&
                ReadExact(mem, static_cast<std::uintptr_t>(registers.rsp),
                          event.stack_words, sizeof(event.stack_words));
            event.stack_read_ok = stack_ok ? 1u : 0u;
            if (!stack_ok) ++header.stack_read_errors;
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
    if (output_ok && header.event_count > 0 &&
        header.value_read_errors == 0 && header.stack_read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0 &&
        header.final_threads ==
            header.initial_threads + header.thread_additions)
        header.flags |= kBarrelClean;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);
    std::printf(
        "BARREL_ANGULAR_V1_DONE events=%" PRIu64
        " head=%" PRIu64 " tail=%" PRIu64 " aux=%" PRIu64
        " value_errors=%" PRIu64 " stack_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " clean=%u path=%s\n",
        header.event_count, header.angular_head_hits,
        header.angular_tail_hits, header.angular_aux_hits,
        header.value_read_errors, header.stack_read_errors,
        header.ptrace_errors, header.unexpected_stops,
        (header.flags & kBarrelClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
