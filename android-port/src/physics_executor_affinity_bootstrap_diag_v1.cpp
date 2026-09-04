#define A9TAS_SAME_THREAD_PROBE_CONTROLLER_LIBRARY
#include "same_thread_probe_controller_v1.cpp"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s PID /absolute/bootstrap.so "
                     "/absolute/arm64_payload.so\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const std::string bootstrap_path = argv[2];
    const std::string payload_path = argv[3];
    if (pid <= 0 || bootstrap_path.empty() || payload_path.empty() ||
        bootstrap_path.front() != '/' || payload_path.front() != '/' ||
        !IsPidTrulyAlive(pid) || ReadTracerPid(pid) != 0)
        return 3;

    std::uintptr_t bootstrap_base = 0;
    if (!FindOffsetZeroModuleBase(pid, bootstrap_path, &bootstrap_base) ||
        !HasExactMappedPath(pid, payload_path))
        return 4;
    std::uint64_t status_offset = 0;
    std::uint64_t stage_offset = 0;
    std::uint64_t getter_offset = 0;
    if (!ResolveElfSymbolValue(
            argv[2], "a9tas_bootstrap_same_thread_probe_status",
            &status_offset) ||
        !ResolveElfSymbolValue(argv[2], "a9tas_bootstrap_stage",
                               &stage_offset) ||
        !ResolveElfSymbolValue(
            argv[2], "a9tas_bootstrap_same_thread_probe_trampoline",
            &getter_offset))
        return 5;
    const std::uintptr_t status_fn = bootstrap_base + status_offset;
    const std::uintptr_t stage_fn = bootstrap_base + stage_offset;
    const std::uintptr_t getter_fn = bootstrap_base + getter_offset;
    const pid_t tid = FindUniqueThreadByName(pid, "Signal Catcher");
    const std::uintptr_t trap = FindInt3Stub(pid);
    const std::uintptr_t remote_gettid = RemoteSymbolByName(pid, "gettid");
    if (tid <= 0 || trap == 0 || remote_gettid == 0) return 6;
    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1) return 7;

    bool passed = false;
    std::uint64_t status = UINT64_MAX;
    std::uint64_t stage = UINT64_MAX;
    std::uint64_t trampoline = 0;
    RemoteCallReport status_report{};
    RemoteCallReport stage_report{};
    RemoteCallReport getter_report{};
    int wait_status = 0;
    if (waitpid(tid, &wait_status, __WALL) == tid &&
        WIFSTOPPED(wait_status)) {
        RemoteCallSession session{};
        const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};
        const bool initialized =
            RemoteCallSessionInit(tid, trap, &session) &&
            CalibrateRipBias(&session, remote_gettid);
        const bool status_ok =
            initialized &&
            RemoteCallSessionCall(&session, status_fn, zero_args, &status,
                                  &status_report);
        const bool stage_ok =
            initialized &&
            RemoteCallSessionCall(&session, stage_fn, zero_args, &stage,
                                  &stage_report);
        const bool getter_ok =
            initialized &&
            RemoteCallSessionCall(&session, getter_fn, zero_args, &trampoline,
                                  &getter_report);
        passed = status_ok && stage_ok && getter_ok;
    }
    if (ptrace(PTRACE_DETACH, tid, nullptr, nullptr) == -1) passed = false;
    usleep(10000);
    const bool alive = IsPidTrulyAlive(pid);
    const int tracer = ReadTracerPid(pid);
    std::printf(
        "P1_BOOTSTRAP_DIAG_V1 passed=%d pid=%d tid=%d status=%lld "
        "status_call=%s stage=%lld stage_call=%s trampoline=%p "
        "getter_call=%s guest_calls=0 "
        "alive=%d tracer_pid=%d\n",
        passed && alive && tracer == 0 ? 1 : 0, pid, tid,
        static_cast<long long>(status),
        RemoteCallResultName(status_report.result),
        static_cast<long long>(stage),
        RemoteCallResultName(stage_report.result),
        reinterpret_cast<void*>(trampoline),
        RemoteCallResultName(getter_report.result), alive ? 1 : 0, tracer);
    return passed && alive && tracer == 0 ? 0 : 1;
}
