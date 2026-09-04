#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class BreakMode {
    kExecute,
    kWrite4,
    kWrite8,
};

const char* ModeName(BreakMode mode) {
    switch (mode) {
        case BreakMode::kExecute: return "execute";
        case BreakMode::kWrite4: return "write4";
        case BreakMode::kWrite8: return "write8";
    }
    return "unknown";
}

bool ParseMode(const char* value, BreakMode* mode) {
    if (std::strcmp(value, "x") == 0) {
        *mode = BreakMode::kExecute;
        return true;
    }
    if (std::strcmp(value, "w4") == 0) {
        *mode = BreakMode::kWrite4;
        return true;
    }
    if (std::strcmp(value, "w8") == 0) {
        *mode = BreakMode::kWrite8;
        return true;
    }
    return false;
}

unsigned long Dr7ForMode(BreakMode mode) {
    // Local DR0 enable. RW0=01 means write; LEN0=11/10 means 4/8 bytes.
    if (mode == BreakMode::kWrite4)
        return 1UL | (1UL << 16) | (3UL << 18);
    if (mode == BreakMode::kWrite8)
        return 1UL | (1UL << 16) | (2UL << 18);
    return 1UL;
}

std::vector<pid_t> ListThreads(pid_t pid) {
    std::vector<pid_t> result;
    const std::string path = "/proc/" + std::to_string(pid) + "/task";
    DIR* dir = opendir(path.c_str());
    if (!dir) return result;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (value > 0 && end != entry->d_name && *end == '\0')
            result.push_back(static_cast<pid_t>(value));
    }
    closedir(dir);
    return result;
}

bool PokeDebug(pid_t tid, int index, unsigned long value) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    return ptrace(PTRACE_POKEUSER, tid, reinterpret_cast<void*>(offset),
                  reinterpret_cast<void*>(value)) != -1;
}

unsigned long PeekDebug(pid_t tid, int index) {
    const auto offset = offsetof(user, u_debugreg) +
                        static_cast<std::size_t>(index) * sizeof(unsigned long);
    errno = 0;
    return static_cast<unsigned long>(ptrace(
        PTRACE_PEEKUSER, tid, reinterpret_cast<void*>(offset), nullptr));
}

bool StopThread(pid_t tid) {
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) return false;
    int status = 0;
    return waitpid(tid, &status, __WALL) == tid && WIFSTOPPED(status);
}

