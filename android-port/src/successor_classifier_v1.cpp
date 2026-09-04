// Strictly read-only four-slot successor phase/call-stack classifier.
//
// DR0: velocity-source RBX owner + 0x1968 (first word, write/4)
// DR1: NativePhysicsBody angular + 0x0C (auxiliary word, write/4)
// DR2: final control owner + 0x0C9C (C9C phase, write/4)
// DR3: active vehicle physics base + 0x0F64 (F64 phase, write/4)
//
// The only writes performed by this program are PTRACE_POKEUSER operations
// that install, acknowledge, and remove x86 hardware debug registers.  The
// game memory descriptor is O_RDONLY; no game value, instruction, or register
// state is changed, no guest method is called, and no input/ESC is sent.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

namespace {

constexpr char kSuccessorMagic[8] = {
    'A', '9', 'S', 'C', 'V', '1', '\0', '\0'};
constexpr std::uint32_t kSuccessorVersion = 1;
constexpr std::uint32_t kStackWordCount = 64;
constexpr std::uintptr_t kRbxFirstWordOffset = 0x1968;
constexpr std::uintptr_t kF64Offset = 0xF64;

// Exact guest return PCs whose stack positions/fingerprints are classified by
// the offline parser.  A HWBP exposes the Houdini host RIP, so these are found
// only in the captured host stack and are never treated as the trap RIP.
constexpr std::uintptr_t kRbxCallerReturnRva = 0x369CC08;
constexpr std::uintptr_t kYawCandidateReturnRva = 0x369E45C;
constexpr std::uintptr_t kRbxInternalAngularReturnRva = 0x369E044;
constexpr int kTrapHardwareBreakpointCode = 4;  // Linux TRAP_HWBKPT.

enum SuccessorHeaderFlags : std::uint32_t {
    kSuccessorClean = 1u << 0,
    kSuccessorTargetVerified = 1u << 1,
    kSuccessorReadOnlyContract = 1u << 2,
};

enum SuccessorEventFlags : std::uint32_t {
    kSuccessorHitRbx = 1u << 0,
    kSuccessorHitAngularAux = 1u << 1,
    kSuccessorHitC9C = 1u << 2,
    kSuccessorHitF64 = 1u << 3,
};

#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint32_t stack_word_count;
    std::uint32_t reserved;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t final_owner;
    std::uint64_t rbx_owner;
    std::uint64_t physics_base;
    std::uint64_t native_body;
    std::uint64_t native_angular;
    std::uint64_t rbx_watch_address;
    std::uint64_t angular_aux_watch_address;
    std::uint64_t c9c_watch_address;
    std::uint64_t f64_watch_address;
    std::uint64_t rbx_caller_return;
    std::uint64_t yaw_candidate_return;
    std::uint64_t rbx_internal_angular_return;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t slot_hits[4];
    std::uint64_t value_read_errors;
    std::uint64_t stack_read_errors;
    std::uint64_t register_read_errors;
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
    std::uint64_t dr6;
    std::uint64_t rip;
    std::uint64_t rsp;
    std::uint64_t rbp;
    std::uint64_t rflags;
    std::uint64_t orig_rax;
    // rax, rbx, rcx, rdx, rsi, rdi, r8..r15.
    std::uint64_t gpr[14];
    std::uint32_t rbx_words[2];
    std::uint32_t angular_words[4];
    std::uint32_t c9c_bits;
    std::uint32_t f64_bits;
    std::uint32_t value_read_ok;
    std::uint32_t stack_read_ok;
    std::uint32_t register_read_ok;
    std::uint32_t reserved;
    std::uint64_t stack_words[kStackWordCount];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 248, "successor classifier header ABI");
static_assert(sizeof(Event) == 744, "successor classifier event ABI");

bool WritableAddress(const std::vector<Mapping>& maps,
                     std::uintptr_t address, std::size_t size) {
    const Mapping* mapping = FindMapping(maps, address, size);
    return mapping && mapping->perms[0] == 'r' && mapping->perms[1] == 'w';
}

bool CheckedAdd(std::uintptr_t base, std::uintptr_t offset,
                std::uintptr_t* output) {
    if (base > UINTPTR_MAX - offset) return false;
    *output = base + offset;
    return true;
}

unsigned long FourWrite4Dr7() {
    // Local enable, write-only (RW=01), four-byte length (LEN=11), all slots.
    return 1UL | (1UL << 16) | (3UL << 18) |
           (1UL << 2) | (1UL << 20) | (3UL << 22) |
           (1UL << 4) | (1UL << 24) | (3UL << 26) |
           (1UL << 6) | (1UL << 28) | (3UL << 30);
}

