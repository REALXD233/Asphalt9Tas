#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS
#define A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS 5000
#endif

namespace {

// ---- H0: LDPlayer9 RIP-resume compensation ----
// Evidence 20260816: LDPlayer9 resumes a tracee at (written RIP - 2) after
// PTRACE_SETREGSET/POKEUSER, regardless of the stop type. This was proven by
// the fn-2 byte patterns in h0_remote_call_spinthread_20260816_093620.txt and
// h0_remote_call_singlestep_20260816_094023.txt. RC_RIP_BIAS=2 compensates it.

static long g_rc_rip_bias = []() -> long {
    const char* s = std::getenv("RC_RIP_BIAS");
    if (s == nullptr || *s == '\0') return 0;
    return std::strtol(s, nullptr, 0);
}();

static long RcRipBias() { return g_rc_rip_bias; }

static void RcSetRipBias(long bias) {
    g_rc_rip_bias = bias;
    std::fprintf(stderr, "rc rip bias set to %ld\n", bias);
}

static constexpr auto RcStageTimeout() {
    static_assert(A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS >= 50);
    return std::chrono::milliseconds(A9TAS_REMOTE_CALL_STAGE_TIMEOUT_MS);
}

static bool RcUseSingleStep() {
    const char* s = std::getenv("RC_USE_SINGLESTEP");
    if (s == nullptr) return true;
    return std::strcmp(s, "0") != 0;
}

// ---- H0: RemoteCall outcome classification ----

// Stop reason classification for waitpid / PTRACE_EVENT stops
enum class StopReason : std::uint8_t {
    kUnknown = 0,
    kSIGTRAP,        // expected hardware breakpoint hit
    kSIGSEGV,
    kSIGBUS,
    kSIGILL,
    kSIGSYS,
    kSIGSTOP,
    kThreadExit,     // thread exited while traced
    kProcessExit,    // whole process exited
    kPtraceEvent,    // PTRACE_EVENT_* stop
    kGroupStop,      // stop caused by stop of process group
    kTimeout,        // our own deadline expired
    kWaitError,      // waitpid returned -1
    kSpuriousWakeup, // waitpid returned 0
};

// RemoteCall final result classification
enum class CallResult : std::uint8_t {
    kSuccess = 0,
    kTimeout,
    kTargetSegv,
    kTargetBus,
    kTargetIll,
    kTargetSys,
    kWrongTrap,
    kThreadExited,
    kProcessExited,
    kPtraceError,
    kRollbackFailed,
    kInternalError,
};

static const char* RemoteCallResultName(CallResult r) {
    switch (r) {
        case CallResult::kSuccess: return "success";
        case CallResult::kTimeout: return "timeout";
        case CallResult::kTargetSegv: return "target_segv";
        case CallResult::kTargetBus: return "target_bus";
        case CallResult::kTargetIll: return "target_ill";
        case CallResult::kTargetSys: return "target_sys";
        case CallResult::kWrongTrap: return "wrong_trap";
        case CallResult::kThreadExited: return "thread_exited";
        case CallResult::kProcessExited: return "process_exited";
        case CallResult::kPtraceError: return "ptrace_error";
        case CallResult::kRollbackFailed: return "rollback_failed";
        case CallResult::kInternalError: return "internal_error";
        default: return "unknown";
    }
}

// Detailed report emitted by RemoteCall for diagnostics
struct RemoteCallReport {
    CallResult result{};
    StopReason stop_reason{};
    int waitpid_result{};       // -1, 0, or tid
    int waitpid_status{};       // raw status from waitpid
    pid_t waited_pid{};          // which pid was returned by waitpid
    bool tracee_stopped{};      // WIFSTOPPED(status)
    int stop_signal{};           // WSTOPSIG(status) if stopped
    bool process_exited{};      // WIFEXITED(status)
    int exit_code{};             // WEXITSTATUS(status) if exited
    bool signaled{};             // WIFSIGNALED(status)
    int signal_number{};        // WTERMSIG(status) if signaled
    bool core_dumped{};          // WCOREDUMP(status)

    // Registers at the moment of stop
    std::uint64_t rax{};
    std::uint64_t rip{};
    std::uint64_t rsp{};
    std::uint64_t rdi{};
    std::uint64_t rsi{};
    std::uint64_t rdx{};
    std::uint64_t rcx{};
    std::uint64_t r8{};
    std::uint64_t r9{};

    // Debug registers at the moment of stop
    std::uint64_t dr0{};
    std::uint64_t dr6{};
    std::uint64_t dr7{};

    // Stack slot that we used for the return address
    std::uint64_t saved_stack_word{};

    // Timing
    std::uint64_t elapsed_ns{};  // wall-clock time from CONT to stop

    // Pre-detach liveness (measured before controller detaches)
    bool pre_detach_target_alive{};        // TGID alive before detach
    bool pre_detach_target_thread_alive{};  // TID alive before detach

    // Validity flags for diagnostic data
    bool registers_valid{};      // PTRACE_GETREGS succeeded
    bool debug_registers_valid{};  // DR0/DR6/DR7 reads succeeded
    bool stack_word_valid{};     // stack slot read succeeded
    bool stop_confirmed{};       // waitpid confirmed WIFSTOPPED
    bool rollback_attempted{};   // restore path was entered
    bool rollback_succeeded{};    // all restore operations succeeded
    bool detach_safe{};          // tracee is stopped and restored, safe to detach

    // Saved TGID for liveness distinction
    pid_t saved_tgid{};

    // Human-readable note for early failures
    std::string note;
};

// Installation state: separate "installing" vs "installed"
enum class InstallState : std::uint8_t {
    kNotStarted = 0,
    kInstalling,   // RemoteCall in flight
    kInstalled,    // RemoteCall succeeded
    kFailed,       // RemoteCall failed
};

// ---- End of H0 classification ----


[[maybe_unused]]
pid_t FindPidByCmdline(const char* expected) {
    DIR* proc = opendir("/proc");
    if (proc == nullptr) return 0;
    pid_t result = 0;
    while (dirent* entry = readdir(proc)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (value <= 0 || end == entry->d_name || *end != '\0') continue;
        std::ifstream cmdline(std::string("/proc/") + entry->d_name + "/cmdline",
                              std::ios::binary);
        std::string command;
        std::getline(cmdline, command, '\0');
        if (command == expected) {
            result = static_cast<pid_t>(value);
            break;
        }
    }
    closedir(proc);
    return result;
}

[[maybe_unused]]
bool ProcessHasMapping(pid_t pid, const char* needle) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(input, line)) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string ThreadName(pid_t pid, pid_t tid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/task/" +
                        std::to_string(tid) + "/comm");
    std::string name;
    std::getline(input, name);
    if (!name.empty() && name.back() == '\r') name.pop_back();
    return name;
}

struct Mapping {
    std::uintptr_t start{};
    std::uintptr_t end{};
    std::uintptr_t offset{};
    bool readable{};
    bool writable{};
    bool executable{};
    bool private_mapping{};
    std::string path;
};

bool FindMapping(pid_t pid, std::uintptr_t address, Mapping* out) {
    const std::string maps = pid == getpid()
        ? "/proc/self/maps"
        : "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream input(maps);
    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, path);
        if (fields >= 4 && address >= start && address < end) {
            out->start = static_cast<std::uintptr_t>(start);
            out->end = static_cast<std::uintptr_t>(end);
            out->offset = static_cast<std::uintptr_t>(offset);
            out->readable = perms[0] == 'r';
            out->writable = perms[1] == 'w';
            out->executable = perms[2] == 'x';
            out->private_mapping = perms[3] == 'p';
            if (fields == 5) {
                out->path = path;
                while (!out->path.empty() && out->path.front() == ' ') out->path.erase(0, 1);
            }
            return true;
        }
    }
    return false;
}

bool FindModuleMapping(pid_t pid, const std::string& path, std::uintptr_t file_offset,
                       Mapping* out) {
    const std::string maps = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream input(maps);
    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char candidate[1024]{};
        const int fields = std::sscanf(line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, candidate);
        std::string candidate_path = fields == 5 ? candidate : "";
        while (!candidate_path.empty() && candidate_path.front() == ' ') candidate_path.erase(0, 1);
        if (fields == 5 && candidate_path == path && offset == file_offset) {
            out->start = static_cast<std::uintptr_t>(start);
            out->end = static_cast<std::uintptr_t>(end);
            out->offset = static_cast<std::uintptr_t>(offset);
            out->readable = perms[0] == 'r';
            out->writable = perms[1] == 'w';
            out->executable = perms[2] == 'x';
            out->private_mapping = perms[3] == 'p';
            out->path = candidate_path;
            return true;
        }
    }
    return false;
}

