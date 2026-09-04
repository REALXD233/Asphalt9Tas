#define A9TAS_SAME_THREAD_PROBE_CONTROLLER_LIBRARY
#include "same_thread_probe_controller_v1.cpp"

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr const char* kAck =
    "I_ACCEPT_FRAME_THREAD_ZERO_SIDE_EFFECT_PROBE_V1";
constexpr const char* kBootstrapPath =
    "/data/local/tmp/liba9tas_bootstrap_same_thread_probe_v1.so";
constexpr const char* kPayloadPath =
    "/data/local/tmp/liba9tas_same_thread_probe_v1.so";
constexpr std::uintptr_t kDirectModeOffset = 0x1378;
constexpr std::uintptr_t kInterfaceOffset = 0x30;
constexpr std::uintptr_t kQueueOffset = 0x1360;
constexpr unsigned long kWriteByteDr7 = 1UL | (1UL << 16);

struct QueueSnapshot {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uintptr_t capacity{};
};

struct SavedDebug {
    unsigned long dr0{};
    unsigned long dr6{};
    unsigned long dr7{};
    bool valid{};
};

bool ParseU64(const char* text, int base, std::uint64_t* value) {
    if (text == nullptr || *text == '\0' || *text == '-' || value == nullptr)
        return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ReadExactUnchecked(pid_t pid, std::uintptr_t address, void* output,
                        std::size_t size) {
    return ReadProcessMemoryUnchecked(pid, address, output, size);
}

bool ReadByteUnchecked(pid_t pid, std::uintptr_t address, std::uint8_t* value) {
    return ReadExactUnchecked(pid, address, value, sizeof(*value));
}

bool QueueSane(const QueueSnapshot& queue) {
    if (queue.begin == 0 && queue.end == 0 && queue.capacity == 0) return true;
    return queue.begin != 0 && queue.begin <= queue.end &&
           queue.end <= queue.capacity &&
           ((queue.end - queue.begin) % sizeof(std::uintptr_t) == 0);
}

bool ValidateOwner(pid_t pid, std::uintptr_t owner, std::uint8_t* direct_mode) {
    std::uintptr_t interface_pointer = 0;
    QueueSnapshot queue{};
    if (!ReadExactUnchecked(pid, owner + kInterfaceOffset, &interface_pointer,
                            sizeof(interface_pointer)) ||
        !ReadExactUnchecked(pid, owner + kQueueOffset, &queue, sizeof(queue)) ||
        !ReadByteUnchecked(pid, owner + kDirectModeOffset, direct_mode) ||
        interface_pointer == 0 || *direct_mode > 1 || !QueueSane(queue)) {
        return false;
    }
    Mapping interface_mapping{};
    return FindMapping(pid, interface_pointer, &interface_mapping) &&
           interface_mapping.readable;
}

bool PeekDebug(pid_t tid, int index, unsigned long* value) {
    if (value == nullptr) return false;
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    errno = 0;
    const long raw = ptrace(PTRACE_PEEKUSER, tid,
                            reinterpret_cast<void*>(offset), nullptr);
    if (raw == -1 && errno != 0) return false;
    *value = static_cast<unsigned long>(raw);
    return true;
}

bool PokeDebug(pid_t tid, int index, unsigned long value) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    return ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset),
                  reinterpret_cast<void*>(value)) != -1;
}

bool SaveDebug(pid_t tid, SavedDebug* saved) {
    if (saved == nullptr) return false;
    saved->valid = PeekDebug(tid, 0, &saved->dr0) &&
                   PeekDebug(tid, 6, &saved->dr6) &&
                   PeekDebug(tid, 7, &saved->dr7);
    return saved->valid;
}