void CopyRegisters(const user_regs_struct& source, Event* event) {
    event->rip = static_cast<std::uint64_t>(source.rip);
    event->rsp = static_cast<std::uint64_t>(source.rsp);
    event->rbp = static_cast<std::uint64_t>(source.rbp);
    event->rflags = static_cast<std::uint64_t>(source.eflags);
    event->orig_rax = static_cast<std::uint64_t>(source.orig_rax);
    event->gpr[0] = static_cast<std::uint64_t>(source.rax);
    event->gpr[1] = static_cast<std::uint64_t>(source.rbx);
    event->gpr[2] = static_cast<std::uint64_t>(source.rcx);
    event->gpr[3] = static_cast<std::uint64_t>(source.rdx);
    event->gpr[4] = static_cast<std::uint64_t>(source.rsi);
    event->gpr[5] = static_cast<std::uint64_t>(source.rdi);
    event->gpr[6] = static_cast<std::uint64_t>(source.r8);
    event->gpr[7] = static_cast<std::uint64_t>(source.r9);
    event->gpr[8] = static_cast<std::uint64_t>(source.r10);
    event->gpr[9] = static_cast<std::uint64_t>(source.r11);
    event->gpr[10] = static_cast<std::uint64_t>(source.r12);
    event->gpr[11] = static_cast<std::uint64_t>(source.r13);
    event->gpr[12] = static_cast<std::uint64_t>(source.r14);
    event->gpr[13] = static_cast<std::uint64_t>(source.r15);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
            "[FINAL_OWNER_HEX]\n",
            argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t duration_value = 0;
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
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open read-only process memory");
        return 4;
    }

    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base,
                           static_cast<std::uintptr_t>(owner_value),
                           &final_owner)) {
        std::fprintf(stderr, "final-control owner resolution failed\n");
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

    std::uintptr_t rbx_watch = 0;
    std::uintptr_t angular_aux_watch = 0;
    std::uintptr_t c9c_watch = 0;
    std::uintptr_t f64_watch = 0;
    std::uintptr_t rbx_caller_return = 0;
    std::uintptr_t yaw_candidate_return = 0;
    std::uintptr_t rbx_internal_angular_return = 0;
    if (!CheckedAdd(backend.angular_source_base, kRbxFirstWordOffset,
                    &rbx_watch) ||
        !CheckedAdd(backend.native_angular_address, 0x0C,
                    &angular_aux_watch) ||
        !CheckedAdd(final_owner, kC9COffset, &c9c_watch) ||
        !CheckedAdd(vehicle.physics_base, kF64Offset, &f64_watch) ||
        !CheckedAdd(base, kRbxCallerReturnRva, &rbx_caller_return) ||
        !CheckedAdd(base, kYawCandidateReturnRva, &yaw_candidate_return) ||
        !CheckedAdd(base, kRbxInternalAngularReturnRva,
                    &rbx_internal_angular_return)) {
        std::fprintf(stderr, "watch/marker address overflow\n");
        close(mem);
        return 3;
    }

    std::vector<Mapping> maps;
    if ((rbx_watch & 3u) != 0 || (angular_aux_watch & 3u) != 0 ||
        (c9c_watch & 3u) != 0 || (f64_watch & 3u) != 0 ||
        !ReadMaps(pid, &maps) || !WritableAddress(maps, rbx_watch, 8) ||
        !WritableAddress(maps, backend.native_angular_address, 16) ||
        !WritableAddress(maps, c9c_watch, 4) ||
        !WritableAddress(maps, f64_watch, 4)) {
        std::fprintf(stderr, "watch address validation failed\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }
    Header header{};
    std::memcpy(header.magic, kSuccessorMagic, sizeof(kSuccessorMagic));
    header.version = kSuccessorVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(Event);
    header.flags = kSuccessorTargetVerified | kSuccessorReadOnlyContract;
    header.stack_word_count = kStackWordCount;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.final_owner = final_owner;
    header.rbx_owner = backend.angular_source_base;
    header.physics_base = vehicle.physics_base;
    header.native_body = backend.native_body;
    header.native_angular = backend.native_angular_address;
    header.rbx_watch_address = rbx_watch;
    header.angular_aux_watch_address = angular_aux_watch;
    header.c9c_watch_address = c9c_watch;
    header.f64_watch_address = f64_watch;
    header.rbx_caller_return = rbx_caller_return;
    header.yaw_candidate_return = yaw_candidate_return;
    header.rbx_internal_angular_return = rbx_internal_angular_return;
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
        pid, rbx_watch, angular_aux_watch, c9c_watch, f64_watch, &threads,
        &failures, FourWrite4Dr7());
    header.initial_threads = static_cast<std::uint32_t>(initial);
    header.ptrace_errors += failures;
    if (initial == 0) {
        std::fclose(out);
        close(mem);
        return 7;
    }
    std::printf(
        "SUCCESSOR_CLASSIFIER_V1 pid=%d owner=0x%" PRIxPTR
        " rbx=0x%" PRIxPTR " angular_aux=0x%" PRIxPTR
        " c9c=0x%" PRIxPTR " f64=0x%" PRIxPTR
        " threads=%zu duration_ms=%" PRIu64
        " game_pwrite=0 guest_calls=0 auto_esc=0 "
        "debug_register_writes_only=1\n",
        static_cast<int>(pid), final_owner, rbx_watch, angular_aux_watch,
        c9c_watch, f64_watch, threads.size(), duration_ms);
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
                pid, rbx_watch, angular_aux_watch, c9c_watch, f64_watch,
                &threads, &new_failures, FourWrite4Dr7());
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
        const unsigned int ptrace_event =
            static_cast<unsigned int>(status) >> 16;
        siginfo_t signal_info{};
        const bool have_signal_info =
            signal == SIGTRAP &&
            ptrace(PTRACE_GETSIGINFO, tid, nullptr, &signal_info) != -1;
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        const bool hardware_breakpoint =
            signal == SIGTRAP && ptrace_event == 0 && have_signal_info &&
            signal_info.si_signo == SIGTRAP &&
            signal_info.si_code == kTrapHardwareBreakpointCode && have_dr6 &&
            (dr6 & 15UL);
        if (hardware_breakpoint) {
            Event event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            event.dr6 = static_cast<std::uint64_t>(dr6);
            for (std::size_t slot = 0; slot < 4; ++slot) {
                if ((dr6 & (1UL << slot)) == 0) continue;
                event.flags |= 1u << slot;
                ++header.slot_hits[slot];
            }

            user_regs_struct registers{};
            const bool registers_ok =
                ptrace(PTRACE_GETREGS, tid, nullptr, &registers) != -1;
            event.register_read_ok = registers_ok ? 1u : 0u;
            if (!registers_ok) {
                ++header.register_read_errors;
                ++header.ptrace_errors;
            } else {
                CopyRegisters(registers, &event);
            }

            const bool values_ok =
                ReadExact(mem, rbx_watch, event.rbx_words,
                          sizeof(event.rbx_words)) &&
                ReadExact(mem, backend.native_angular_address,
                          event.angular_words, sizeof(event.angular_words)) &&
                ReadExact(mem, c9c_watch, &event.c9c_bits,
                          sizeof(event.c9c_bits)) &&
                ReadExact(mem, f64_watch, &event.f64_bits,
                          sizeof(event.f64_bits));
            event.value_read_ok = values_ok ? 1u : 0u;
            if (!values_ok) ++header.value_read_errors;

            const bool stack_ok =
                registers_ok &&
                ReadExact(mem, static_cast<std::uintptr_t>(registers.rsp),
                          event.stack_words, sizeof(event.stack_words));
            event.stack_read_ok = stack_ok ? 1u : 0u;
            if (!stack_ok) ++header.stack_read_errors;

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
            // PTRACE_EVENT_STOP is generated by our own PTRACE_INTERRUPT and
            // is not a tracee signal.  A real non-HWBP SIGTRAP has event 0;
            // re-deliver it unchanged instead of silently swallowing it.
            const bool ptrace_stop =
                signal == SIGTRAP && ptrace_event == PTRACE_EVENT_STOP;
            const int deliver = ptrace_stop ? 0 : signal;
            if (!ContinueThread(tid, deliver)) {
                ++header.ptrace_errors;
            } else if (tracked) {
                tracked->stopped = false;
            }
        }
    }

    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped)) {
            ++header.ptrace_errors;
        } else {
            ++accounted_threads;
        }
        thread.live = false;
        thread.stopped = false;
    }
    header.final_threads = accounted_threads;
    if (output_ok && header.event_count > 0 &&
        header.value_read_errors == 0 && header.stack_read_errors == 0 &&
        header.register_read_errors == 0 && header.ptrace_errors == 0 &&
        header.unexpected_stops == 0 &&
        header.final_threads ==
            header.initial_threads + header.thread_additions) {
        header.flags |= kSuccessorClean;
    }
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out)) {
        output_ok = false;
    }
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);
    std::printf(
        "SUCCESSOR_CLASSIFIER_V1_DONE events=%" PRIu64
        " dr0=%" PRIu64 " dr1=%" PRIu64 " dr2=%" PRIu64
        " dr3=%" PRIu64 " value_errors=%" PRIu64
        " stack_errors=%" PRIu64 " register_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " clean=%u game_pwrite=0 guest_calls=0 auto_esc=0 path=%s\n",
        header.event_count, header.slot_hits[0], header.slot_hits[1],
        header.slot_hits[2], header.slot_hits[3], header.value_read_errors,
        header.stack_read_errors, header.register_read_errors,
        header.ptrace_errors, header.unexpected_stops,
        (header.flags & kSuccessorClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