// Root-side read from /proc/<pid>/mem. Used for the 0xCC stub scan before the
// tracee is attached (PTRACE_PEEKDATA requires an attached, stopped tracee).
static bool ReadProcessMemoryUnchecked(pid_t pid, std::uintptr_t address,
                                       void* buffer, std::size_t size) {
    if (size == 0) return true;
    const std::string mem_path = "/proc/" + std::to_string(pid) + "/mem";
    const int fd = open(mem_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;
    bool ok = true;
    std::size_t done = 0;
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    while (done < size) {
        const ssize_t n = pread(fd, bytes + done, size - done,
                                static_cast<off_t>(address + done));
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    close(fd);
    return ok;
}

// Find an existing 0xCC byte in an x86_64-executable mapping of the target
// and use it as a no-modification int3 return stub. Preferred order:
// libc.so, libdl.so, then every other file-backed r-x mapping that is not an
// ARM64 guest library (those bytes are ARM64 text, not host x86 code).
std::uintptr_t FindInt3Stub(pid_t pid) {
    struct Candidate {
        std::uintptr_t start{};
        std::uintptr_t end{};
        std::string path;
    };
    std::vector<Candidate> candidates;
    {
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        std::string line;
        while (std::getline(maps, line)) {
            unsigned long long start = 0, end = 0, offset = 0;
            char perms[5]{};
            char path[1024]{};
            const int fields = std::sscanf(line.c_str(),
                                           "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                           &start, &end, perms, &offset, path);
            if (fields < 4 || perms[0] != 'r' || perms[2] != 'x') continue;
            std::string candidate_path = fields == 5 ? path : "";
            while (!candidate_path.empty() && candidate_path.front() == ' ') {
                candidate_path.erase(0, 1);
            }
            if (candidate_path.empty()) continue;
            if (candidate_path.find("arm64") != std::string::npos) continue;
            if (candidate_path.find("Asphalt") != std::string::npos) continue;
            candidates.push_back({static_cast<std::uintptr_t>(start),
                                  static_cast<std::uintptr_t>(end),
                                  std::move(candidate_path)});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         auto rank = [](const std::string& p) {
                             if (p.find("libc.so") != std::string::npos) return 0;
                             if (p.find("libdl.so") != std::string::npos) return 1;
                             return 2;
                         };
                         return rank(a.path) < rank(b.path);
                     });

    std::vector<std::uint8_t> page;
    std::uint64_t total_scanned = 0;
    for (const Candidate& c : candidates) {
        if (total_scanned > (64ULL << 20)) break;
        for (std::uintptr_t cursor = c.start; cursor < c.end;) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uintptr_t>(0x1000, c.end - cursor));
            page.resize(chunk);
            if (!ReadProcessMemoryUnchecked(pid, cursor, page.data(), chunk)) {
                break;
            }
            total_scanned += chunk;
            for (std::size_t i = 0; i < chunk; ++i) {
                if (page[i] == 0xCC) {
                    std::fprintf(stderr,
                                 "int3 stub candidate=0x%llx mapping=%s\n",
                                 static_cast<unsigned long long>(cursor + i),
                                 c.path.c_str());
                    return cursor + i;
                }
            }
            cursor += chunk;
        }
    }
    std::fprintf(stderr, "int3 stub scan failed: no 0xCC in candidate mappings\n");
    return 0;
}

std::uintptr_t RemoteSymbol(pid_t pid, void* local_symbol) {
    Mapping local{};
    if (!FindMapping(getpid(), reinterpret_cast<std::uintptr_t>(local_symbol), &local) ||
        local.path.empty()) {
        return 0;
    }
    Mapping remote{};
    if (!FindModuleMapping(pid, local.path, local.offset, &remote)) {
        return 0;
    }
    return remote.start + (reinterpret_cast<std::uintptr_t>(local_symbol) - local.start);
}

bool WriteRemote(pid_t pid, std::uintptr_t address, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t done = 0; done < size; done += sizeof(long)) {
        const size_t chunk = size - done < sizeof(long) ? size - done : sizeof(long);
        long word = 0;
        if (chunk != sizeof(long)) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid, address + done, nullptr);
            if (word == -1 && errno != 0) return false;
        }
        std::memcpy(&word, bytes + done, chunk);
        if (ptrace(PTRACE_POKEDATA, pid, address + done, word) == -1) return false;
    }
    return true;
}

bool SelectVerifiedCallStack(pid_t tid, std::uintptr_t saved_rsp,
                             std::uintptr_t* call_stack) {
    Mapping stack{};
    if (!FindMapping(tid, saved_rsp - 1, &stack) || !stack.readable ||
        !stack.writable || !stack.private_mapping) {
        return false;
    }
    constexpr std::uintptr_t kReserve = 0x8000;
    constexpr std::uintptr_t kGuardMargin = 0x1000;
    if (saved_rsp < stack.start + kReserve + kGuardMargin) return false;
    const std::uintptr_t candidate = ((saved_rsp - kReserve) & ~std::uintptr_t{0xf}) - 8;
    if (candidate < stack.start + kGuardMargin ||
        candidate + sizeof(std::uintptr_t) > stack.end) {
        return false;
    }
    Mapping candidate_mapping{};
    if (!FindMapping(tid, candidate, &candidate_mapping) ||
        candidate_mapping.start != stack.start || candidate_mapping.end != stack.end ||
        !candidate_mapping.writable || !candidate_mapping.private_mapping) {
        return false;
    }
    *call_stack = candidate;
    return true;
}

bool PeekRemoteWord(pid_t tid, std::uintptr_t address, long* output) {
    errno = 0;
    const long value = ptrace(PTRACE_PEEKDATA, tid, address, nullptr);
    if (value == -1 && errno != 0) return false;
    *output = value;
    return true;
}

// LDPlayer9's kernel showed non-deterministic PTRACE_SETREGS (H0 20260816:
// RIP sometimes stayed unchanged, turning a remote call into a plain return
// through the int3 stub). Try PTRACE_SETREGSET/NT_PRSTATUS first, verify by
// readback, and only then fall back to word-by-word PTRACE_POKEUSER.
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

bool GetUserRegs(pid_t tid, user_regs_struct* out) {
    if (out == nullptr) return false;
    iovec iov{out, sizeof(user_regs_struct)};
    return ptrace(PTRACE_GETREGSET, tid,
                  reinterpret_cast<void*>(NT_PRSTATUS), &iov) != -1;
}

bool SetUserRegs(pid_t tid, const user_regs_struct& regs) {
    user_regs_struct copy = regs;
    iovec iov{&copy, sizeof(copy)};
    return ptrace(PTRACE_SETREGSET, tid,
                  reinterpret_cast<void*>(NT_PRSTATUS), &iov) != -1;
}

bool PokeUserRegs(pid_t tid, const user_regs_struct& regs) {
    user_regs_struct readback{};
    if (SetUserRegs(tid, regs) && GetUserRegs(tid, &readback) &&
        readback.rip == regs.rip && readback.rsp == regs.rsp) {
        return true;
    }

    const auto* words = reinterpret_cast<const unsigned long*>(&regs);
    constexpr std::size_t kWords =
        sizeof(user_regs_struct) / sizeof(unsigned long);
    constexpr std::uintptr_t kRipOffset = offsetof(user, regs.rip);
    constexpr std::uintptr_t kRspOffset = offsetof(user, regs.rsp);
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool wrote = true;
        for (std::size_t i = 0; i < kWords; ++i) {
            const auto data = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(words[i]));
            if (ptrace(PTRACE_POKEUSER, tid, i * sizeof(unsigned long), data) ==
                -1) {
                wrote = false;
                break;
            }
        }
        if (!wrote) return false;
        errno = 0;
        const long rip = ptrace(PTRACE_PEEKUSER, tid, kRipOffset, nullptr);
        const bool rip_ok = !(rip == -1 && errno != 0) &&
                            static_cast<unsigned long>(rip) == regs.rip;
        errno = 0;
        const long rsp = ptrace(PTRACE_PEEKUSER, tid, kRspOffset, nullptr);
        const bool rsp_ok = !(rsp == -1 && errno != 0) &&
                            static_cast<unsigned long>(rsp) == regs.rsp;
        if (rip_ok && rsp_ok) return true;
        std::fprintf(stderr,
                     "poke_regs attempt=%d mismatch rip=0x%lx want=0x%lx "
                     "rsp=0x%lx want=0x%lx\n",
                     attempt, rip, static_cast<unsigned long>(regs.rip), rsp,
                     static_cast<unsigned long>(regs.rsp));
    }
    return false;
}

// ---- H0: Monotonic deadline helpers ----

// Structured result from WaitWithDeadline that fully expresses the state
// of the wait operation. This replaces the old pid_t return value that
// conflated timeout-recovered stops with normal stops.
struct WaitResult {
    // The raw pid returned by waitpid (>0 = a real stop, 0 = spurious, -1 = error)
    pid_t waited_pid{};
    // The raw status from waitpid
    int status{};
    // Whether we observed a real WIFSTOPPED stop
    bool stop_confirmed{};
    // Whether this stop was caused by our own SIGSTOP after deadline expiry
    bool timeout_recovered{};
    // Whether the tracee exited (WIFEXITED or WIFSIGNALED)
    bool tracee_exited{};
    // Whether the wait itself errored
    bool wait_error{};
    // Error code if wait_error is true (errno or ETIMEDOUT/ECHILD)
    int error_code{};
    // Whether it is safe to restore registers / DRs / stack
    // (only true if stop_confirmed && !tracee_exited)
    bool safe_to_restore{};
    // Whether the final classification should be kTimeout
    bool is_timeout{};
};

// Wait for a specific tid with a bounded deadline.
// Returns a WaitResult that fully expresses:
// - whether we observed a real stop
// - whether the stop was caused by our timeout recovery
// - the raw waitpid status
// - whether it is safe to restore registers, DRs and stack
// - whether the final classification should be kTimeout
//
// H0-BLOCKER-4 fix: once WIFEXITED or WIFSIGNALED is received, return
// immediately with the original status preserved. Do not call waitpid
// again after consuming a termination status (next call returns ECHILD).
// H0-BLOCKER-4 fix: the two timeout-recovery loops are merged into a
// single helper to prevent divergence.

static WaitResult WaitWithDeadline(
    pid_t tid,
    const std::chrono::steady_clock::time_point& deadline) {
    WaitResult result{};
    const auto kPollInterval = std::chrono::milliseconds(10);

    // Unified timeout-recovery helper: send SIGSTOP, then wait a second
    // bounded interval for an actual stop. Returns immediately upon
    // WIFSTOPPED, WIFEXITED, or WIFSIGNALED.
    auto do_timeout_recovery = [&](WaitResult& r) -> void {
        (void)kill(tid, SIGSTOP);
        const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;) {
            int status = 0;
            const pid_t waited = waitpid(tid, &status, WNOHANG | __WALL);
            if (waited == -1) {
                if (errno == EINTR) continue;
                if (errno == ECHILD) {
                    r.tracee_exited = true;
                    r.wait_error = true;
                    r.error_code = ECHILD;
                    return;
                }
                r.wait_error = true;
                r.error_code = errno;
                return;
            }
            if (waited == 0) {
                if (std::chrono::steady_clock::now() >= stop_deadline) {
                    r.is_timeout = true;
                    return;
                }
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (waited != tid) {
                r.wait_error = true;
                r.error_code = -1;
                return;
            }
            r.waited_pid = waited;
            r.status = status;
            if (WIFSTOPPED(status)) {
                r.stop_confirmed = true;
                r.timeout_recovered = true;
                r.is_timeout = true;
                r.safe_to_restore = true;
                return;
            }
            // WIFEXITED or WIFSIGNALED: return immediately.
            // Do not call waitpid again.
            // Deadline expired, but we observed a real termination event.
            // The termination event takes priority over the timeout label.
            r.tracee_exited = true;
            // is_timeout stays false: we have a real exit, not just a timeout.
            return;
        }
    };

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            do_timeout_recovery(result);
            return result;
        }
        const auto remaining = deadline - now;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        const int ms_int = static_cast<int>(ms.count());
        if (ms_int <= 0) {
            do_timeout_recovery(result);
            return result;
        }
        // Normal polling: use WNOHANG + __WALL.
        int status = 0;
        const pid_t waited = waitpid(tid, &status, WNOHANG | __WALL);
        if (waited == -1) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                result.tracee_exited = true;
                result.wait_error = true;
                result.error_code = ECHILD;
                return result;
            }
            result.wait_error = true;
            result.error_code = errno;
            return result;
        }
        if (waited == 0) {
            // Nothing stopped yet; poll again.
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }
        if (waited != tid) {
            result.wait_error = true;
            result.error_code = -1;
            return result;
        }
        result.waited_pid = waited;
        result.status = status;
        if (WIFSTOPPED(status)) {
            result.stop_confirmed = true;
            result.safe_to_restore = true;
            return result;
        }
        // WIFEXITED or WIFSIGNALED: return immediately.
        // Do not call waitpid again.
        result.tracee_exited = true;
        return result;
    }
}