bool RestoreDebug(pid_t tid, const SavedDebug& saved) {
    if (!saved.valid) return false;
    if (!PokeDebug(tid, 7, 0) || !PokeDebug(tid, 0, saved.dr0) ||
        !PokeDebug(tid, 6, 0) || !PokeDebug(tid, 7, saved.dr7)) {
        return false;
    }
    unsigned long dr0 = 0, dr6 = 0, dr7 = 0;
    const bool read_ok = PeekDebug(tid, 0, &dr0) &&
                         PeekDebug(tid, 6, &dr6) &&
                         PeekDebug(tid, 7, &dr7);
    const bool restored = read_ok && dr0 == saved.dr0 &&
                          (dr6 & 0xFUL) == 0 && dr7 == saved.dr7;
    if (!restored) {
        std::fprintf(stderr,
                     "debug restore readback saved=(0x%lx,0x%lx,0x%lx) "
                     "actual=(0x%lx,0x%lx,0x%lx) read_ok=%d\n",
                     saved.dr0, saved.dr6, saved.dr7, dr0, dr6, dr7,
                     read_ok ? 1 : 0);
    }
    return restored;
}

bool DisableProbeDebug(pid_t tid) {
    if (!PokeDebug(tid, 7, 0) || !PokeDebug(tid, 0, 0) ||
        !PokeDebug(tid, 6, 0)) {
        return false;
    }
    unsigned long dr0 = 1, dr7 = 1;
    return PeekDebug(tid, 0, &dr0) && PeekDebug(tid, 7, &dr7) &&
           dr0 == 0 && dr7 == 0;
}

bool InterruptAndWait(pid_t tid) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) return false;
    int status = 0;
    return waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
}

int OfflineSelftest() {
    QueueSnapshot empty{};
    QueueSnapshot ordered{0x1000, 0x1010, 0x1020};
    QueueSnapshot reversed{0x1020, 0x1010, 0x1000};
    std::uint64_t parsed = 0;
    const bool ok = kWriteByteDr7 == 0x10001UL && QueueSane(empty) &&
                    QueueSane(ordered) && !QueueSane(reversed) &&
                    ParseU64("7ffff4533560", 16, &parsed) &&
                    parsed == 0x7ffff4533560ULL &&
                    !ParseU64("-1", 16, &parsed) &&
                    kDirectModeOffset == 0x1378;
    std::printf("FRAME_THREAD_PROBE_V1_SELFTEST passed=%d dr7=0x%lx\n",
                ok ? 1 : 0, kWriteByteDr7);
    return ok ? 0 : 1;
}

}  // namespace

