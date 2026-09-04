// Short live gate for the passive Nitro recording wrapper.
//
// The gate freezes the complete target thread set only while it installs or
// removes the shadow service vptr.  No hardware breakpoint remains armed while
// the user performs one natural Nitro input.  This isolates wrapper safety from
// the synchronized 900-frame ptrace recorder.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "natural_action_recording_host_v1.h"

#include <cerrno>
#include <cinttypes>
#include <climits>

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_ONE_PASSIVE_NITRO_RECORDING_SMOKE_V1";

bool FreezeStable(pid_t pid, std::vector<TracedThread>* threads,
                  std::uint64_t* failures, std::uint32_t* passes) {
    if (!threads || !failures || !passes) return false;
    *passes = 0;
    for (std::uint32_t pass = 1; pass <= 6; ++pass) {
        for (auto& thread : *threads) {
            if (!thread.live || thread.stopped) continue;
            if (!StopThread(thread.tid)) {
                ++*failures;
                return false;
            }
            thread.stopped = true;
        }
        const std::size_t added = AttachNewThreadsStopped(
            pid, 0, 0, 0, 0, threads, failures, 0);
        *passes = pass;
        if (*failures != 0) return false;
        if (added == 0 && pass >= 2) return !threads->empty();
    }
    return false;
}

bool DetachAll(std::vector<TracedThread>* threads) {
    if (!threads) return false;
    bool ok = true;
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped)) ok = false;
        thread.live = false;
        thread.stopped = false;
    }
    return ok;
}