// Classify the stop reason from waitpid status and ptrace context.
static StopReason ClassifyStopReason(int status, pid_t waited_pid, pid_t target_tid) {
    if (waited_pid != target_tid) {
        return StopReason::kSpuriousWakeup;
    }
    if (WIFEXITED(status)) {
        return StopReason::kProcessExit;
    }
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        switch (sig) {
            case SIGSEGV: return StopReason::kSIGSEGV;
            case SIGBUS:  return StopReason::kSIGBUS;
            case SIGILL:  return StopReason::kSIGILL;
            case SIGSYS:  return StopReason::kSIGSYS;
            case SIGTRAP: return StopReason::kSIGTRAP;
            case SIGSTOP: return StopReason::kSIGSTOP;
            default:       return StopReason::kUnknown;
        }
    }
    if (WIFSTOPPED(status)) {
        const int sig = WSTOPSIG(status);
        switch (sig) {
            case SIGTRAP:
                // PTRACE_EVENT_* is encoded in the upper bits of status.
                if ((status >> 16) != 0) {
                    return StopReason::kPtraceEvent;
                }
                return StopReason::kSIGTRAP;
            case SIGSEGV: return StopReason::kSIGSEGV;
            case SIGBUS:  return StopReason::kSIGBUS;
            case SIGILL:  return StopReason::kSIGILL;
            case SIGSYS:  return StopReason::kSIGSYS;
            case SIGSTOP:
                // A raw SIGSTOP here is usually a group stop.
                return StopReason::kGroupStop;
            default:       return StopReason::kUnknown;
        }
    }
    return StopReason::kUnknown;
}

static pid_t ThreadGroupIdOfTid(pid_t tid) {
    std::ifstream status("/proc/" + std::to_string(tid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        int tgid = 0;
        if (std::sscanf(line.c_str(), "Tgid:\t%d", &tgid) == 1 && tgid > 0) {
            return static_cast<pid_t>(tgid);
        }
    }
    return 0;
}

// Read the state character from /proc/<pid>/stat.
// Returns 'Z' for zombie, 'X' for dead, 'R'/'S'/'D'/'T'/'W' etc for alive,
// or '\0' if /proc/<pid>/stat cannot be opened (pid does not exist).
static char ProcessStateChar(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (!f) return '\0';
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    // /proc/<pid>/stat format: pid (comm) state ...
    // comm may contain spaces and parens, so find the last ')' and read
    // the next non-space character.
    std::size_t last_paren = content.rfind(')');
    if (last_paren == std::string::npos) return '\0';
    std::size_t i = last_paren + 1;
    while (i < content.size() && content[i] == ' ') ++i;
    if (i >= content.size()) return '\0';
    return content[i];
}

// Check if a pid is truly alive (not zombie, not dead).
// kill(pid, 0) succeeds for zombies; we must check /proc/<pid>/stat.
static bool IsPidTrulyAlive(pid_t pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) == -1) return false;  // ESRCH or other
    const char state = ProcessStateChar(pid);
    if (state == '\0') return false;  // /proc not found
    if (state == 'Z') return false;   // zombie
    if (state == 'X') return false;   // dead
    return true;
}

// ---- End of H0 helpers ----

// RemoteCall v5: DR0/DR7 hardware-breakpoint remote call with bounded wait.
// Executed on the SPECIFIC tid passed in.
//
// Stop/resume protocol:
// - This implementation uses PTRACE_ATTACH for entry and detach on cleanup.
// - waitpid calls always use __WALL.
// - A timeout first requests a stop, then waits a second bounded interval for
//   an actual stopped state before any restore or detach is attempted.
//
// Returns true on success and fills *result with the return value (rax).
// On any failure, returns false and fills *report with diagnostic detail.
bool RemoteCall(pid_t tid, std::uintptr_t function, const std::uint64_t args[6],
                std::uint64_t* result, RemoteCallReport* report = nullptr) {
    RemoteCallReport local_report{};
    local_report.result = CallResult::kInternalError;
    local_report.stop_reason = StopReason::kUnknown;
    local_report.waitpid_result = 0;
    local_report.waitpid_status = 0;
    local_report.waited_pid = 0;
    local_report.pre_detach_target_alive = false;
    local_report.pre_detach_target_thread_alive = false;

    if (function == 0 || result == nullptr) {
        local_report.note = "null function or result";
        if (report != nullptr) *report = local_report;
        return false;
    }
    Mapping function_mapping{};
    if (!FindMapping(tid, function, &function_mapping) ||
        !function_mapping.readable || !function_mapping.executable) {
        local_report.note = "function not in r-x mapping";
        if (report != nullptr) *report = local_report;
        return false;
    }

    user_regs_struct saved{};
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &saved) == -1) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "GETREGS failed";
        if (report != nullptr) *report = local_report;
        return false;
    }

    std::uintptr_t call_stack = 0;
    if (!SelectVerifiedCallStack(tid, static_cast<std::uintptr_t>(saved.rsp),
                                 &call_stack)) {
        local_report.note = "no verified call stack";
        if (report != nullptr) *report = local_report;
        return false;
    }

    constexpr std::uintptr_t kDr0Offset = offsetof(user, u_debugreg[0]);
    constexpr std::uintptr_t kDr6Offset = offsetof(user, u_debugreg[6]);
    constexpr std::uintptr_t kDr7Offset = offsetof(user, u_debugreg[7]);
    long saved_dr0 = 0, saved_dr6 = 0, saved_dr7 = 0, saved_stack_word = 0;
    if (!PeekRemoteWord(tid, call_stack, &saved_stack_word)) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "peek stack word failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    errno = 0;
    saved_dr0 = ptrace(PTRACE_PEEKUSER, tid, kDr0Offset, nullptr);
    if (saved_dr0 == -1 && errno != 0) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "peek DR0 failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    errno = 0;
    saved_dr6 = ptrace(PTRACE_PEEKUSER, tid, kDr6Offset, nullptr);
    if (saved_dr6 == -1 && errno != 0) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "peek DR6 failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    errno = 0;
    saved_dr7 = ptrace(PTRACE_PEEKUSER, tid, kDr7Offset, nullptr);
    if (saved_dr7 == -1 && errno != 0) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "peek DR7 failed";
        if (report != nullptr) *report = local_report;
        return false;
    }

    // Save TGID before PTRACE_CONT for liveness distinction.
    const pid_t saved_tgid = ThreadGroupIdOfTid(tid);
    local_report.saved_tgid = saved_tgid;

    bool debug_touched = false;
    bool stack_touched = false;
    bool call_regs_touched = false;
    bool tracee_stopped = true;
    bool ok = true;

    if (ptrace(PTRACE_POKEUSER, tid, kDr7Offset, 0) == -1) {
        ok = false;
    } else {
        debug_touched = true;
    }
    if (ok && (ptrace(PTRACE_POKEUSER, tid, kDr0Offset, saved.rip) == -1 ||
               ptrace(PTRACE_POKEUSER, tid, kDr6Offset, 0) == -1 ||
               ptrace(PTRACE_POKEUSER, tid, kDr7Offset, 1) == -1)) {
        ok = false;
    }

    const std::uintptr_t return_address = saved.rip;
    if (ok) {
        ok = WriteRemote(tid, call_stack, &return_address, sizeof(return_address));
        stack_touched = ok;
    }
    user_regs_struct call = saved;
    if (ok) {
        call.rip = function;
        call.rsp = call_stack;
        call.rdi = args[0];
        call.rsi = args[1];
        call.rdx = args[2];
        call.rcx = args[3];
        call.r8 = args[4];
        call.r9 = args[5];
        ok = PokeUserRegs(tid, call);
        call_regs_touched = ok;
    }
    if (ok) {
        ok = ptrace(PTRACE_CONT, tid, nullptr, nullptr) != -1;
        if (ok) tracee_stopped = false;
    }

    const auto cont_start = std::chrono::steady_clock::now();
    if (ok) {
        const WaitResult wr =
            WaitWithDeadline(tid, cont_start + RcStageTimeout());
        local_report.waitpid_result = static_cast<int>(wr.waited_pid);
        local_report.waitpid_status = wr.status;
        local_report.waited_pid = wr.waited_pid;
        local_report.stop_confirmed = wr.stop_confirmed;
        tracee_stopped = wr.stop_confirmed;
        local_report.tracee_stopped = wr.stop_confirmed;

        if (wr.wait_error) {
            local_report.stop_reason = StopReason::kWaitError;
            if (wr.tracee_exited) {
                if (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid)) {
                    local_report.result = CallResult::kProcessExited;
                    local_report.stop_reason = StopReason::kProcessExit;
                } else {
                    local_report.result = CallResult::kThreadExited;
                    local_report.stop_reason = StopReason::kThreadExit;
                }
            } else {
                local_report.result = CallResult::kPtraceError;
            }
            ok = false;
        } else if (wr.tracee_exited && !wr.stop_confirmed) {
            // Real termination event takes priority over timeout label.
            if (WIFEXITED(wr.status)) {
                local_report.process_exited = true;
                local_report.exit_code = WEXITSTATUS(wr.status);
            }
            if (WIFSIGNALED(wr.status)) {
                local_report.signaled = true;
                local_report.signal_number = WTERMSIG(wr.status);
                local_report.core_dumped = WCOREDUMP(wr.status);
            }
            if (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid)) {
                local_report.stop_reason = StopReason::kProcessExit;
                local_report.result = CallResult::kProcessExited;
            } else {
                local_report.stop_reason = StopReason::kThreadExit;
                local_report.result = CallResult::kThreadExited;
            }
            ok = false;
        } else if (wr.is_timeout && !wr.stop_confirmed) {
            local_report.stop_reason = StopReason::kTimeout;
            local_report.result = CallResult::kTimeout;
            ok = false;
        } else {
            // stop_confirmed == true (normal or timeout-recovered).
            // Unified diagnostics: always capture registers/DRs/stack first.
            local_report.stop_signal = WSTOPSIG(wr.status);
            local_report.stop_reason = ClassifyStopReason(wr.status, wr.waited_pid, tid);
            user_regs_struct returned{};
            long dr6 = 0;
            long dr0 = 0;
            long dr7 = 0;
            long stack_word = 0;
            const bool regs_ok = ptrace(PTRACE_GETREGS, tid, nullptr, &returned) != -1;
            local_report.registers_valid = regs_ok;
            errno = 0;
            dr6 = ptrace(PTRACE_PEEKUSER, tid, kDr6Offset, nullptr);
            const bool dr6_ok = !(dr6 == -1 && errno != 0);
            errno = 0;
            dr0 = ptrace(PTRACE_PEEKUSER, tid, kDr0Offset, nullptr);
            const bool dr0_ok = !(dr0 == -1 && errno != 0);
            errno = 0;
            dr7 = ptrace(PTRACE_PEEKUSER, tid, kDr7Offset, nullptr);
            const bool dr7_ok = !(dr7 == -1 && errno != 0);
            local_report.debug_registers_valid = dr6_ok && dr0_ok && dr7_ok;
            const bool stack_ok = PeekRemoteWord(tid, call_stack, &stack_word);
            local_report.stack_word_valid = stack_ok;
            if (regs_ok) {
                local_report.rax = returned.rax;
                local_report.rip = returned.rip;
                local_report.rsp = returned.rsp;
                local_report.rdi = returned.rdi;
                local_report.rsi = returned.rsi;
                local_report.rdx = returned.rdx;
                local_report.rcx = returned.rcx;
                local_report.r8 = returned.r8;
                local_report.r9 = returned.r9;
            }
            if (dr0_ok) local_report.dr0 = static_cast<std::uint64_t>(dr0);
            if (dr6_ok) local_report.dr6 = static_cast<std::uint64_t>(dr6);
            if (dr7_ok) local_report.dr7 = static_cast<std::uint64_t>(dr7);
            if (stack_ok) local_report.saved_stack_word = static_cast<std::uint64_t>(stack_word);
            local_report.elapsed_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - cont_start)
                    .count());

            // Now classify.
            if (wr.is_timeout) {
                // Timeout-recovered stop: diagnostics collected, but result
                // is still kTimeout.
                local_report.stop_reason = StopReason::kTimeout;
                local_report.result = CallResult::kTimeout;
                ok = false;
            } else if (local_report.stop_signal != SIGTRAP) {
                switch (local_report.stop_signal) {
                    case SIGSEGV: local_report.result = CallResult::kTargetSegv; break;
                    case SIGBUS:  local_report.result = CallResult::kTargetBus; break;
                    case SIGILL:  local_report.result = CallResult::kTargetIll; break;
                    case SIGSYS:  local_report.result = CallResult::kTargetSys; break;
                    case SIGSTOP: local_report.result = CallResult::kInternalError; break;
                    default:      local_report.result = CallResult::kInternalError; break;
                }
                ok = false;
            } else {
                const bool provenance_ok = regs_ok && returned.rip == saved.rip && (dr6 & 0x1) != 0;
                if (provenance_ok) {
                    *result = returned.rax;
                    local_report.result = CallResult::kSuccess;
                } else {
                    local_report.result = CallResult::kWrongTrap;
                    std::fprintf(stderr,
                                 "rc: trap provenance failed rip=0x%llx expected=0x%llx dr6=0x%lx\n",
                                 static_cast<unsigned long long>(returned.rip),
                                 static_cast<unsigned long long>(saved.rip), dr6);
                    ok = false;
                }
            }
        }
    }

    // Liveness checks: use saved_tgid to distinguish thread vs process exit.
    // These are pre-detach measurements; controller must re-check after detach.
    // Use IsPidTrulyAlive to detect zombies (kill(pid,0) succeeds for zombies).
    local_report.pre_detach_target_thread_alive = IsPidTrulyAlive(tid);
    if (saved_tgid > 0) {
        local_report.pre_detach_target_alive = IsPidTrulyAlive(saved_tgid);
    } else {
        local_report.pre_detach_target_alive = local_report.pre_detach_target_thread_alive;
    }

    if (!tracee_stopped) {
        std::fprintf(stderr, "rc: tracee is not stopped; state cannot be restored\n");
        local_report.detach_safe = false;
        if (report != nullptr) *report = local_report;
        return false;
    }
    // Restore state: RemoteCall is the single owner of stop/restore.
    local_report.rollback_attempted = true;
    bool restored = true;
    if (debug_touched) {
        restored = ptrace(PTRACE_POKEUSER, tid, kDr7Offset, 0) != -1 && restored;
        restored = ptrace(PTRACE_POKEUSER, tid, kDr0Offset, saved_dr0) != -1 && restored;
        restored = ptrace(PTRACE_POKEUSER, tid, kDr6Offset, saved_dr6) != -1 && restored;
        restored = ptrace(PTRACE_POKEUSER, tid, kDr7Offset, saved_dr7) != -1 && restored;
    }
    if (call_regs_touched) {
        user_regs_struct rollback = saved;
        rollback.rip =
            saved.rip + static_cast<unsigned long long>(RcRipBias());
        restored = PokeUserRegs(tid, rollback) && restored;
    }
    if (stack_touched)
        restored = WriteRemote(tid, call_stack, &saved_stack_word,
                               sizeof(saved_stack_word)) && restored;
    local_report.rollback_succeeded = restored;
    if (!restored) local_report.result = CallResult::kRollbackFailed;
    local_report.detach_safe = restored && tracee_stopped;
    if (report != nullptr) *report = local_report;
    if (!restored) std::fprintf(stderr, "rc: mandatory rollback failed\n");
    return ok && restored;
}