#ifndef A9TAS_FRAME_PROBE_CONTROLLER_LIBRARY
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) {
        return OfflineSelftest();
    }
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s PID TRAMPOLINE_HEX OWNER_HEX TIMEOUT_MS "
                     "EXPECTED_RIP_BIAS ACK\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, trampoline_value = 0, owner_value = 0;
    std::uint64_t timeout_value = 0, bias_value = 0;
    if (!ParseU64(argv[1], 10, &pid_value) ||
        !ParseU64(argv[2], 16, &trampoline_value) ||
        !ParseU64(argv[3], 16, &owner_value) ||
        !ParseU64(argv[4], 10, &timeout_value) ||
        !ParseU64(argv[5], 10, &bias_value) ||
        std::strcmp(argv[6], kAck) != 0 || pid_value == 0 ||
        trampoline_value == 0 || owner_value == 0 || timeout_value < 1000 ||
        timeout_value > 15000 || bias_value != 0) {
        std::fprintf(stderr, "invalid or unacknowledged arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto trampoline = static_cast<std::uintptr_t>(trampoline_value);
    const auto owner = static_cast<std::uintptr_t>(owner_value);
    const auto direct_address = owner + kDirectModeOffset;
    if (!IsPidTrulyAlive(pid) || ReadTracerPid(pid) != 0 ||
        !HasExactMappedPath(pid, kBootstrapPath) ||
        !HasExactMappedPath(pid, kPayloadPath)) {
        std::fprintf(stderr, "process/module/tracer precondition failed\n");
        return 3;
    }
    Mapping trampoline_mapping{};
    if (!FindMapping(pid, trampoline, &trampoline_mapping) ||
        !trampoline_mapping.readable || !trampoline_mapping.executable) {
        std::fprintf(stderr, "stale or non-executable trampoline\n");
        return 4;
    }
    std::uint8_t initial_direct = 0xff;
    if (!ValidateOwner(pid, owner, &initial_direct) || initial_direct != 0) {
        std::fprintf(stderr,
                     "owner precondition failed or direct_mode not zero: %u\n",
                     static_cast<unsigned>(initial_direct));
        return 5;
    }
    const pid_t tid = FindUniqueThreadByName(pid, "FrameThread 0");
    const std::uintptr_t trap = FindInt3Stub(pid);
    const std::uintptr_t remote_gettid = RemoteSymbolByName(pid, "gettid");
    if (tid <= 0 || trap == 0 || remote_gettid == 0) {
        std::fprintf(stderr,
                     "FrameThread/controller prerequisite unavailable tid=%d "
                     "trap=%p gettid=%p\n",
                     tid, reinterpret_cast<void*>(trap),
                     reinterpret_cast<void*>(remote_gettid));
        return 6;
    }

    bool seized = false;
    bool stopped = false;
    bool passed = false;
    bool accepted_hit = false;
    bool host_tid_ok = false;
    bool guest_tid_ok = false;
    int watch_hits = 0;
    int unexpected_stops = 0;
    std::uint64_t host_tid = 0;
    std::uint64_t guest_tid = 0;
    SavedDebug saved_debug{};
    RemoteCallReport host_report{};
    RemoteCallReport guest_report{};
    const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};

    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "seize failed tid=%d errno=%d\n", tid, errno);
        goto cleanup;
    }
    seized = true;
    if (!InterruptAndWait(tid)) {
        std::fprintf(stderr, "seize/interrupt failed tid=%d errno=%d\n", tid,
                     errno);
        goto cleanup;
    }
    stopped = true;
    if (!SaveDebug(tid, &saved_debug) || saved_debug.dr0 != 0 ||
        saved_debug.dr7 != 0) {
        std::fprintf(stderr,
                     "debug-register precondition failed dr0=0x%lx dr7=0x%lx\n",
                     saved_debug.dr0, saved_debug.dr7);
        goto cleanup;
    }
    if (!PokeDebug(tid, 7, 0) ||
        !PokeDebug(tid, 0, static_cast<unsigned long>(direct_address)) ||
        !PokeDebug(tid, 6, 0) || !PokeDebug(tid, 7, kWriteByteDr7)) {
        std::fprintf(stderr, "watchpoint programming failed\n");
        goto cleanup;
    }
    if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "initial continue failed\n");
        goto cleanup;
    }
    stopped = false;
    std::printf(
        "FRAME_THREAD_PROBE_V1_ARMED pid=%d tid=%d owner=%p direct=%p "
        "trampoline=%p timeout_ms=%llu bias=0\n",
        pid, tid, reinterpret_cast<void*>(owner),
        reinterpret_cast<void*>(direct_address),
        reinterpret_cast<void*>(trampoline),
        static_cast<unsigned long long>(timeout_value));
    std::fflush(stdout);

    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_value);
        while (std::chrono::steady_clock::now() < deadline && watch_hits < 8) {
            int status = 0;
            const pid_t got = waitpid(tid, &status, __WALL | WNOHANG);
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (got != tid || !WIFSTOPPED(status)) break;
            stopped = true;
            const int signal = WSTOPSIG(status);
            unsigned long dr6 = 0;
            if (signal == SIGTRAP && PeekDebug(tid, 6, &dr6) &&
                (dr6 & 1UL) != 0) {
                ++watch_hits;
                std::uint8_t direct = 0xff;
                const bool read_ok = ReadByteUnchecked(pid, direct_address,
                                                       &direct);
                if (read_ok && direct == 1 &&
                    ThreadName(pid, tid) == "FrameThread 0") {
                    accepted_hit = true;
                    break;
                }
                if (!PokeDebug(tid, 6, 0) ||
                    ptrace(PTRACE_CONT, tid, nullptr, nullptr) == -1) {
                    break;
                }
                stopped = false;
                continue;
            }
            ++unexpected_stops;
            break;
        }
    }
    if (!accepted_hit) {
        std::fprintf(stderr,
                     "no accepted direct_mode=1 hit hits=%d unexpected=%d\n",
                     watch_hits, unexpected_stops);
        goto cleanup;
    }
    if (!DisableProbeDebug(tid)) {
        std::fprintf(stderr, "failed to disable watchpoint before calls\n");
        goto cleanup;
    }
    {
        std::uint8_t direct = 0xff;
        user_regs_struct hit_regs{};
        RemoteCallSession session{};
        if (!ReadByteUnchecked(pid, direct_address, &direct) || direct != 1 ||
            ptrace(PTRACE_GETREGS, tid, nullptr, &hit_regs) == -1 ||
            !RemoteCallSessionInit(tid, trap, &session) ||
            std::memcmp(&hit_regs, &session.original, sizeof(hit_regs)) != 0) {
            std::fprintf(stderr, "accepted-hit snapshot failed\n");
            goto cleanup;
        }
        RcSetRipBias(0);
        host_tid_ok = RemoteCallSessionCall(&session, remote_gettid, zero_args,
                                            &host_tid, &host_report) &&
                      host_tid == static_cast<std::uint64_t>(tid);
        direct = 0xff;
        if (!host_tid_ok ||
            !ReadByteUnchecked(pid, direct_address, &direct) || direct != 1) {
            std::fprintf(stderr,
                         "FrameThread host gettid failed returned=%llu expected=%d "
                         "result=%s direct=%u\n",
                         static_cast<unsigned long long>(host_tid), tid,
                         RemoteCallResultName(host_report.result),
                         static_cast<unsigned>(direct));
            stopped = host_report.stop_confirmed;
            goto cleanup;
        }
        guest_tid_ok = RemoteCallSessionCall(
                           &session, trampoline, zero_args, &guest_tid,
                           &guest_report) &&
                       guest_tid == static_cast<std::uint64_t>(tid);
        direct = 0xff;
        if (!guest_tid_ok ||
            !ReadByteUnchecked(pid, direct_address, &direct) || direct != 1) {
            std::fprintf(stderr,
                         "FrameThread guest gettid failed returned=%llu expected=%d "
                         "result=%s signal=%d direct=%u\n",
                         static_cast<unsigned long long>(guest_tid), tid,
                         RemoteCallResultName(guest_report.result),
                         guest_report.stop_signal,
                         static_cast<unsigned>(direct));
            stopped = guest_report.stop_confirmed;
            goto cleanup;
        }
        stopped = true;
        passed = true;
    }

