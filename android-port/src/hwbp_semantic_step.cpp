#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

// UNSAFE NEGATIVE EXPERIMENT — DO NOT DEPLOY OR RUN.
// On LDPlayer 9/Houdini this multi-thread ptrace state machine caused the game
// process to terminate with SIGSEGV at 0xdead0000 in libhoudini.so (publisher
// TID 25582).  Kept only as auditable evidence of the rejected approach.

namespace {
bool PokeDebug(pid_t tid, int index, unsigned long value) {
    const auto off = offsetof(user, u_debugreg) +
                     static_cast<std::size_t>(index) * sizeof(unsigned long);
    return ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(off),
                  reinterpret_cast<void*>(value)) != -1;
}
unsigned long PeekDebug(pid_t tid, int index) {
    const auto off = offsetof(user, u_debugreg) +
                     static_cast<std::size_t>(index) * sizeof(unsigned long);
    return static_cast<unsigned long>(ptrace(
        PTRACE_PEEKUSER, tid, reinterpret_cast<void*>(off), nullptr));
}
bool Interrupt(pid_t tid) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) return false;
    int status{};
    return waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
}
bool Continue(pid_t tid, int signal = 0) {
    return ptrace(PTRACE_CONT, tid, nullptr,
                  reinterpret_cast<void*>(static_cast<intptr_t>(signal))) != -1;
}
bool AttachWatch(pid_t tid, unsigned long address) {
    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1 || !Interrupt(tid))
        return false;
    const unsigned long dr7 = 1UL | (1UL << 16) | (3UL << 18);  // write4
    return PokeDebug(tid, 0, address) && PokeDebug(tid, 6, 0) &&
           PokeDebug(tid, 7, dr7);
}
void Cleanup(pid_t tid, bool stopped) {
    if (tid <= 0) return;
    if (!stopped && !Interrupt(tid)) return;
    PokeDebug(tid, 7, 0);
    PokeDebug(tid, 6, 0);
    PokeDebug(tid, 0, 0);
    ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
}
bool ReadWord(pid_t tid, unsigned long address, unsigned long* value) {
    errno = 0;
    *value = static_cast<unsigned long>(ptrace(
        PTRACE_PEEKDATA, tid, reinterpret_cast<void*>(address), nullptr));
    return errno == 0;
}
bool ReadFloat(pid_t tid, unsigned long address, float* value) {
    unsigned long word{};
    if (!ReadWord(tid, address, &word)) return false;
    const std::uint32_t bits = static_cast<std::uint32_t>(word);
    std::memcpy(value, &bits, sizeof(bits));
    return true;
}
bool WriteFloat(pid_t tid, unsigned long address, float value) {
    unsigned long word{};
    if (!ReadWord(tid, address, &word)) return false;
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    word = (word & ~0xffffffffUL) | bits;
    return ptrace(PTRACE_POKEDATA, tid, reinterpret_cast<void*>(address),
                  reinterpret_cast<void*>(word)) != -1;
}