bool CreateReady(const char* path, pid_t pid, std::uint32_t passes,
                 std::size_t threads) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    char text[256]{};
    const int length = std::snprintf(
        text, sizeof(text),
        "NITRO_RECORDING_SMOKE_READY pid=%d controller_pid=%d "
        "threads=%zu freeze_passes=%u service_vptr_swaps=1 "
        "hardware_breakpoints=0 host_resume_gate=marker_removal\n",
        static_cast<int>(pid), static_cast<int>(getpid()), threads, passes);
    bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(text);
    std::size_t written = 0;
    while (ok && written < static_cast<std::size_t>(length)) {
        const ssize_t put = write(fd, text + written,
                                  static_cast<std::size_t>(length) - written);
        if (put <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(put);
    }
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

bool WaitForRelease(const char* path, std::uint64_t timeout_ms) {
    const std::uint64_t deadline = MonotonicNs() + timeout_ms * 1000000ULL;
    while (MonotonicNs() < deadline) {
        if (access(path, F_OK) != 0) return errno == ENOENT;
        usleep(1000);
    }
    return false;
}

bool TargetAlive(pid_t pid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/status",
                  static_cast<int>(pid));
    return access(path, F_OK) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX FINAL_OWNER_HEX "
                     "TIMEOUT_MS READY_PATH ACK\n", argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t owner_value = 0;
    std::uint64_t timeout_ms = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 16, &owner_value) ||
        !ParseUnsigned(argv[4], 10, &timeout_ms) ||
        pid_value == 0 || pid_value > INT_MAX || base_value == 0 ||
        timeout_ms < 1000 || timeout_ms > 60000 ||
        argv[5][0] != '/' || std::strcmp(argv[6], kAcknowledgement) != 0) {
        std::fprintf(stderr, "invalid arguments; no target mutation\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const std::uintptr_t base = static_cast<std::uintptr_t>(base_value);
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no target mutation\n");
        return 3;
    }
    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base, static_cast<std::uintptr_t>(owner_value),
                           &final_owner)) {
        std::fprintf(stderr, "final owner resolution failed; no target mutation\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) return 4;

    namespace host = a9tas::natural_action_recording_host_v1;
    namespace protocol = a9tas::natural_action_recording_v1;
    host::Runtime runtime{};
    const std::uint32_t session =
        (static_cast<std::uint32_t>(pid) ^ 0xa9c1f001u) | 1u;
    const char* resolve_failure = nullptr;
    if (!host::Resolve(mem, pid, base, final_owner, 1, session, &runtime,
                       &resolve_failure)) {
        std::fprintf(stderr, "recording hook resolution failed stage=%s\n",
                     resolve_failure ? resolve_failure : "unknown");
        close(mem);
        return 3;
    }

    std::vector<TracedThread> threads;
    std::uint64_t freeze_failures = 0;
    std::uint32_t install_passes = 0;
    bool staged = false;
    bool detached_for_action = false;
    bool success = false;
    host::Result result{};
    if (!FreezeStable(pid, &threads, &freeze_failures, &install_passes) ||
        !host::Stage(mem, &runtime) || !host::ArmFirstFrame(mem, &runtime)) {
        std::fprintf(stderr,
                     "recording hook install failed failures=%" PRIu64 "\n",
                     freeze_failures);
        if (runtime.staged) {
            (void)host::Finish(mem, &runtime, &result);
        }
        (void)DetachAll(&threads);
        close(mem);
        return 7;
    }
    staged = true;
    if (access(argv[5], F_OK) == 0 ||
        !CreateReady(argv[5], pid, install_passes, threads.size())) {
        std::fprintf(stderr, "READY marker creation failed\n");
    } else {
        std::printf(
            "NITRO_RECORDING_SMOKE_ARMED pid=%d owner=0x%" PRIxPTR
            " service=0x%" PRIxPTR " threads=%zu hardware_breakpoints=0\n",
            static_cast<int>(pid), final_owner, runtime.service, threads.size());
        std::fflush(stdout);
        if (WaitForRelease(argv[5], 5000) && DetachAll(&threads)) {
            detached_for_action = true;
            const std::uint64_t deadline =
                MonotonicNs() + timeout_ms * 1000000ULL;
            while (MonotonicNs() < deadline && TargetAlive(pid)) {
                protocol::Evidence evidence{};
                std::uint32_t count = 0;
                if (!host::ReadExact(mem, runtime.payload.evidence, &evidence,
                                     sizeof(evidence)) ||
                    !host::ReadExact(mem, runtime.payload.counts, &count,
                                     sizeof(count))) {
                    break;
                }
                if (evidence.failures != 0 || evidence.overflow_calls != 0 ||
                    evidence.out_of_window_calls != 0) {
                    break;
                }
                if (count == 1 && evidence.wrapper_entries == 1 &&
                    evidence.original_calls == 1 &&
                    evidence.clean_returns == 1) {
                    success = true;
                    break;
                }
                usleep(1000);
            }
        }
    }

    unlink(argv[5]);
    std::uint64_t cleanup_failures = 0;
    std::uint32_t cleanup_passes = 0;
    bool cleanup_frozen = false;
    bool finish_ok = false;
    bool detach_ok = false;
    if (TargetAlive(pid)) {
        if (detached_for_action) threads.clear();
        cleanup_frozen =
            FreezeStable(pid, &threads, &cleanup_failures, &cleanup_passes);
        if (cleanup_frozen && staged)
            finish_ok = host::Finish(mem, &runtime, &result);
        detach_ok = DetachAll(&threads);
    }
    close(mem);
    const bool complete = success && cleanup_frozen && finish_ok && detach_ok &&
                          result.service_restored && result.evidence_exact &&
                          result.count_sum == 1;
    std::printf(
        "NITRO_RECORDING_SMOKE_DONE complete=%u wrapper_entries=%" PRIu64
        " original_calls=%" PRIu64 " clean_returns=%" PRIu64
        " count_sum=%" PRIu64 " restored=%u install_passes=%u "
        "cleanup_passes=%u freeze_failures=%" PRIu64 " target_alive=%u\n",
        complete ? 1u : 0u, result.evidence.wrapper_entries,
        result.evidence.original_calls, result.evidence.clean_returns,
        result.count_sum, result.service_restored ? 1u : 0u, install_passes,
        cleanup_passes, freeze_failures + cleanup_failures,
        TargetAlive(pid) ? 1u : 0u);
    return complete ? 0 : 7;
}