// RemoteCall v6: software-breakpoint (int3 return stub) remote call.
//
// LDPlayer9's x86_64 ptrace surface produced unreliable DR6 on hardware
// breakpoints (H0 isolated test 20260816: ill became success, ok became
// wrong_trap). This variant does not touch DR0-DR7 at all. The caller must
// provide an executable page whose first byte is 0xCC. The remote call uses
// that address as the return address, so a normal function return produces a
// clean SIGTRAP with RIP == trap_address.
bool RemoteCallInt3(pid_t tid, std::uintptr_t function,
                    const std::uint64_t args[6], std::uintptr_t trap_address,
                    std::uint64_t* result, RemoteCallReport* report = nullptr,
                    const user_regs_struct* rollback_regs = nullptr) {
    RemoteCallReport local_report{};
    local_report.result = CallResult::kInternalError;
    local_report.stop_reason = StopReason::kUnknown;
    local_report.pre_detach_target_alive = false;
    local_report.pre_detach_target_thread_alive = false;

    if (function == 0 || trap_address == 0 || result == nullptr) {
        local_report.note = "null function, trap address or result";
        if (report != nullptr) *report = local_report;
        return false;
    }
    Mapping function_mapping{};
    if (!FindMapping(tid, function, &function_mapping) ||
        !function_mapping.readable || !function_mapping.executable) {
        local_report.note = "function not in r-x mapping";
        if (report != nullptr) *report = local_report;
        return false;
    }
    Mapping trap_mapping{};
    long trap_word = 0;
    if (!FindMapping(tid, trap_address, &trap_mapping) ||
        !trap_mapping.readable || !trap_mapping.executable) {
        local_report.note = "trap address not in r-x mapping";
        if (report != nullptr) *report = local_report;
        return false;
    }
    if (!PeekRemoteWord(tid, trap_address, &trap_word) ||
        (trap_word & 0xFF) != 0xCC) {
        local_report.note = "trap stub byte is not 0xCC";
        if (report != nullptr) *report = local_report;
        return false;
    }

    user_regs_struct saved{};
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &saved) == -1) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "GETREGS failed";
        if (report != nullptr) *report = local_report;
        return false;
    }

    std::uintptr_t call_stack = 0;
    if (!SelectVerifiedCallStack(tid, static_cast<std::uintptr_t>(saved.rsp),
                                 &call_stack)) {
        local_report.note = "no verified call stack";
        if (report != nullptr) *report = local_report;
        return false;
    }

    long saved_stack_word = 0;
    if (!PeekRemoteWord(tid, call_stack, &saved_stack_word)) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "peek stack word failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    local_report.stack_word_valid = true;

    const pid_t saved_tgid = ThreadGroupIdOfTid(tid);
    local_report.saved_tgid = saved_tgid;

    bool stack_touched = false;
    bool call_regs_touched = false;
    bool tracee_stopped = true;
    bool ok = true;

    const std::uintptr_t return_address = trap_address;
    ok = WriteRemote(tid, call_stack, &return_address, sizeof(return_address));
    stack_touched = ok;

    user_regs_struct call = saved;
    if (ok) {
        call.rip = function + static_cast<std::uintptr_t>(RcRipBias());
        call.rsp = call_stack;
        call.rdi = args[0];
        call.rsi = args[1];
        call.rdx = args[2];
        call.rcx = args[3];
        call.r8 = args[4];
        call.r9 = args[5];
        ok = PokeUserRegs(tid, call);
        call_regs_touched = ok;
    }
    if (ok) {
        ok = ptrace(PTRACE_CONT, tid, nullptr, nullptr) != -1;
        if (ok) tracee_stopped = false;
        std::fprintf(stderr,
                     "rc_int3 tid=%d function=0x%llx trap=0x%llx "
                     "saved_rip=0x%llx saved_rsp=0x%llx call_stack=0x%llx "
                     "bias=%ld cont=%d\n",
                     static_cast<int>(tid),
                     static_cast<unsigned long long>(function),
                     static_cast<unsigned long long>(trap_address),
                     static_cast<unsigned long long>(saved.rip),
                     static_cast<unsigned long long>(saved.rsp),
                     static_cast<unsigned long long>(call_stack), RcRipBias(),
                     ok ? 1 : 0);
    }

    const auto cont_start = std::chrono::steady_clock::now();
    if (ok) {
        const WaitResult wr =
            WaitWithDeadline(tid, cont_start + RcStageTimeout());
        local_report.waitpid_result = static_cast<int>(wr.waited_pid);
        local_report.waitpid_status = wr.status;
        local_report.waited_pid = wr.waited_pid;
        local_report.stop_confirmed = wr.stop_confirmed;
        tracee_stopped = wr.stop_confirmed;
        local_report.tracee_stopped = wr.stop_confirmed;

        if (wr.wait_error) {
            local_report.stop_reason = StopReason::kWaitError;
            if (wr.tracee_exited) {
                if (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid)) {
                    local_report.result = CallResult::kProcessExited;
                    local_report.stop_reason = StopReason::kProcessExit;
                } else {
                    local_report.result = CallResult::kThreadExited;
                    local_report.stop_reason = StopReason::kThreadExit;
                }
            } else {
                local_report.result = CallResult::kPtraceError;
            }
            ok = false;
        } else if (wr.tracee_exited && !wr.stop_confirmed) {
            if (WIFEXITED(wr.status)) {
                local_report.process_exited = true;
                local_report.exit_code = WEXITSTATUS(wr.status);
            }
            if (WIFSIGNALED(wr.status)) {
                local_report.signaled = true;
                local_report.signal_number = WTERMSIG(wr.status);
                local_report.core_dumped = WCOREDUMP(wr.status);
            }
            if (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid)) {
                local_report.stop_reason = StopReason::kProcessExit;
                local_report.result = CallResult::kProcessExited;
            } else {
                local_report.stop_reason = StopReason::kThreadExit;
                local_report.result = CallResult::kThreadExited;
            }
            ok = false;
        } else if (wr.is_timeout && !wr.stop_confirmed) {
            local_report.stop_reason = StopReason::kTimeout;
            local_report.result = CallResult::kTimeout;
            ok = false;
        } else {
            local_report.stop_signal = WSTOPSIG(wr.status);
            user_regs_struct returned{};
            const bool regs_ok = ptrace(PTRACE_GETREGS, tid, nullptr, &returned) != -1;
            local_report.registers_valid = regs_ok;
            if (regs_ok) {
                local_report.rax = returned.rax;
                local_report.rip = returned.rip;
                local_report.rsp = returned.rsp;
                local_report.rdi = returned.rdi;
                local_report.rsi = returned.rsi;
                local_report.rdx = returned.rdx;
                local_report.rcx = returned.rcx;
                local_report.r8 = returned.r8;
                local_report.r9 = returned.r9;
            }
            local_report.elapsed_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - cont_start)
                    .count());

            if (wr.is_timeout) {
                local_report.stop_reason = StopReason::kTimeout;
                local_report.result = CallResult::kTimeout;
                ok = false;
            } else if (local_report.stop_signal != SIGTRAP) {
                switch (local_report.stop_signal) {
                    case SIGSEGV: local_report.result = CallResult::kTargetSegv; break;
                    case SIGBUS:  local_report.result = CallResult::kTargetBus; break;
                    case SIGILL:  local_report.result = CallResult::kTargetIll; break;
                    case SIGSYS:  local_report.result = CallResult::kTargetSys; break;
                    case SIGSTOP: local_report.result = CallResult::kInternalError; break;
                    default:      local_report.result = CallResult::kInternalError; break;
                }
                ok = false;
            } else if (regs_ok &&
                       (returned.rip == trap_address ||
                        returned.rip == trap_address + 1)) {
                // On x86 the trap instruction advances RIP by one byte.
                *result = returned.rax;
                local_report.result = CallResult::kSuccess;
            } else {
                local_report.result = CallResult::kWrongTrap;
                std::fprintf(stderr,
                             "rc_int3: trap provenance failed rip=0x%llx expected=0x%llx\n",
                             static_cast<unsigned long long>(returned.rip),
                             static_cast<unsigned long long>(trap_address));
                ok = false;
            }
        }
    }

    local_report.pre_detach_target_thread_alive = IsPidTrulyAlive(tid);
    if (saved_tgid > 0) {
        local_report.pre_detach_target_alive = IsPidTrulyAlive(saved_tgid);
    } else {
        local_report.pre_detach_target_alive =
            local_report.pre_detach_target_thread_alive;
    }

    if (!tracee_stopped) {
        local_report.detach_safe = false;
        if (report != nullptr) *report = local_report;
        return false;
    }
    local_report.rollback_attempted = true;
    bool restored = true;
    if (call_regs_touched) {
        user_regs_struct rollback = rollback_regs != nullptr ? *rollback_regs
                                                             : saved;
        rollback.rip =
            rollback.rip + static_cast<unsigned long long>(RcRipBias());
        restored = PokeUserRegs(tid, rollback) && restored;
    }
    if (stack_touched)
        restored = WriteRemote(tid, call_stack, &saved_stack_word,
                               sizeof(saved_stack_word)) && restored;
    local_report.rollback_succeeded = restored;
    if (!restored) local_report.result = CallResult::kRollbackFailed;
    local_report.detach_safe = restored && tracee_stopped;
    if (report != nullptr) *report = local_report;
    if (!restored) std::fprintf(stderr, "rc_int3: mandatory rollback failed\n");
    return ok && restored;
}