bool WaitForWatch(pid_t wanted, int timeout_ms, int* signal_out) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status{};
        const pid_t got = waitpid(-1, &status, __WALL | WNOHANG);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        if (got == wanted) {
            if (signal_out) *signal_out = sig;
            return sig == SIGTRAP && (PeekDebug(got, 6) & 1UL);
        }
        // Only the two explicitly attached threads can appear here. Keep the
        // non-target participant running while waiting for the requested one.
        PokeDebug(got, 6, 0);
        Continue(got, sig == SIGTRAP ? 0 : sig);
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 8) {
        std::fprintf(stderr,
            "usage: %s FRAME_TID ACCUMULATOR_HEX PUBLISHER_TID CACHE_HEX "
            "REVISION_HEX HOLD_MS TIMEOUT_MS\n", argv[0]);
        return 2;
    }
    const pid_t frame_tid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto accumulator = static_cast<unsigned long>(std::strtoull(argv[2], nullptr, 16));
    const pid_t publisher_tid = static_cast<pid_t>(std::strtol(argv[3], nullptr, 10));
    const auto cache = static_cast<unsigned long>(std::strtoull(argv[4], nullptr, 16));
    const auto revision = static_cast<unsigned long>(std::strtoull(argv[5], nullptr, 16));
    const int hold_ms = std::atoi(argv[6]);
    const int timeout_ms = std::atoi(argv[7]);
    if (frame_tid <= 0 || publisher_tid <= 0 || frame_tid == publisher_tid ||
        !accumulator || !cache || !revision || (accumulator & 3UL) ||
        (cache & 3UL) || hold_ms < 0 || hold_ms > 2000 ||
        timeout_ms < 1000 || timeout_ms > 15000) return 2;

    bool frame_stopped = false;
    bool publisher_stopped = false;
    if (!AttachWatch(frame_tid, accumulator)) {
        Cleanup(frame_tid, true);
        return 3;
    }
    frame_stopped = true;
    if (!AttachWatch(publisher_tid, cache)) {
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, true);
        return 4;
    }
    publisher_stopped = true;
    Continue(publisher_tid);
    publisher_stopped = false;
    Continue(frame_tid);
    frame_stopped = false;

    std::printf("armed frame_tid=%d publisher_tid=%d hold_ms=%d write_scope=accumulator_only\n",
                frame_tid, publisher_tid, hold_ms);
    std::fflush(stdout);

    // Phase 1: find a genuine post-step accumulator subtraction.
    bool have_previous = false;
    float previous{};
    float first_boundary_value{};
    int frame_hits = 0;
    bool found_boundary = false;
    const auto seek_deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < seek_deadline) {
        int status{};
        const pid_t got = waitpid(-1, &status, __WALL | WNOHANG);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        if (got == publisher_tid) {
            publisher_stopped = true;
            PokeDebug(got, 6, 0);
            Continue(got, sig == SIGTRAP ? 0 : sig);
            publisher_stopped = false;
            continue;
        }
        if (got != frame_tid) continue;
        frame_stopped = true;
        if (sig == SIGTRAP && (PeekDebug(got, 6) & 1UL)) {
            ++frame_hits;
            float current{};
            const bool ok = ReadFloat(frame_tid, accumulator, &current);
            const float decrease = previous - current;
            if (ok && have_previous && std::isfinite(current) &&
                std::fabs(decrease - (1.0f / 60.0f)) <= 0.002f &&
                std::fabs(current) <= 0.02f) {
                first_boundary_value = current;
                found_boundary = true;
                break;
            }
            if (ok && std::isfinite(current)) {
                previous = current;
                have_previous = true;
            }
            PokeDebug(got, 6, 0);
            Continue(got);
            frame_stopped = false;
        } else {
            Continue(got, sig == SIGTRAP ? 0 : sig);
            frame_stopped = false;
        }
    }
    if (!found_boundary) {
        std::fprintf(stderr, "initial boundary timeout\n");
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, publisher_stopped);
        return 5;
    }

    // Stop the publisher at the proven world boundary.  Do not wait for a new
    // publication here: runtime validation showed that no publication occurs
    // while the world is fixed.  The existing cache is the coherent state from
    // the preceding completed step.
    if (!publisher_stopped && !Interrupt(publisher_tid)) {
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, publisher_stopped);
        return 6;
    }
    publisher_stopped = true;
    PokeDebug(publisher_tid, 6, 0);
    unsigned long revision_before{}, cache_before{};
    ReadWord(frame_tid, revision, &revision_before);
    ReadWord(frame_tid, cache, &cache_before);

    // Eliminate any already accumulated remainder before the wall-clock hold.
    // This is the only game-memory write performed by this experiment.
    if (!WriteFloat(frame_tid, accumulator, 0.0f)) {
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, publisher_stopped);
        return 7;
    }
    std::printf("freeze boundary_value=%.9g revision=0x%x cache_low=0x%x\n",
                first_boundary_value, static_cast<unsigned int>(revision_before),
                static_cast<unsigned int>(cache_before));
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));

    // Phase 2: keep publisher frozen, resume world, intercept the next dt add,
    // and replace only the accumulator with exactly one 1/60-second quantum.
    PokeDebug(frame_tid, 6, 0);
    Continue(frame_tid);
    frame_stopped = false;
    int ignored_signal{};
    if (!WaitForWatch(frame_tid, timeout_ms, &ignored_signal)) {
        std::fprintf(stderr, "incoming dt timeout\n");
        Cleanup(frame_tid, true);
        Cleanup(publisher_tid, publisher_stopped);
        return 8;
    }
    frame_stopped = true;
    float incoming_accumulator{};
    ReadFloat(frame_tid, accumulator, &incoming_accumulator);
    constexpr float kOneStep = 1.0f / 60.0f;
    if (!WriteFloat(frame_tid, accumulator, kOneStep)) {
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, publisher_stopped);
        return 9;
    }
    PokeDebug(frame_tid, 6, 0);
    Continue(frame_tid);
    frame_stopped = false;

    // The next accumulator write must be the completed-step subtraction.
    if (!WaitForWatch(frame_tid, timeout_ms, &ignored_signal)) {
        std::fprintf(stderr, "single-step completion timeout\n");
        Cleanup(frame_tid, true);
        Cleanup(publisher_tid, publisher_stopped);
        return 10;
    }
    frame_stopped = true;
    float completed_accumulator{};
    ReadFloat(frame_tid, accumulator, &completed_accumulator);
    unsigned long revision_after{};
    ReadWord(frame_tid, revision, &revision_after);
    const bool exact_step = std::isfinite(completed_accumulator) &&
                            std::fabs(completed_accumulator) <= 0.00001f;

    // Phase 3: with world fixed at the second boundary, allow exactly one cache
    // publication, then refreeze it and detach both threads safely.
    PokeDebug(publisher_tid, 6, 0);
    Continue(publisher_tid);
    publisher_stopped = false;
    if (!WaitForWatch(publisher_tid, timeout_ms, &ignored_signal)) {
        std::fprintf(stderr, "final publication timeout\n");
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, true);
        return 11;
    }
    publisher_stopped = true;
    unsigned long cache_after{};
    ReadWord(frame_tid, cache, &cache_after);

    Cleanup(frame_tid, frame_stopped);
    Cleanup(publisher_tid, publisher_stopped);
    std::printf("single_step incoming_accumulator=%.9g clamped_dt=%.9g "
                "completed_accumulator=%.9g exact=%d revision_delta=%u "
                "publications=1 cache_changed=%d detached=1\n",
                incoming_accumulator, kOneStep, completed_accumulator,
                exact_step ? 1 : 0,
                static_cast<unsigned int>(revision_after - revision_before),
                cache_before != cache_after ? 1 : 0);
    return exact_step ? 0 : 12;
}
