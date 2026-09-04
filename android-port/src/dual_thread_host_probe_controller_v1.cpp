#define A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS 250
#define A9TAS_FRAME_PROBE_CONTROLLER_LIBRARY
#include "same_thread_frame_probe_controller_v1.cpp"

#include <chrono>
#include <thread>

namespace {

constexpr const char* kDualHostAck =
    "I_ACCEPT_DUAL_THREAD_HOST_GETTID_PROBE_V1";

bool WaitForStopBounded(pid_t tid, int timeout_ms, int* stop_status) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t got = waitpid(tid, &status, __WALL | WNOHANG);
        if (got == tid) {
            if (stop_status != nullptr) *stop_status = status;
            return WIFSTOPPED(status);
        }
        if (got < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool InterruptAndWaitBounded(pid_t tid, int timeout_ms,
                             int* stop_status = nullptr) {
    // A previous bounded interrupt can time out just before its stop event is
    // delivered. Consume an already-pending stop first, and still wait when a
    // second PTRACE_INTERRUPT reports EIO because the tracee is now stopped.
    int status = 0;
    const pid_t pending = waitpid(tid, &status, __WALL | WNOHANG);
    if (pending == tid) {
        if (stop_status != nullptr) *stop_status = status;
        return WIFSTOPPED(status);
    }
    if (pending < 0 && errno != EINTR) return false;
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1 && errno != EIO)
        return false;
    return WaitForStopBounded(tid, timeout_ms, stop_status);
}

bool IsExpectedInterruptStop(int status) {
    return WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP &&
           (status >> 16) == PTRACE_EVENT_STOP;
}

bool RegistersEqual(pid_t tid, const user_regs_struct& expected) {
    user_regs_struct actual{};
    return ptrace(PTRACE_GETREGS, tid, nullptr, &actual) != -1 &&
           std::memcmp(&actual, &expected, sizeof(actual)) == 0;
}

bool RegistersReadable(pid_t tid) {
    user_regs_struct regs{};
    return ptrace(PTRACE_GETREGS, tid, nullptr, &regs) != -1;
}

bool RestoreRegisters(pid_t tid, const user_regs_struct& expected) {
    if (RegistersEqual(tid, expected)) return true;
    return PokeUserRegs(tid, expected) && RegistersEqual(tid, expected);
}

std::uint64_t SteadyNs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

int DualHostSelftest() {
    std::uint64_t parsed = 0;
    const int interrupt_stop =
        (PTRACE_EVENT_STOP << 16) | (SIGTRAP << 8) | 0x7f;
    const int unrelated_stop = (SIGSTOP << 8) | 0x7f;
    const bool ok = ParseU64("4701", 10, &parsed) && parsed == 4701 &&
                     kWriteByteDr7 == 0x10001UL &&
                     kDirectModeOffset == 0x1378 &&
                     A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS == 250 &&
                     IsExpectedInterruptStop(interrupt_stop) &&
                     !IsExpectedInterruptStop(unrelated_stop) &&
                     std::strcmp(kDualHostAck,
                                "I_ACCEPT_DUAL_THREAD_HOST_GETTID_PROBE_V1") == 0;
    std::printf(
        "DUAL_THREAD_HOST_PROBE_V1_SELFTEST passed=%d stage_timeout_ms=%d\n",
        ok ? 1 : 0, A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) {
        return DualHostSelftest();
    }
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s PID OWNER_HEX WRITER_TID TIMEOUT_MS "
                     "EXPECTED_RIP_BIAS ACK\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, owner_value = 0, writer_value = 0;
    std::uint64_t timeout_value = 0, bias_value = 0;
    if (!ParseU64(argv[1], 10, &pid_value) ||
        !ParseU64(argv[2], 16, &owner_value) ||
        !ParseU64(argv[3], 10, &writer_value) ||
        !ParseU64(argv[4], 10, &timeout_value) ||
        !ParseU64(argv[5], 10, &bias_value) ||
        std::strcmp(argv[6], kDualHostAck) != 0 || pid_value == 0 ||
        owner_value == 0 || writer_value == 0 || timeout_value < 1000 ||
        timeout_value > 15000 || bias_value != 0) {
        std::fprintf(stderr, "invalid or unacknowledged arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const pid_t writer_tid = static_cast<pid_t>(writer_value);
    const auto owner = static_cast<std::uintptr_t>(owner_value);
    const auto direct_address = owner + kDirectModeOffset;
    if (!IsPidTrulyAlive(pid) || ReadTracerPid(pid) != 0) {
        std::fprintf(stderr, "process/tracer precondition failed\n");
        return 3;
    }
    const std::string writer_name = ThreadName(pid, writer_tid);
    if (writer_name.empty() || writer_name == "FrameThread 0") {
        std::fprintf(stderr, "invalid writer thread identity: %s\n",
                     writer_name.c_str());
        return 4;
    }
    const pid_t frame_tid = FindUniqueThreadByName(pid, "FrameThread 0");
    if (frame_tid <= 0 || frame_tid == writer_tid) {
        std::fprintf(stderr, "unique FrameThread 0 unavailable\n");
        return 4;
    }
    std::uint8_t initial_direct = 0xff;
    if (!ValidateOwner(pid, owner, &initial_direct)) {
        std::fprintf(stderr,
                     "owner precondition failed: direct_mode=%u\n",
                     static_cast<unsigned>(initial_direct));
        return 5;
    }
    if (initial_direct != 0) {
        const auto zero_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(500);
        do {
            if (!IsPidTrulyAlive(pid) || ReadTracerPid(pid) != 0 ||
                !ReadByteUnchecked(pid, direct_address, &initial_direct)) {
                std::fprintf(stderr, "direct_mode zero-window wait failed\n");
                return 5;
            }
            if (initial_direct == 0) break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        } while (std::chrono::steady_clock::now() < zero_deadline);
        if (initial_direct != 0) {
            std::fprintf(stderr, "no direct_mode=0 window within 500ms\n");
            return 5;
        }
        std::printf("DUAL_THREAD_HOST_PROBE_V1_ZERO_WINDOW_ACQUIRED\n");
        std::fflush(stdout);
    }
    const std::uintptr_t trap = FindInt3Stub(pid);
    const std::uintptr_t remote_gettid = RemoteSymbolByName(pid, "gettid");
    if (trap == 0 || remote_gettid == 0) {
        std::fprintf(stderr, "host call prerequisite unavailable\n");
        return 6;
    }

    bool writer_seized = false;
    bool writer_stopped = false;
    bool frame_seized = false;
    bool frame_stopped = false;
    bool writer_hit = false;
    bool host_ok = false;
    bool passed = false;
    bool writer_regs_valid = false;
    bool frame_regs_valid = false;
    int watch_hits = 0;
    int unexpected_stops = 0;
    int writer_interrupt_status = 0;
    int frame_interrupt_status = 0;
    std::uint64_t host_tid = 0;
    std::uint64_t writer_hold_ns = 0;
    std::uint64_t hold_started_ns = 0;
    std::uint8_t direct = 0xff;
    user_regs_struct writer_regs{};
    user_regs_struct frame_regs{};
    RemoteCallReport host_report{};
    const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};

    if (ptrace(PTRACE_SEIZE, writer_tid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "writer seize failed errno=%d\n", errno);
        goto cleanup;
    }
    writer_seized = true;
    std::printf(
        "DUAL_THREAD_HOST_PROBE_V1_ARMED pid=%d writer_tid=%d "
        "writer_name=%s frame_tid=%d owner=%p direct=%p timeout_ms=%llu "
        "hold_method=poll_interrupt bias=0 guest_calls=0 activations=0\n",
        pid, writer_tid, writer_name.c_str(), frame_tid,
        reinterpret_cast<void*>(owner), reinterpret_cast<void*>(direct_address),
        static_cast<unsigned long long>(timeout_value));
    std::fflush(stdout);

    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_value);
        while (std::chrono::steady_clock::now() < deadline && watch_hits < 8) {
            direct = 0xff;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!ReadByteUnchecked(pid, direct_address, &direct)) {
                    ++unexpected_stops;
                    break;
                }
                if (direct == 1) break;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (direct != 1) break;
            ++watch_hits;
            writer_interrupt_status = 0;
            writer_stopped = InterruptAndWaitBounded(
                writer_tid, 1000, &writer_interrupt_status);
            if (!writer_stopped ||
                !IsExpectedInterruptStop(writer_interrupt_status)) {
                ++unexpected_stops;
                break;
            }
            direct = 0xff;
            if (ReadByteUnchecked(pid, direct_address, &direct) && direct == 1 &&
                ThreadName(pid, writer_tid) == writer_name &&
                ptrace(PTRACE_GETREGS, writer_tid, nullptr, &writer_regs) != -1) {
                writer_regs_valid = true;
                writer_hit = true;
                hold_started_ns = SteadyNs();
                break;
            }
            if (ptrace(PTRACE_CONT, writer_tid, nullptr, nullptr) == -1) break;
            writer_stopped = false;
        }
    }
    if (!writer_hit) {
        std::fprintf(stderr,
                     "writer direct=1 hold not established hits=%d unexpected=%d\n",
                     watch_hits, unexpected_stops);
        goto cleanup;
    }

    if (ptrace(PTRACE_SEIZE, frame_tid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "FrameThread seize failed errno=%d\n", errno);
        goto cleanup;
    }
    frame_seized = true;
    frame_stopped =
        InterruptAndWaitBounded(frame_tid, 1000, &frame_interrupt_status);
    if (!frame_stopped || !IsExpectedInterruptStop(frame_interrupt_status)) {
        std::fprintf(stderr,
                     "FrameThread interrupt failed/unexpected status=0x%x\n",
                     frame_interrupt_status);
        goto cleanup;
    }
    if (ptrace(PTRACE_GETREGS, frame_tid, nullptr, &frame_regs) == -1 ||
        !ReadByteUnchecked(pid, direct_address, &direct) || direct != 1 ||
        ThreadName(pid, frame_tid) != "FrameThread 0") {
        std::fprintf(stderr,
                     "FrameThread/direct hold validation failed direct=%u\n",
                     static_cast<unsigned>(direct));
        goto cleanup;
    }
    frame_regs_valid = true;

    {
        RemoteCallSession session{};
        if (!RemoteCallSessionInit(frame_tid, trap, &session) ||
            std::memcmp(&frame_regs, &session.original, sizeof(frame_regs)) != 0) {
            std::fprintf(stderr, "FrameThread call snapshot failed\n");
            goto cleanup;
        }
        RcSetRipBias(0);
        host_ok = RemoteCallSessionCall(&session, remote_gettid, zero_args,
                                        &host_tid, &host_report) &&
                  host_tid == static_cast<std::uint64_t>(frame_tid);
        direct = 0xff;
        if (!host_ok || !RegistersEqual(frame_tid, frame_regs) ||
            !ReadByteUnchecked(pid, direct_address, &direct) || direct != 1) {
            std::fprintf(stderr,
                         "dual-thread host gettid failed returned=%llu expected=%d "
                         "result=%s direct=%u\n",
                         static_cast<unsigned long long>(host_tid), frame_tid,
                         RemoteCallResultName(host_report.result),
                         static_cast<unsigned>(direct));
            goto cleanup;
        }
        frame_stopped = true;
    }
    passed = true;