// RemoteCall v7: two-stage software-breakpoint remote call.
//
// Stage 1 patches saved.rip with 0xCC and lets the thread trap there first.
// This escapes the syscall-stop register restoration observed on LDPlayer9,
// where a direct register rewrite could be overwritten by PTRACE_CONT.
// Stage 2 then writes the real call registers from a clean user-space trap
// and runs the target function, returning through a separate 0xCC stub.
bool RemoteCallInt3TwoStage(pid_t tid, std::uintptr_t function,
                            const std::uint64_t args[6],
                            std::uintptr_t trap_address,
                            std::uint64_t* result,
                            RemoteCallReport* report = nullptr) {
    RemoteCallReport local_report{};
    local_report.result = CallResult::kInternalError;
    local_report.stop_reason = StopReason::kUnknown;
    local_report.pre_detach_target_alive = false;
    local_report.pre_detach_target_thread_alive = false;

    if (function == 0 || trap_address == 0 || result == nullptr) {
        local_report.note = "null function, trap address or result";
        if (report != nullptr) *report = local_report;
        return false;
    }
    Mapping function_mapping{};
    if (!FindMapping(tid, function, &function_mapping) ||
        !function_mapping.readable || !function_mapping.executable) {
        local_report.note = "function not in r-x mapping";
        if (report != nullptr) *report = local_report;
        return false;
    }
    Mapping trap_mapping{};
    long trap_word = 0;
    if (!FindMapping(tid, trap_address, &trap_mapping) ||
        !trap_mapping.readable || !trap_mapping.executable ||
        !PeekRemoteWord(tid, trap_address, &trap_word) ||
        (trap_word & 0xFF) != 0xCC) {
        local_report.note = "trap stub invalid";
        if (report != nullptr) *report = local_report;
        return false;
    }

    user_regs_struct saved{};
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &saved) == -1) {
        local_report.result = CallResult::kPtraceError;
        local_report.note = "GETREGS failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    Mapping entry_mapping{};
    if (!FindMapping(tid, static_cast<std::uintptr_t>(saved.rip),
                     &entry_mapping) ||
        !entry_mapping.readable || !entry_mapping.executable) {
        local_report.note = "saved.rip not in r-x mapping";
        if (report != nullptr) *report = local_report;
        return false;
    }
    long entry_word = 0;
    if (!PeekRemoteWord(tid, static_cast<std::uintptr_t>(saved.rip),
                        &entry_word)) {
        local_report.note = "peek entry word failed";
        if (report != nullptr) *report = local_report;
        return false;
    }

    std::uintptr_t call_stack = 0;
    if (!SelectVerifiedCallStack(tid, static_cast<std::uintptr_t>(saved.rsp),
                                 &call_stack)) {
        local_report.note = "no verified call stack";
        if (report != nullptr) *report = local_report;
        return false;
    }
    long saved_stack_word = 0;
    if (!PeekRemoteWord(tid, call_stack, &saved_stack_word)) {
        local_report.note = "peek stack word failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    local_report.stack_word_valid = true;

    const pid_t saved_tgid = ThreadGroupIdOfTid(tid);
    local_report.saved_tgid = saved_tgid;

    bool stack_touched = false;
    bool call_regs_touched = false;
    bool tracee_stopped = true;
    bool ok = true;

    // Stage 1: software trap at the thread's own next instruction.
    // The thread is often stopped at a syscall exit. Set orig_rax=-1 and
    // rax=-ENOSYS so PTRACE_CONT does not restart the interrupted syscall
    // (nanosleep/futex) and instead continues at saved.rip.
    user_regs_struct resume = saved;
    resume.orig_rax = static_cast<unsigned long>(-1);
    resume.rax = static_cast<unsigned long>(-ENOSYS);
    if (!PokeUserRegs(tid, resume)) {
        local_report.note = "stage1 poke no-restart regs failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    const std::uint8_t int3 = 0xCC;
    if (!WriteRemote(tid, static_cast<std::uintptr_t>(saved.rip), &int3,
                     sizeof(int3))) {
        (void)PokeUserRegs(tid, saved);
        local_report.note = "stage1 patch saved.rip failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) == -1) {
        (void)WriteRemote(tid, static_cast<std::uintptr_t>(saved.rip),
                          &entry_word, sizeof(entry_word));
        local_report.result = CallResult::kPtraceError;
        local_report.note = "stage1 CONT failed";
        if (report != nullptr) *report = local_report;
        return false;
    }
    tracee_stopped = false;
    const auto stage1_start = std::chrono::steady_clock::now();
    const WaitResult wr1 = WaitWithDeadline(tid, stage1_start + RcStageTimeout());
    local_report.waitpid_result = static_cast<int>(wr1.waited_pid);
    local_report.waitpid_status = wr1.status;
    local_report.waited_pid = wr1.waited_pid;
    local_report.stop_confirmed = wr1.stop_confirmed;
    tracee_stopped = wr1.stop_confirmed;
    local_report.tracee_stopped = wr1.stop_confirmed;
    const bool entry_restored =
        WriteRemote(tid, static_cast<std::uintptr_t>(saved.rip), &entry_word,
                    sizeof(entry_word));

    if (wr1.wait_error || wr1.tracee_exited ||
        (wr1.is_timeout && !wr1.stop_confirmed)) {
        if (wr1.tracee_exited) {
            local_report.result = (saved_tgid > 0 &&
                                   !IsPidTrulyAlive(saved_tgid))
                                      ? CallResult::kProcessExited
                                      : CallResult::kThreadExited;
            local_report.stop_reason = local_report.result ==
                                               CallResult::kProcessExited
                                           ? StopReason::kProcessExit
                                           : StopReason::kThreadExit;
        } else if (wr1.wait_error) {
            local_report.result = CallResult::kPtraceError;
            local_report.stop_reason = StopReason::kWaitError;
        } else {
            local_report.result = CallResult::kTimeout;
            local_report.stop_reason = StopReason::kTimeout;
        }
        local_report.rollback_attempted = true;
        local_report.rollback_succeeded = entry_restored;
        local_report.detach_safe = entry_restored && tracee_stopped;
        if (report != nullptr) *report = local_report;
        return false;
    }

    user_regs_struct stage1_regs{};
    const bool stage1_regs_ok =
        ptrace(PTRACE_GETREGS, tid, nullptr, &stage1_regs) != -1;
    local_report.registers_valid = stage1_regs_ok;
    const int stage1_signal = WSTOPSIG(wr1.status);
    local_report.stop_signal = stage1_signal;
    const bool stage1_trap_ok =
        stage1_regs_ok && stage1_signal == SIGTRAP &&
        (stage1_regs.rip == saved.rip || stage1_regs.rip == saved.rip + 1);
    if (!stage1_trap_ok) {
        local_report.result = CallResult::kWrongTrap;
        std::fprintf(stderr,
                     "rc_int3_2stage: stage1 trap failed sig=%d rip=0x%llx "
                     "expected=0x%llx\n",
                     stage1_signal,
                     static_cast<unsigned long long>(stage1_regs.rip),
                     static_cast<unsigned long long>(saved.rip));
        local_report.rollback_attempted = true;
        local_report.rollback_succeeded = entry_restored;
        local_report.detach_safe = entry_restored && tracee_stopped;
        if (report != nullptr) *report = local_report;
        return false;
    }

    // Stage 2: real call from a clean user-space trap stop.
    const std::uintptr_t return_address = trap_address;
    ok = WriteRemote(tid, call_stack, &return_address, sizeof(return_address));
    stack_touched = ok;
    user_regs_struct call = saved;
    if (ok) {
        call.rip = function + static_cast<std::uintptr_t>(RcRipBias());
        call.rsp = call_stack;
        call.rdi = args[0];
        call.rsi = args[1];
        call.rdx = args[2];
        call.rcx = args[3];
        call.r8 = args[4];
        call.r9 = args[5];
        ok = PokeUserRegs(tid, call);
        call_regs_touched = ok;
    }
    if (ok && !RcUseSingleStep()) {
        std::fprintf(stderr,
                     "rc_int3_2stage cont-mode tid=%d function=0x%llx "
                     "trap=0x%llx bias=%ld saved_rip=0x%llx\n",
                     static_cast<int>(tid),
                     static_cast<unsigned long long>(function),
                     static_cast<unsigned long long>(trap_address),
                     RcRipBias(),
                     static_cast<unsigned long long>(saved.rip));
    }
    if (ok && RcUseSingleStep()) {
        const auto stage2_start = std::chrono::steady_clock::now();
        // Stage 2a: execute exactly the first instruction of the target
        // function. This verifies that the emulator really entered `function`
        // instead of resuming one/two bytes before it after the stage1 trap.
        ok = ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr) != -1;
        if (ok) tracee_stopped = false;
        const WaitResult wr_ss =
            WaitWithDeadline(tid, stage2_start + RcStageTimeout());
        local_report.waitpid_result = static_cast<int>(wr_ss.waited_pid);
        local_report.waitpid_status = wr_ss.status;
        local_report.waited_pid = wr_ss.waited_pid;
        local_report.stop_confirmed = wr_ss.stop_confirmed;
        tracee_stopped = wr_ss.stop_confirmed;
        local_report.tracee_stopped = wr_ss.stop_confirmed;
        std::fprintf(stderr,
                     "rc_int3_ss tid=%d function=0x%llx trap=0x%llx "
                     "saved_rip=0x%llx bias=%ld single_step=%d\n",
                     static_cast<int>(tid),
                     static_cast<unsigned long long>(function),
                     static_cast<unsigned long long>(trap_address),
                     static_cast<unsigned long long>(saved.rip), RcRipBias(),
                     ok ? 1 : 0);

        if (ok && wr_ss.wait_error) {
            local_report.stop_reason = StopReason::kWaitError;
            if (wr_ss.tracee_exited) {
                local_report.result =
                    (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid))
                        ? CallResult::kProcessExited
                        : CallResult::kThreadExited;
                local_report.stop_reason =
                    local_report.result == CallResult::kProcessExited
                        ? StopReason::kProcessExit
                        : StopReason::kThreadExit;
            } else {
                local_report.result = CallResult::kPtraceError;
            }
            ok = false;
        } else if (ok && wr_ss.tracee_exited && !wr_ss.stop_confirmed) {
            local_report.result =
                (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid))
                    ? CallResult::kProcessExited
                    : CallResult::kThreadExited;
            local_report.stop_reason =
                local_report.result == CallResult::kProcessExited
                    ? StopReason::kProcessExit
                    : StopReason::kThreadExit;
            ok = false;
        } else if (ok && wr_ss.is_timeout && !wr_ss.stop_confirmed) {
            local_report.stop_reason = StopReason::kTimeout;
            local_report.result = CallResult::kTimeout;
            ok = false;
        } else if (ok) {
            const int ss_signal = WSTOPSIG(wr_ss.status);
            user_regs_struct ss_regs{};
            const bool ss_regs_ok =
                ptrace(PTRACE_GETREGS, tid, nullptr, &ss_regs) != -1;
            local_report.registers_valid = ss_regs_ok;
            local_report.stop_signal = ss_signal;
            if (ss_signal != SIGTRAP) {
                switch (ss_signal) {
                    case SIGSEGV: local_report.result = CallResult::kTargetSegv; break;
                    case SIGBUS:  local_report.result = CallResult::kTargetBus; break;
                    case SIGILL:  local_report.result = CallResult::kTargetIll; break;
                    case SIGSYS:  local_report.result = CallResult::kTargetSys; break;
                    case SIGSTOP: local_report.result = CallResult::kInternalError; break;
                    default:      local_report.result = CallResult::kInternalError; break;
                }
                ok = false;
            } else if (!ss_regs_ok ||
                       ss_regs.rip < function + 1 ||
                       ss_regs.rip > function + 0x10) {
                local_report.result = CallResult::kWrongTrap;
                std::fprintf(stderr,
                             "rc_int3_ss: entry verification failed rip=0x%llx "
                             "function=0x%llx\n",
                             static_cast<unsigned long long>(ss_regs.rip),
                             static_cast<unsigned long long>(function));
                ok = false;
            } else {
                // Stage 2b: entry verified; run the rest of the function.
                ok = ptrace(PTRACE_CONT, tid, nullptr, nullptr) != -1;
                if (ok) tracee_stopped = false;
            }
        }
    }

    if (ok) {
        const auto stage2b_start = std::chrono::steady_clock::now();
        const WaitResult wr2 =
            WaitWithDeadline(tid, stage2b_start + RcStageTimeout());
        local_report.waitpid_result = static_cast<int>(wr2.waited_pid);
        local_report.waitpid_status = wr2.status;
        local_report.waited_pid = wr2.waited_pid;
        local_report.stop_confirmed = wr2.stop_confirmed;
        tracee_stopped = wr2.stop_confirmed;
        local_report.tracee_stopped = wr2.stop_confirmed;

        if (wr2.wait_error) {
            local_report.stop_reason = StopReason::kWaitError;
            if (wr2.tracee_exited) {
                if (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid)) {
                    local_report.result = CallResult::kProcessExited;
                    local_report.stop_reason = StopReason::kProcessExit;
                } else {
                    local_report.result = CallResult::kThreadExited;
                    local_report.stop_reason = StopReason::kThreadExit;
                }
            } else {
                local_report.result = CallResult::kPtraceError;
            }
            ok = false;
        } else if (wr2.tracee_exited && !wr2.stop_confirmed) {
            if (WIFEXITED(wr2.status)) {
                local_report.process_exited = true;
                local_report.exit_code = WEXITSTATUS(wr2.status);
            }
            if (WIFSIGNALED(wr2.status)) {
                local_report.signaled = true;
                local_report.signal_number = WTERMSIG(wr2.status);
                local_report.core_dumped = WCOREDUMP(wr2.status);
            }
            if (saved_tgid > 0 && !IsPidTrulyAlive(saved_tgid)) {
                local_report.stop_reason = StopReason::kProcessExit;
                local_report.result = CallResult::kProcessExited;
            } else {
                local_report.stop_reason = StopReason::kThreadExit;
                local_report.result = CallResult::kThreadExited;
            }
            ok = false;
        } else if (wr2.is_timeout && !wr2.stop_confirmed) {
            local_report.stop_reason = StopReason::kTimeout;
            local_report.result = CallResult::kTimeout;
            ok = false;
        } else {
            local_report.stop_signal = WSTOPSIG(wr2.status);
            user_regs_struct returned{};
            const bool regs_ok =
                ptrace(PTRACE_GETREGS, tid, nullptr, &returned) != -1;
            local_report.registers_valid = regs_ok;
            if (regs_ok) {
                local_report.rax = returned.rax;
                local_report.rip = returned.rip;
                local_report.rsp = returned.rsp;
                local_report.rdi = returned.rdi;
                local_report.rsi = returned.rsi;
                local_report.rdx = returned.rdx;
                local_report.rcx = returned.rcx;
                local_report.r8 = returned.r8;
                local_report.r9 = returned.r9;
            }
            local_report.elapsed_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - stage2b_start)
                    .count());
            if (wr2.is_timeout) {
                local_report.stop_reason = StopReason::kTimeout;
                local_report.result = CallResult::kTimeout;
                ok = false;
            } else if (local_report.stop_signal != SIGTRAP) {
                switch (local_report.stop_signal) {
                    case SIGSEGV: local_report.result = CallResult::kTargetSegv; break;
                    case SIGBUS:  local_report.result = CallResult::kTargetBus; break;
                    case SIGILL:  local_report.result = CallResult::kTargetIll; break;
                    case SIGSYS:  local_report.result = CallResult::kTargetSys; break;
                    case SIGSTOP: local_report.result = CallResult::kInternalError; break;
                    default:      local_report.result = CallResult::kInternalError; break;
                }
                ok = false;
            } else if (regs_ok &&
                       (returned.rip == trap_address ||
                        returned.rip == trap_address + 1)) {
                *result = returned.rax;
                local_report.result = CallResult::kSuccess;
            } else {
                local_report.result = CallResult::kWrongTrap;
                std::fprintf(stderr,
                             "rc_int3_2stage: stage2 trap provenance failed "
                             "rip=0x%llx expected=0x%llx\n",
                             static_cast<unsigned long long>(returned.rip),
                             static_cast<unsigned long long>(trap_address));
                ok = false;
            }
        }
    }

    local_report.pre_detach_target_thread_alive = IsPidTrulyAlive(tid);
    if (saved_tgid > 0) {
        local_report.pre_detach_target_alive = IsPidTrulyAlive(saved_tgid);
    } else {
        local_report.pre_detach_target_alive =
            local_report.pre_detach_target_thread_alive;
    }

    if (!tracee_stopped) {
        local_report.detach_safe = false;
        if (report != nullptr) *report = local_report;
        return false;
    }
    local_report.rollback_attempted = true;
    bool restored = entry_restored;
    if (call_regs_touched) {
        user_regs_struct rollback = saved;
        rollback.rip =
            saved.rip + static_cast<unsigned long long>(RcRipBias());
        restored = PokeUserRegs(tid, rollback) && restored;
    }
    if (stack_touched)
        restored = WriteRemote(tid, call_stack, &saved_stack_word,
                               sizeof(saved_stack_word)) && restored;
    local_report.rollback_succeeded = restored;
    if (!restored) local_report.result = CallResult::kRollbackFailed;
    local_report.detach_safe = restored && tracee_stopped;
    if (report != nullptr) *report = local_report;
    if (!restored)
        std::fprintf(stderr, "rc_int3_2stage: mandatory rollback failed\n");
    return ok && restored;
}

}  // namespace

