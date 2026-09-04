#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
pid_t FindThread(pid_t pid, const char* wanted) {
    const std::string root = "/proc/" + std::to_string(pid) + "/task";
    DIR* dir = opendir(root.c_str());
    if (!dir) return -1;
    pid_t found = -1;
    while (dirent* e = readdir(dir)) {
        char* end{};
        const long value = std::strtol(e->d_name, &end, 10);
        if (value <= 0 || end == e->d_name || *end != '\0') continue;
        const std::string path = root + "/" + e->d_name + "/comm";
        FILE* f = std::fopen(path.c_str(), "re");
        char name[128]{};
        if (f && std::fgets(name, sizeof(name), f)) {
            name[std::strcspn(name, "\r\n")] = '\0';
            if (std::strcmp(name, wanted) == 0) found = static_cast<pid_t>(value);
        }
        if (f) std::fclose(f);
        if (found > 0) break;
    }
    closedir(dir);
    return found;
}

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

void Cleanup(pid_t tid, bool stopped) {
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
}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
            "usage: %s PID ACCUMULATOR_HEX HOLD_MS BOUNDARY_ABS_MAX [REVISION_HEX]\n",
            argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto address = static_cast<unsigned long>(std::strtoull(argv[2], nullptr, 16));
    const int hold_ms = std::atoi(argv[3]);
    const float boundary_abs_max = std::strtof(argv[4], nullptr);
    const auto revision_address = argc == 6
        ? static_cast<unsigned long>(std::strtoull(argv[5], nullptr, 16)) : 0UL;
    if (pid <= 0 || !address || (address & 3UL) || hold_ms < 0 ||
        boundary_abs_max <= 0.0f) return 2;

    const pid_t tid = FindThread(pid, "FrameThread 0");
    if (tid <= 0) { std::fprintf(stderr, "FrameThread 0 not found\n"); return 3; }
    if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1 || !Interrupt(tid)) {
        std::fprintf(stderr, "attach/interrupt failed: %s\n", std::strerror(errno));
        return 4;
    }
    bool stopped = true;
    const unsigned long dr7 = 1UL | (1UL << 16) | (3UL << 18);
    if (!PokeDebug(tid, 0, address) || !PokeDebug(tid, 6, 0) ||
        !PokeDebug(tid, 7, dr7)) {
        Cleanup(tid, stopped); return 5;
    }
    if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) == -1) {
        Cleanup(tid, stopped); return 6;
    }
    stopped = false;
    std::printf("armed pid=%d tid=%d accumulator=0x%lx hold_ms=%d threshold=%g\n",
                pid, tid, address, hold_ms, boundary_abs_max);
    std::fflush(stdout);

    int boundaries = 0;
    int watch_hits = 0;
    bool have_previous = false;
    float previous = 0.0f;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    while (boundaries < 2 && std::chrono::steady_clock::now() < deadline) {
        int status{};
        const pid_t got = waitpid(tid, &status, __WALL | WNOHANG);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!WIFSTOPPED(status)) break;
        stopped = true;
        const int sig = WSTOPSIG(status);
        const unsigned long dr6 = PeekDebug(tid, 6);
        if (sig == SIGTRAP && (dr6 & 1UL)) {
            ++watch_hits;
            float accumulator{};
            user_regs_struct regs{};
            const bool read_ok = ReadFloat(tid, address, &accumulator);
            ptrace(PTRACE_GETREGS, tid, nullptr, &regs);
            // The fixed-step loop adds frame time, then subtracts one 1/60 s
            // quantum after completing a physics substep.  Require that exact
            // quantum instead of accepting any decrease, which could otherwise
            // mistake unrelated accumulator movement for a semantic boundary.
            const float decrease = previous - accumulator;
            constexpr float kExpectedStep = 1.0f / 60.0f;
            constexpr float kStepTolerance = 0.002f;
            const bool post_step = read_ok && have_previous &&
                                   std::fabs(decrease - kExpectedStep) <=
                                       kStepTolerance;
            if (post_step && std::isfinite(accumulator) &&
                std::fabs(accumulator) <= boundary_abs_max) {
                ++boundaries;
                unsigned long revision_word = 0;
                int revision_errno = 0;
                if (revision_address) {
                    errno = 0;
                    revision_word = static_cast<unsigned long>(ptrace(
                        PTRACE_PEEKDATA, tid,
                        reinterpret_cast<void*>(revision_address), nullptr));
                    revision_errno = errno;
                }
                std::printf("boundary=%d hit=%d accumulator=%.9g decrease=%.9g rip=0x%llx"
                            " revision=0x%x revision_errno=%d\n",
                            boundaries, watch_hits, accumulator, decrease,
                            static_cast<unsigned long long>(regs.rip),
                            static_cast<unsigned int>(revision_word), revision_errno);
                std::fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            }
            if (read_ok && std::isfinite(accumulator)) {
                previous = accumulator;
                have_previous = true;
            }
            PokeDebug(tid, 6, 0);
            if (boundaries >= 2) break;
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
            stopped = false;
        } else {
            const int deliver = sig == SIGTRAP ? 0 : sig;
            ptrace(PTRACE_CONT, tid, nullptr,
                   reinterpret_cast<void*>(static_cast<intptr_t>(deliver)));
            stopped = false;
        }
    }
    Cleanup(tid, stopped);
    std::printf("summary boundaries=%d watch_hits=%d detached=1\n",
                boundaries, watch_hits);
    return boundaries == 2 ? 0 : 7;
}