cleanup:
    if (frame_seized) {
        if (!frame_stopped || !RegistersReadable(frame_tid))
            frame_stopped = InterruptAndWaitBounded(frame_tid, 1000);
        const bool frame_restored =
            frame_stopped &&
            (!frame_regs_valid || RestoreRegisters(frame_tid, frame_regs));
        if (!frame_restored) {
            std::fprintf(stderr, "mandatory FrameThread rollback failed\n");
            passed = false;
        }
        if (frame_stopped &&
            ptrace(PTRACE_DETACH, frame_tid, nullptr, nullptr) == -1) {
            std::fprintf(stderr, "FrameThread detach failed errno=%d\n", errno);
            passed = false;
        }
    }
    if (writer_seized) {
        if (!writer_stopped || !RegistersReadable(writer_tid))
            writer_stopped = InterruptAndWaitBounded(writer_tid, 1000);
        const bool writer_restored =
            writer_stopped &&
            (!writer_regs_valid || RestoreRegisters(writer_tid, writer_regs));
        if (!writer_restored) {
            std::fprintf(stderr, "mandatory writer rollback failed\n");
            passed = false;
        }
        if (writer_stopped &&
            ptrace(PTRACE_DETACH, writer_tid, nullptr, nullptr) == -1) {
            std::fprintf(stderr, "writer detach failed errno=%d\n", errno);
            passed = false;
        }
    }
    if (hold_started_ns != 0) writer_hold_ns = SteadyNs() - hold_started_ns;
    usleep(10000);
    const bool alive = IsPidTrulyAlive(pid);
    const int tracer = ReadTracerPid(pid);
    if (!alive || tracer != 0) passed = false;
    std::printf(
        "DUAL_THREAD_HOST_PROBE_V1_RESULT passed=%d pid=%d writer_tid=%d "
        "writer_name=%s frame_tid=%d hits=%d writer_hit=%d host_tid=%llu "
        "host_ok=%d hold_ns=%llu unexpected_stops=%d alive=%d tracer_pid=%d "
        "guest_calls=0 activations=0\n",
        passed ? 1 : 0, pid, writer_tid, writer_name.c_str(), frame_tid,
        watch_hits, writer_hit ? 1 : 0,
        static_cast<unsigned long long>(host_tid), host_ok ? 1 : 0,
        static_cast<unsigned long long>(writer_hold_ns), unexpected_stops,
        alive ? 1 : 0, tracer);
    return passed ? 0 : 1;
}