// ---- H0 v8: session-based int3 RemoteCall for the bootstrap injector ----
// A session keeps the register state captured right after PTRACE_ATTACH so
// that the FINAL rollback of a multi-call sequence (dlerror -> mmap -> write
// -> dlopen -> dlerror) restores the thread to its original state, not to
// the state of the latest int3 return-stub stop.

struct RemoteCallSession {
    pid_t tid = 0;
    std::uintptr_t trap_address = 0;
    user_regs_struct original{};
};

static bool RemoteCallSessionInit(pid_t tid, std::uintptr_t trap_address,
                                  RemoteCallSession* session) {
    if (session == nullptr || tid <= 0 || trap_address == 0) return false;
    session->tid = tid;
    session->trap_address = trap_address;
    return ptrace(PTRACE_GETREGS, tid, nullptr, &session->original) != -1;
}

static bool RemoteCallSessionCall(RemoteCallSession* session,
                                  std::uintptr_t function,
                                  const std::uint64_t args[6],
                                  std::uint64_t* result,
                                  RemoteCallReport* report = nullptr) {
    if (session == nullptr || result == nullptr) return false;
    return RemoteCallInt3(session->tid, function, args,
                          session->trap_address, result, report,
                          &session->original);
}

// LDPlayer9's RIP-resume offset is not globally constant across every
// thread/process state (H0/offline nanosleep threads need +2; the game's
// Signal Catcher and busy spinner threads were observed resuming at the
// written RIP with no offset). Before running the bootstrap sequence, probe
// with the harmless `gettid` wrapper: for each bias candidate the remote call
// must return through the int3 stub with rax == this thread's tid.
static bool CalibrateRipBias(RemoteCallSession* session,
                             std::uintptr_t gettid_fn) {
    if (session == nullptr || gettid_fn == 0) return false;
    const long preferred = RcRipBias();
    long candidates[] = {preferred, 0, 2, -2, 4};
    constexpr std::size_t kCount = sizeof(candidates) / sizeof(candidates[0]);
    const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};

    for (std::size_t i = 0; i < kCount; ++i) {
        const long bias = candidates[i];
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (candidates[j] == bias) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        RcSetRipBias(bias);

        std::uint64_t value = 0;
        RemoteCallReport report{};
        const bool ok = RemoteCallInt3(session->tid, gettid_fn, zero_args,
                                       session->trap_address, &value, &report,
                                       &session->original);
        std::fprintf(stderr,
                     "rc calibrate bias=%ld ok=%d result=%s value=%llu "
                     "expected=%d status=0x%x\n",
                     bias, ok ? 1 : 0, RemoteCallResultName(report.result),
                     static_cast<unsigned long long>(value),
                     static_cast<int>(session->tid),
                     report.waitpid_status);
        if (ok && value == static_cast<std::uint64_t>(session->tid)) {
            std::fprintf(stderr, "rc calibrated rip bias=%ld tid=%d\n", bias,
                         static_cast<int>(session->tid));
            return true;
        }
        if (!report.stop_confirmed ||
            report.result == CallResult::kProcessExited ||
            report.result == CallResult::kThreadExited) {
            std::fprintf(stderr,
                         "rc calibration aborted: tracee no longer stopped "
                         "tid=%d result=%s\n",
                         static_cast<int>(session->tid),
                         RemoteCallResultName(report.result));
            return false;
        }
    }
    std::fprintf(stderr, "rc calibration failed: no working rip bias tid=%d\n",
                 static_cast<int>(session->tid));
    return false;
}