bool ClearAndDetach(pid_t tid, bool stopped) {
    if (!stopped && !StopThread(tid)) return false;
    const bool writes_ok = PokeDebug(tid, 7, 0) &&
                           PokeDebug(tid, 6, 0) &&
                           PokeDebug(tid, 0, 0);
    const unsigned long observed_dr7 = PeekDebug(tid, 7);
    const unsigned long observed_dr6 = PeekDebug(tid, 6);
    const unsigned long observed_dr0 = PeekDebug(tid, 0);
    // Architectural DR6 reserved bits read back as ones on x86.  Only B0..B3
    // are status bits we asked to clear; requiring the whole register to read
    // as zero incorrectly rejects a successful clear.
    const bool cleared = writes_ok && observed_dr7 == 0 &&
                         (observed_dr6 & 0xFUL) == 0 && observed_dr0 == 0;
    if (!cleared) {
        std::fprintf(stderr,
                     "refusing detach: debug register clear/readback failed "
                     "tid=%d dr0=0x%lx dr6=0x%lx dr7=0x%lx errno=%d\n",
                     tid, observed_dr0, observed_dr6, observed_dr7, errno);
        return false;
    }
    return ptrace(PTRACE_DETACH, tid, nullptr, nullptr) != -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: %s PID ADDRESS_HEX DURATION_MS x|w4|w8 [THREAD_NAME]\n",
                     argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const auto address = static_cast<unsigned long>(std::strtoull(argv[2], nullptr, 16));
    const int duration_ms = std::atoi(argv[3]);
    BreakMode mode{};
    if (pid <= 0 || address == 0 || duration_ms < 100 ||
        !ParseMode(argv[4], &mode))
        return 2;
    if ((mode == BreakMode::kWrite4 && (address & 3UL) != 0) ||
        (mode == BreakMode::kWrite8 && (address & 7UL) != 0)) {
        std::fprintf(stderr, "watch address is not naturally aligned\n");
        return 2;
    }

    const char* thread_name = argc == 6 ? argv[5] : nullptr;
    std::vector<pid_t> tids = ListThreads(pid);
    std::vector<pid_t> traced;
    for (pid_t tid : tids) {
        if (thread_name != nullptr) {
            char comm_path[96]{};
            std::snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm", pid, tid);
            FILE* comm = std::fopen(comm_path, "re");
            char value[128]{};
            if (!comm || !std::fgets(value, sizeof(value), comm)) {
                if (comm) std::fclose(comm);
                continue;
            }
            std::fclose(comm);
            value[std::strcspn(value, "\r\n")] = '\0';
            if (std::strcmp(value, thread_name) != 0) continue;
        }
        // Existing threads are enough for a short observation window. Avoid
        // TRACECLONE because forwarding its synthetic SIGTRAP can kill the app.
        if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1)
            continue;
        if (!StopThread(tid)) {
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            continue;
        }
        if (!PokeDebug(tid, 0, address) || !PokeDebug(tid, 6, 0) ||
            !PokeDebug(tid, 7, Dr7ForMode(mode))) {
            ClearAndDetach(tid, true);
            continue;
        }
        traced.push_back(tid);
    }
    for (pid_t tid : traced) ptrace(PTRACE_CONT, tid, nullptr, nullptr);

    std::printf("armed pid=%d address=0x%lx mode=%s threads=%zu duration_ms=%d\n",
                pid, address, ModeName(mode), traced.size(), duration_ms);
    std::fflush(stdout);
    std::uint64_t hits = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        const unsigned long dr6 = PeekDebug(tid, 6);
        if (sig == SIGTRAP && (dr6 & 1) != 0) {
            user_regs_struct regs{};
            ptrace(PTRACE_GETREGS, tid, nullptr, &regs);
            ++hits;
            if (hits <= 32) {
                errno = 0;
                const unsigned long value = static_cast<unsigned long>(ptrace(
                    PTRACE_PEEKDATA, tid, reinterpret_cast<void*>(address), nullptr));
                std::printf("hit=%" PRIu64
                            " tid=%d rip=0x%llx dr6=0x%lx value=0x%lx peek_errno=%d\n",
                            hits, tid,
                            static_cast<unsigned long long>(regs.rip), dr6, value, errno);
                // Houdini keeps active ARM64 guest return PCs in the host
                // stack.  Preserve a bounded read-only window so a caller can
                // classify the guest call chain without patching game code.
                std::printf("stack hit=%" PRIu64, hits);
                for (std::size_t i = 0; i < 48; ++i) {
                    errno = 0;
                    const unsigned long word = static_cast<unsigned long>(ptrace(
                        PTRACE_PEEKDATA, tid,
                        reinterpret_cast<void*>(
                            static_cast<std::uintptr_t>(regs.rsp) +
                            i * sizeof(unsigned long)),
                        nullptr));
                    if (errno != 0) {
                        std::printf(" read_error_at=%zu errno=%d", i, errno);
                        break;
                    }
                    std::printf(" s%02zu=0x%lx", i, word);
                }
                std::printf("\n");
                std::fflush(stdout);
            }
            PokeDebug(tid, 6, 0);
            ptrace(PTRACE_CONT, tid, nullptr, nullptr);
        } else {
            // A ptrace-generated SIGTRAP is debugger bookkeeping, not an app
            // signal. Swallow it; preserve genuine non-TRAP signals.
            const int deliver = sig == SIGTRAP ? 0 : sig;
            ptrace(PTRACE_CONT, tid, nullptr,
                   reinterpret_cast<void*>(static_cast<intptr_t>(deliver)));
        }
    }

    for (pid_t tid : traced) ClearAndDetach(tid, false);
    std::printf("summary hits=%" PRIu64 " traced_threads=%zu\n", hits, traced.size());
    return 0;
}