cleanup:
    if (seized) {
        if (!stopped) stopped = InterruptAndWait(tid);
        if (!stopped || !RestoreDebug(tid, saved_debug)) {
            std::fprintf(stderr, "mandatory debug-register restoration failed\n");
            passed = false;
        }
        if (stopped && ptrace(PTRACE_DETACH, tid, nullptr, nullptr) == -1) {
            std::fprintf(stderr, "detach failed tid=%d errno=%d\n", tid, errno);
            passed = false;
        }
    }
    usleep(10000);
    const bool alive = IsPidTrulyAlive(pid);
    const int tracer = ReadTracerPid(pid);
    if (!alive || tracer != 0) passed = false;
    std::printf(
        "FRAME_THREAD_PROBE_V1_RESULT passed=%d pid=%d tid=%d hits=%d "
        "accepted=%d host_tid=%llu host_ok=%d guest_tid=%llu guest_ok=%d "
        "unexpected_stops=%d alive=%d tracer_pid=%d activations=0\n",
        passed ? 1 : 0, pid, tid, watch_hits, accepted_hit ? 1 : 0,
        static_cast<unsigned long long>(host_tid), host_tid_ok ? 1 : 0,
        static_cast<unsigned long long>(guest_tid), guest_tid_ok ? 1 : 0,
        unexpected_stops, alive ? 1 : 0, tracer);
    return passed ? 0 : 1;
}
#endif