// Read a NUL-terminated string from the stopped tracee via PTRACE_PEEKDATA.
static bool ReadRemoteString(pid_t tid, std::uintptr_t address, char* output,
                             std::size_t capacity) {
    if (output == nullptr || capacity == 0 || address == 0) return false;
    for (std::size_t done = 0; done < capacity;) {
        long word = 0;
        if (!PeekRemoteWord(tid, address + done, &word)) return false;
        std::memcpy(output + done, &word, sizeof(word));
        for (std::size_t i = 0; i < sizeof(word) && done < capacity; ++i, ++done) {
            if (output[done] == '\0') return true;
        }
    }
    output[capacity - 1] = '\0';
    return true;
}

// ---- H0: Standalone RemoteCall entry point for self-test ----

// Resolve a symbol address in the target process by matching the local
// symbol's file offset in the same loaded module.
// This is the same logic used by the main injector loop.
std::uintptr_t RemoteSymbolByName(pid_t pid, const char* symbol_name) {
    void* local_symbol = dlsym(RTLD_DEFAULT, symbol_name);
    if (local_symbol == nullptr) {
        return 0;
    }
    return RemoteSymbol(pid, local_symbol);
}

// ---- End of H0 helpers ----

#ifndef RC_SELFTEST_CONTROLLER
int main(int argc, char** argv) {

    if (argc != 3 && argc != 4 && argc != 6) {
        std::fprintf(stderr,
                     "usage: %s PID /absolute/library.so\n"
                     "       %s --wait PROCESS_NAME /absolute/library.so\n"
                     "       %s --wait-window PROCESS_NAME REQUIRED_MAPPING "
                     "FORBIDDEN_MAPPING /absolute/library.so\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    pid_t pid = 0;
    const char* library = nullptr;
    bool prefer_workers = false;
    const bool wait_window =
        argc == 6 && std::strcmp(argv[1], "--wait-window") == 0;
    const char* forbidden_mapping = wait_window ? argv[4] : nullptr;
    if (wait_window) {
        library = argv[5];
        prefer_workers = true;
        for (int attempt = 0; attempt < 30000 && pid == 0; ++attempt) {
            const pid_t candidate = FindPidByCmdline(argv[2]);
            if (candidate != 0) {
                if (ProcessHasMapping(candidate, forbidden_mapping)) {
                    std::fprintf(stderr,
                                 "preload window missed: forbidden mapping already present: %s\n",
                                 forbidden_mapping);
                    return 3;
                }
                if (ProcessHasMapping(candidate, "/system/lib64/libc.so") &&
                    ProcessHasMapping(candidate, "/system/lib64/libdl.so") &&
                    ProcessHasMapping(candidate, argv[3])) {
                    pid = candidate;
                    break;
                }
            }
            usleep(1000);
        }
        if (pid == 0) {
            std::fprintf(stderr, "timed out waiting for preload window in %s\n",
                         argv[2]);
            return 1;
        }
        std::printf("found preload window process=%s pid=%d required=%s forbidden=%s\n",
                    argv[2], pid, argv[3], forbidden_mapping);
    } else if (argc == 4 && std::strcmp(argv[1], "--wait") == 0) {
        library = argv[3];
        for (int attempt = 0; attempt < 30000 && pid == 0; ++attempt) {
            const pid_t candidate = FindPidByCmdline(argv[2]);
            if (candidate != 0 && ProcessHasMapping(candidate, "/system/lib64/libc.so") &&
                ProcessHasMapping(candidate, "/system/lib64/libdl.so")) {
                pid = candidate;
                break;
            }
            usleep(1000);
        }
        if (pid == 0) {
            std::fprintf(stderr, "timed out waiting for %s\n", argv[2]);
            return 1;
        }
        std::printf("found process=%s pid=%d\n", argv[2], pid);
    } else if (argc == 3) {
        pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
        library = argv[2];
    } else {
        return 2;
    }
    if (pid <= 0 || library[0] != '/') return 2;

    void* mmap_symbol = dlsym(RTLD_DEFAULT, "mmap");
    void* dlopen_symbol = dlsym(RTLD_DEFAULT, "dlopen");
    void* dlerror_symbol = dlsym(RTLD_DEFAULT, "dlerror");
    void* dlsym_symbol = dlsym(RTLD_DEFAULT, "dlsym");
    void* dlclose_symbol = dlsym(RTLD_DEFAULT, "dlclose");
    void* gettid_symbol = dlsym(RTLD_DEFAULT, "gettid");
    const auto remote_mmap = RemoteSymbol(pid, mmap_symbol);
    const auto remote_dlopen = RemoteSymbol(pid, dlopen_symbol);
    const auto remote_dlerror = RemoteSymbol(pid, dlerror_symbol);
    const auto remote_dlsym = RemoteSymbol(pid, dlsym_symbol);
    const auto remote_dlclose = RemoteSymbol(pid, dlclose_symbol);
    const auto remote_gettid = RemoteSymbol(pid, gettid_symbol);
    if (remote_mmap == 0 || remote_dlopen == 0 || remote_dlerror == 0 ||
        remote_dlsym == 0 || remote_dlclose == 0 || remote_gettid == 0) {
        std::fprintf(stderr,
                     "symbol resolution failed mmap=%p dlopen=%p dlerror=%p "
                     "dlsym=%p dlclose=%p gettid=%p\n",
                     reinterpret_cast<void*>(remote_mmap),
                     reinterpret_cast<void*>(remote_dlopen),
                     reinterpret_cast<void*>(remote_dlerror),
                     reinterpret_cast<void*>(remote_dlsym),
                     reinterpret_cast<void*>(remote_dlclose),
                     reinterpret_cast<void*>(remote_gettid));
        return 1;
    }
    std::fprintf(stderr, "rip bias=%ld (RC_RIP_BIAS; LDPlayer9 needs 2)\n",
                 RcRipBias());

    // Thread-enumerating attach: the game's anti-debug clears DR0-DR7 on the
    // main thread, so the DR execute breakpoint never fires there (the thread
    // runs past saved.rip and SEGV's). Idle worker threads (blocked in
    // syscalls) resume into the libc syscall-return path which contains no
    // DR-clearing code, so the breakpoint fires cleanly. We also waitpid on
    // the specific tid, so another thread's crash stop is never mistaken for
    // our call's result.
    std::vector<pid_t> tids;
    DIR* task_dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (task_dir != nullptr) {
        while (dirent* entry = readdir(task_dir)) {
            char* end = nullptr;
            const long value = std::strtol(entry->d_name, &end, 10);
            if (value > 0 && end != entry->d_name && *end == '\0') {
                tids.push_back(static_cast<pid_t>(value));
            }
        }
        closedir(task_dir);
    }
    if (tids.empty()) tids.push_back(pid);
    // Offline diagnosis only: restrict the candidate set to one tid.
    const char* force_tid_env = std::getenv("RC_FORCE_TID");
    if (force_tid_env != nullptr && *force_tid_env != '\0') {
        const pid_t force_tid =
            static_cast<pid_t>(std::strtol(force_tid_env, nullptr, 10));
        const bool present = std::find(tids.begin(), tids.end(), force_tid) !=
                             tids.end();
        tids.erase(std::remove_if(tids.begin(), tids.end(),
                                  [force_tid](pid_t t) { return t != force_tid; }),
                   tids.end());
        if (!present || force_tid <= 0) tids.clear();
    }
    // Historical late-attach mode keeps its main-thread-first order. The
    // guarded preload-window mode tries worker threads first, because the
    // game's main thread is known to clear debug registers.
    std::stable_sort(tids.begin(), tids.end(), [pid, prefer_workers](pid_t a, pid_t b) {
        const bool a_is_main = a == pid;
        const bool b_is_main = b == pid;
        if (a_is_main == b_is_main) return false;
        return prefer_workers ? !a_is_main : a_is_main;
    });
    if (wait_window) {
        // This ART thread is already present once libnb.so is mapped and is
        // blocked in the runtime's signal wait path. It succeeded in the
        // marker-only handshake, whereas the JIT worker trapped in Houdini.
        // Never probe arbitrary threads in the guarded preload mode.
        tids.erase(std::remove_if(tids.begin(), tids.end(), [pid](pid_t tid) {
                       return ThreadName(pid, tid) != "Signal Catcher";
                   }),
                   tids.end());
        if (tids.empty()) {
            std::fprintf(stderr,
                         "guarded preload aborted: Signal Catcher thread unavailable\n");
            return 4;
        }
    }
    std::fprintf(stderr, "target tids=%zu\n", tids.size());

    // Find an existing 0xCC padding byte in an x86_64 r-x mapping. It is the
    // int3 return stub for every remote call; no target memory is modified.
    const std::uintptr_t int3_stub = FindInt3Stub(pid);
    if (int3_stub == 0) {
        std::fprintf(stderr, "no usable int3 stub in target executable mappings\n");
        return 5;
    }

    std::vector<pid_t> attached_tids;
    bool success = false;
    for (size_t ti = 0; ti < tids.size() && !success; ++ti) {
        const pid_t tid = tids[ti];
        const std::string thread_name = ThreadName(pid, tid);
        std::fprintf(stderr, "candidate tid=%d name=%s\n", tid,
                     thread_name.empty() ? "<unknown>" : thread_name.c_str());
        if (wait_window && ProcessHasMapping(pid, forbidden_mapping)) {
            std::fprintf(stderr,
                         "preload window closed before attach: %s\n",
                         forbidden_mapping);
            break;
        }
        if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1) {
            std::fprintf(stderr, "attach tid=%d failed: %s\n", tid, std::strerror(errno));
            continue;
        }
        int att_status = 0;
        if (waitpid(tid, &att_status, __WALL) != tid || !WIFSTOPPED(att_status)) {
            std::fprintf(stderr, "attach wait tid=%d failed status=0x%x\n", tid, att_status);
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            continue;
        }
        attached_tids.push_back(tid);
        std::fprintf(stderr, "attached tid=%d\n", tid);

        if (wait_window && ProcessHasMapping(pid, forbidden_mapping)) {
            std::fprintf(stderr,
                         "preload window closed after attach: %s\n",
                         forbidden_mapping);
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            attached_tids.pop_back();
            break;
        }

        RemoteCallSession session{};
        if (!RemoteCallSessionInit(tid, int3_stub, &session)) {
            std::fprintf(stderr, "session init failed on tid=%d\n", tid);
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            attached_tids.pop_back();
            if (wait_window) break;
            continue;
        }

        if (!CalibrateRipBias(&session, remote_gettid)) {
            std::fprintf(stderr,
                         "rip bias calibration failed on tid=%d; detaching "
                         "without injection\n",
                         tid);
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            attached_tids.pop_back();
            if (wait_window) break;
            continue;
        }

        const std::uint64_t zero_args[6] = {0, 0, 0, 0, 0, 0};
        std::uint64_t stale_error = 0;
        RemoteCallReport clear_report{};
        const bool clear_ok = RemoteCallSessionCall(
            &session, remote_dlerror, zero_args, &stale_error, &clear_report);
        if (!clear_ok) {
            std::fprintf(stderr,
                         "remote dlerror clear failed on tid=%d result=%s "
                         "status=0x%x\n",
                         tid, RemoteCallResultName(clear_report.result),
                         clear_report.waitpid_status);
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            attached_tids.pop_back();
            if (wait_window) break;
            continue;
        }

        const std::uint64_t mmap_args[6] = {
            0, 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, static_cast<std::uint64_t>(-1), 0
        };
        std::uint64_t scratch = 0;
        RemoteCallReport mmap_report{};
        const bool mmap_ok = RemoteCallSessionCall(
            &session, remote_mmap, mmap_args, &scratch, &mmap_report);
        if (!mmap_ok || scratch == 0 ||
            scratch == static_cast<std::uint64_t>(-1)) {
            std::fprintf(stderr,
                         "remote mmap failed on tid=%d result=%s status=0x%x "
                         "scratch=%p\n",
                         tid, RemoteCallResultName(mmap_report.result),
                         mmap_report.waitpid_status,
                         reinterpret_cast<void*>(scratch));
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            attached_tids.pop_back();
            if (wait_window) break;
            continue;
        }
        if (!WriteRemote(tid, scratch, library, std::strlen(library) + 1)) {
            std::fprintf(stderr, "remote path write failed on tid=%d\n", tid);
            ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
            attached_tids.pop_back();
            if (wait_window) break;
            continue;
        }
        const std::uint64_t dlopen_args[6] = {
            scratch, RTLD_NOW | RTLD_LOCAL, 0, 0, 0, 0
        };
        std::uint64_t handle = 0;
        RemoteCallReport dlopen_report{};
        const bool dlopen_ok = RemoteCallSessionCall(
            &session, remote_dlopen, dlopen_args, &handle, &dlopen_report);

        std::uint64_t error_ptr = 0;
        RemoteCallReport error_report{};
        const bool error_ok = RemoteCallSessionCall(
            &session, remote_dlerror, zero_args, &error_ptr, &error_report);

        std::printf("pid=%d tid=%d mmap=%p dlopen=%p dlerror=%p scratch=%p "
                    "handle=%p dlopen_result=%s error_ptr=%p\n",
                    pid, tid, reinterpret_cast<void*>(remote_mmap),
                    reinterpret_cast<void*>(remote_dlopen),
                    reinterpret_cast<void*>(remote_dlerror),
                    reinterpret_cast<void*>(scratch),
                    reinterpret_cast<void*>(handle),
                    RemoteCallResultName(dlopen_report.result),
                    reinterpret_cast<void*>(error_ptr));

        if (dlopen_ok && handle != 0) {
            // Layer-1 gate: prove the bootstrap constructor ran by remote
            // dlsym + calling a9tas_bootstrap_status/stage. Any failure here
            // must dlclose the fresh handle and report, not silently proceed.
            constexpr std::uintptr_t kStatusNameOff = 0x200;
            constexpr std::uintptr_t kStageNameOff = 0x300;
            const char kStatusName[] = "a9tas_bootstrap_status";
            const char kStageName[] = "a9tas_bootstrap_stage";
            bool verified =
                WriteRemote(tid, scratch + kStatusNameOff, kStatusName,
                            std::strlen(kStatusName) + 1) &&
                WriteRemote(tid, scratch + kStageNameOff, kStageName,
                            std::strlen(kStageName) + 1);

            std::uint64_t status_fn = 0;
            RemoteCallReport dlsym_status_report{};
            if (verified) {
                const std::uint64_t dlsym_status_args[6] = {
                    handle, scratch + kStatusNameOff, 0, 0, 0, 0
                };
                verified = RemoteCallSessionCall(
                    &session, remote_dlsym, dlsym_status_args, &status_fn,
                    &dlsym_status_report);
            }

            std::uint64_t status = 0;
            RemoteCallReport status_report{};
            if (verified && status_fn != 0) {
                verified = RemoteCallSessionCall(
                    &session, static_cast<std::uintptr_t>(status_fn),
                    zero_args, &status, &status_report);
                verified = verified && status == 1;
            } else {
                verified = false;
            }

            std::uint64_t stage_fn = 0;
            RemoteCallReport dlsym_stage_report{};
            if (verified) {
                const std::uint64_t dlsym_stage_args[6] = {
                    handle, scratch + kStageNameOff, 0, 0, 0, 0
                };
                verified = RemoteCallSessionCall(
                    &session, remote_dlsym, dlsym_stage_args, &stage_fn,
                    &dlsym_stage_report);
            }

            std::uint64_t stage = 0;
            RemoteCallReport stage_report{};
            if (verified && stage_fn != 0) {
                verified = RemoteCallSessionCall(
                    &session, static_cast<std::uintptr_t>(stage_fn),
                    zero_args, &stage, &stage_report);
            } else {
                verified = false;
            }

            std::printf("pid=%d tid=%d bootstrap_verified=%d status=%llu "
                        "stage=%lld\n", pid, tid, verified ? 1 : 0,
                        static_cast<unsigned long long>(status),
                        static_cast<long long>(stage));
            if (verified) {
                success = true;
            } else {
                std::fprintf(stderr,
                             "bootstrap verification failed tid=%d "
                             "status_fn=%p stage_fn=%p status=%llu stage=%lld "
                             "dlsym_status_result=%s dlsym_stage_result=%s\n",
                             tid, reinterpret_cast<void*>(status_fn),
                             reinterpret_cast<void*>(stage_fn),
                             static_cast<unsigned long long>(status),
                             static_cast<long long>(stage),
                             RemoteCallResultName(dlsym_status_report.result),
                             RemoteCallResultName(dlsym_stage_report.result));
                const std::uint64_t dlclose_args[6] = {handle, 0, 0, 0, 0, 0};
                std::uint64_t closed = 0;
                RemoteCallReport dlclose_report{};
                const bool closed_ok = RemoteCallSessionCall(
                    &session, remote_dlclose, dlclose_args, &closed,
                    &dlclose_report);
                std::fprintf(stderr,
                             "best-effort dlclose tid=%d ok=%d rc=%llu\n",
                             tid, closed_ok ? 1 : 0,
                             static_cast<unsigned long long>(closed));
            }
        } else {
            char error_text[256]{};
            if (error_ok && error_ptr != 0 &&
                ReadRemoteString(tid, static_cast<std::uintptr_t>(error_ptr),
                                 error_text, sizeof(error_text))) {
                std::fprintf(stderr, "remote dlopen failed on tid=%d: %s\n",
                             tid, error_text);
            } else {
                std::fprintf(stderr,
                             "remote dlopen failed on tid=%d result=%s "
                             "status=0x%x dlerror_ok=%d\n",
                             tid, RemoteCallResultName(dlopen_report.result),
                             dlopen_report.waitpid_status, error_ok ? 1 : 0);
            }
        }
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        attached_tids.pop_back();
    }
    if (!success) std::fprintf(stderr, "remote injection did not complete\n");

    for (const pid_t tid : attached_tids) {
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    }
    return success ? 0 : 1;
}
#endif  // RC_SELFTEST_CONTROLLER
