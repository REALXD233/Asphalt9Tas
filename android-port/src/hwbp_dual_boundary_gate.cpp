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

bool ReadFloat(pid_t tid, unsigned long address, float* value) {
    errno = 0;
    const unsigned long word = static_cast<unsigned long>(ptrace(
        PTRACE_PEEKDATA, tid, reinterpret_cast<void*>(address), nullptr));
    if (errno != 0) return false;
    const std::uint32_t bits = static_cast<std::uint32_t>(word);
    std::memcpy(value, &bits, sizeof(bits));
    return true;
}

bool ReadWord(pid_t tid, unsigned long address, unsigned long* value) {
    errno = 0;
    *value = static_cast<unsigned long>(ptrace(
        PTRACE_PEEKDATA, tid, reinterpret_cast<void*>(address), nullptr));
    return errno == 0;
}

bool Continue(pid_t tid, int signal = 0) {
    return ptrace(PTRACE_CONT, tid, nullptr,
                  reinterpret_cast<void*>(static_cast<intptr_t>(signal))) != -1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 9) {
        std::fprintf(stderr,
            "usage: %s FRAME_TID ACCUMULATOR_HEX PUBLISHER_TID CACHE_HEX "
            "REVISION_HEX HOLD_MS BOUNDARY_ABS_MAX TIMEOUT_MS\n", argv[0]);
        return 2;
    }
    const pid_t frame_tid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto accumulator = static_cast<unsigned long>(std::strtoull(argv[2], nullptr, 16));
    const pid_t publisher_tid = static_cast<pid_t>(std::strtol(argv[3], nullptr, 10));
    const auto cache = static_cast<unsigned long>(std::strtoull(argv[4], nullptr, 16));
    const auto revision = static_cast<unsigned long>(std::strtoull(argv[5], nullptr, 16));
    const int hold_ms = std::atoi(argv[6]);
    const float boundary_abs_max = std::strtof(argv[7], nullptr);
    const int timeout_ms = std::atoi(argv[8]);
    if (frame_tid <= 0 || publisher_tid <= 0 || frame_tid == publisher_tid ||
        !accumulator || !cache || !revision || (accumulator & 3UL) ||
        (cache & 3UL) || hold_ms < 0 || boundary_abs_max <= 0.0f ||
        timeout_ms < 1000 || timeout_ms > 15000) return 2;

    bool frame_stopped = false;
    bool publisher_stopped = false;
    if (!AttachWatch(frame_tid, accumulator)) {
        std::fprintf(stderr, "frame attach failed: %s\n", std::strerror(errno));
        Cleanup(frame_tid, true);
        return 3;
    }
    frame_stopped = true;
    if (!AttachWatch(publisher_tid, cache)) {
        std::fprintf(stderr, "publisher attach failed: %s\n", std::strerror(errno));
        Cleanup(frame_tid, frame_stopped);
        Cleanup(publisher_tid, true);
        return 4;
    }
    publisher_stopped = true;
    Continue(publisher_tid);
    publisher_stopped = false;
    Continue(frame_tid);
    frame_stopped = false;

    std::printf("armed frame_tid=%d accumulator=0x%lx publisher_tid=%d "
                "cache=0x%lx revision=0x%lx hold_ms=%d\n",
                frame_tid, accumulator, publisher_tid, cache, revision, hold_ms);
    std::fflush(stdout);

    int boundaries = 0;
    int frame_hits = 0;
    int publications_between = 0;
    int publications_before = 0;
    bool have_previous = false;
    float previous{};
    unsigned long first_revision{};
    unsigned long second_revision{};
    unsigned long first_cache{};
    unsigned long second_cache{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (boundaries < 2 && std::chrono::steady_clock::now() < deadline) {
        int status{};
        const pid_t got = waitpid(-1, &status, __WALL | WNOHANG);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        const unsigned long dr6 = PeekDebug(got, 6);

        if (got == publisher_tid) {
            publisher_stopped = true;
            if (sig == SIGTRAP && (dr6 & 1UL)) {
                if (boundaries == 0) ++publications_before;
                else if (boundaries == 1) ++publications_between;
                PokeDebug(publisher_tid, 6, 0);
                Continue(publisher_tid);
                publisher_stopped = false;
            } else {
                Continue(publisher_tid, sig == SIGTRAP ? 0 : sig);
                publisher_stopped = false;
            }
            continue;
        }

        if (got != frame_tid) {
            Continue(got, sig == SIGTRAP ? 0 : sig);
            continue;
        }
        frame_stopped = true;
        if (sig != SIGTRAP || !(dr6 & 1UL)) {
            Continue(frame_tid, sig == SIGTRAP ? 0 : sig);
            frame_stopped = false;
            continue;
        }

        ++frame_hits;
        float current{};
        user_regs_struct regs{};
        const bool read_ok = ReadFloat(frame_tid, accumulator, &current);
        ptrace(PTRACE_GETREGS, frame_tid, nullptr, &regs);
        const float decrease = previous - current;
        constexpr float kExpectedStep = 1.0f / 60.0f;
        constexpr float kTolerance = 0.002f;
        const bool post_step = read_ok && have_previous &&
            std::fabs(decrease - kExpectedStep) <= kTolerance &&
            std::isfinite(current) && std::fabs(current) <= boundary_abs_max;

        if (post_step) {
            ++boundaries;
            // Freeze the cache publisher at the same proven world boundary.
            if (!publisher_stopped && !Interrupt(publisher_tid)) {
                std::fprintf(stderr, "publisher interrupt failed at boundary %d\n",
                             boundaries);
                break;
            }
            publisher_stopped = true;
            // If the publisher reached its watchpoint concurrently with the
            // frame boundary, PTRACE_INTERRUPT may reap that SIGTRAP. Account
            // for it here instead of silently losing one publication.
            const unsigned long publisher_dr6 = PeekDebug(publisher_tid, 6);
            if (publisher_dr6 & 1UL) {
                if (boundaries == 1) ++publications_before;
                else ++publications_between;
                PokeDebug(publisher_tid, 6, 0);
            }
            unsigned long rev_word{}, cache_word{};
            ReadWord(frame_tid, revision, &rev_word);
            ReadWord(frame_tid, cache, &cache_word);
            if (boundaries == 1) {
                first_revision = rev_word;
                first_cache = cache_word;
            } else {
                second_revision = rev_word;
                second_cache = cache_word;
            }
            std::printf("boundary=%d frame_hit=%d accumulator=%.9g "
                        "decrease=%.9g rip=0x%llx revision=0x%x cache_low=0x%x "
                        "publications_between=%d\n",
                        boundaries, frame_hits, current, decrease,
                        static_cast<unsigned long long>(regs.rip),
                        static_cast<unsigned int>(rev_word),
                        static_cast<unsigned int>(cache_word), publications_between);
            std::fflush(stdout);
            if (boundaries == 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
                Continue(publisher_tid);
                publisher_stopped = false;
            }
        }

        if (read_ok && std::isfinite(current)) {
            previous = current;
            have_previous = true;
        }
        PokeDebug(frame_tid, 6, 0);
        if (boundaries >= 2) break;
        Continue(frame_tid);
        frame_stopped = false;
    }

    Cleanup(frame_tid, frame_stopped);
    Cleanup(publisher_tid, publisher_stopped);
    std::printf("summary boundaries=%d frame_hits=%d publications_before=%d "
                "publications_between=%d revision_delta=%u cache_changed=%d detached=1\n",
                boundaries, frame_hits, publications_before, publications_between,
                static_cast<unsigned int>(second_revision - first_revision),
                first_cache != second_cache ? 1 : 0);
    return boundaries == 2 ? 0 : 7;
}
