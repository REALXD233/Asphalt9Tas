#define RC_SELFTEST_CONTROLLER

#include "src/injector.cpp"

#include <elf.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TargetResolution {
    std::string path;
    std::uintptr_t base{};
    bool is_dyn{};
};

struct TestSpec {
    const char* name;
    const char* symbol;
    std::uint64_t args[6];
    CallResult expected;
    bool expect_result_value;
    std::uint64_t expected_value;
};

struct TestOutcome {
    std::string name;
    bool passed{};
    bool executed{};
    bool rc_ok{};
    RemoteCallReport report{};
    std::uint64_t result{};
    std::string note;
    // Cleanup three-state: not_applicable / succeeded / failed
    enum class CleanupState : std::uint8_t {
        kNotApplicable = 0,
        kSucceeded,
        kFailed,
    };
    CleanupState cleanup_state{CleanupState::kNotApplicable};
    bool detach_attempted{};
    bool detach_succeeded{};
    bool post_detach_thread_alive{};
    bool post_detach_process_alive{};
    bool forced_target_kill{};
    std::uint64_t stress_first_failure_iter{0};  // 0 = no failure
};

static void SetThreadName(const char* name) {
#if defined(__linux__) && defined(PR_SET_NAME)
    prctl(PR_SET_NAME, name, 0, 0, 0);
#else
    (void)name;
#endif
}

static bool ResolveElfSymbol(const std::string& path, const char* symbol,
                             std::uint64_t* value, bool* is_dyn) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    Elf64_Ehdr ehdr{};
    if (std::fread(&ehdr, sizeof(ehdr), 1, file) != 1 ||
        std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_machine != EM_X86_64 ||
        ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
        std::fclose(file);
        return false;
    }

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    if (std::fseek(file, static_cast<long>(ehdr.e_shoff), SEEK_SET) != 0 ||
        std::fread(shdrs.data(), sizeof(Elf64_Shdr), shdrs.size(), file) != shdrs.size()) {
        std::fclose(file);
        return false;
    }

    if (ehdr.e_shstrndx >= shdrs.size()) {
        std::fclose(file);
        return false;
    }

    const Elf64_Shdr& shstr = shdrs[ehdr.e_shstrndx];
    std::vector<char> shstrtab(shstr.sh_size);
    if (std::fseek(file, static_cast<long>(shstr.sh_offset), SEEK_SET) != 0 ||
        (shstr.sh_size != 0 && std::fread(shstrtab.data(), 1, shstr.sh_size, file) != shstr.sh_size)) {
        std::fclose(file);
        return false;
    }

    for (std::size_t si = 0; si < shdrs.size(); ++si) {
        const Elf64_Shdr& sec = shdrs[si];
        if (sec.sh_type != SHT_SYMTAB && sec.sh_type != SHT_DYNSYM) continue;
        if (sec.sh_link >= shdrs.size()) continue;
        const Elf64_Shdr& strsec = shdrs[sec.sh_link];
        if (strsec.sh_type != SHT_STRTAB) continue;

        std::vector<char> strtab(strsec.sh_size);
        if (std::fseek(file, static_cast<long>(strsec.sh_offset), SEEK_SET) != 0 ||
            (strsec.sh_size != 0 && std::fread(strtab.data(), 1, strsec.sh_size, file) != strsec.sh_size)) {
            continue;
        }

        if (sec.sh_entsize == 0) continue;
        const std::size_t sym_count = static_cast<std::size_t>(sec.sh_size / sec.sh_entsize);
        if (std::fseek(file, static_cast<long>(sec.sh_offset), SEEK_SET) != 0) {
            continue;
        }
        for (std::size_t i = 0; i < sym_count; ++i) {
            Elf64_Sym sym{};
            if (std::fread(&sym, sizeof(sym), 1, file) != 1) break;
            if (sym.st_name >= strtab.size()) continue;
            const char* name = strtab.data() + sym.st_name;
            if (std::strcmp(name, symbol) == 0) {
                *value = sym.st_value;
                *is_dyn = (ehdr.e_type == ET_DYN);
                std::fclose(file);
                return true;
            }
        }
    }

    std::fclose(file);
    return false;
}

static bool FindTargetMapping(pid_t pid, const std::string& needle,
                              TargetResolution* out) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, path);
        if (fields == 5 && offset == 0) {
            std::string candidate = path;
            while (!candidate.empty() && candidate.front() == ' ') candidate.erase(0, 1);
            if (candidate.find(needle) != std::string::npos) {
                out->path = candidate;
                out->base = static_cast<std::uintptr_t>(start);
                out->is_dyn = true;
                return true;
            }
        }
    }
    return false;
}

static bool WaitForExecMapping(pid_t pid, const std::string& needle,
                               TargetResolution* out) {
    for (int attempt = 0; attempt < 5000; ++attempt) {
        if (FindTargetMapping(pid, needle, out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static pid_t SpawnTarget(const std::string& target_path) {
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        SetThreadName("rc-selftest-spawn");
        execl(target_path.c_str(), target_path.c_str(), "--serve", nullptr);
        std::perror("execl");
        _exit(127);
    }
    return pid;
}

static std::vector<pid_t> ListThreads(pid_t pid) {
    std::vector<pid_t> tids;
    DIR* dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (dir == nullptr) return tids;
    while (dirent* entry = readdir(dir)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (value > 0 && end != entry->d_name && *end == '\0') {
            tids.push_back(static_cast<pid_t>(value));
        }
    }
    closedir(dir);
    std::sort(tids.begin(), tids.end());
    return tids;
}

static std::optional<pid_t> ChooseCallThread(pid_t pid) {
    // RC_THREAD=spin  -> spinner (busy user-space loop)
    // RC_THREAD=worker -> worker (blocked in nanosleep, like a game thread)
    const char* preference = std::getenv("RC_THREAD");
    const std::string prefer = preference ? preference : "spin";

    // The worker thread is created asynchronously right after exec. Wait a
    // short bounded time for it instead of falling back to the main thread;
    // earlier H0 runs showed main-thread RemoteCall was less reliable.
    for (int attempt = 0; attempt < 200; ++attempt) {
        const auto tids = ListThreads(pid);
        if (!tids.empty()) {
            if (prefer == "worker") {
                for (pid_t tid : tids) {
                    if (ThreadName(pid, tid).find("rc-selftest-wrk") !=
                        std::string::npos) {
                        return tid;
                    }
                }
                // Do not fall back to the spinner: waiting for the worker
                // is the whole point of RC_THREAD=worker.
            } else {
                for (pid_t tid : tids) {
                    if (ThreadName(pid, tid).find("rc-selftest-spin") !=
                        std::string::npos) {
                        return tid;
                    }
                }
                for (pid_t tid : tids) {
                    if (ThreadName(pid, tid).find("rc-selftest-wrk") !=
                        std::string::npos) {
                        return tid;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto tids = ListThreads(pid);
    if (!tids.empty()) return tids.front();
    return std::nullopt;
}

static std::uintptr_t ResolveRemoteFunction(pid_t pid, const std::string& exe_path,
                                            const char* symbol) {
    std::uint64_t value = 0;
    bool is_dyn = false;
    if (!ResolveElfSymbol(exe_path, symbol, &value, &is_dyn)) {
        return 0;
    }
    if (is_dyn) {
        TargetResolution mapping{};
        if (!FindTargetMapping(pid, fs::path(exe_path).filename().string(), &mapping)) {
            return 0;
        }
        return mapping.base + static_cast<std::uintptr_t>(value);
    }
    return static_cast<std::uintptr_t>(value);
}

static const char* CallResultName(CallResult r) {
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

static const char* StopReasonName(StopReason r) {
    switch (r) {
        case StopReason::kUnknown: return "unknown";
        case StopReason::kSIGTRAP: return "sigtrap";
        case StopReason::kSIGSEGV: return "sigsegv";
        case StopReason::kSIGBUS: return "sigbus";
        case StopReason::kSIGILL: return "sigill";
        case StopReason::kSIGSYS: return "sigsys";
        case StopReason::kSIGSTOP: return "sigstop";
        case StopReason::kThreadExit: return "thread_exit";
        case StopReason::kProcessExit: return "process_exit";
        case StopReason::kPtraceEvent: return "ptrace_event";
        case StopReason::kGroupStop: return "group_stop";
        case StopReason::kTimeout: return "timeout";
        case StopReason::kWaitError: return "wait_error";
        case StopReason::kSpuriousWakeup: return "spurious_wakeup";
        default: return "unknown";
    }
}

static bool DetachThread(pid_t tid) {
    if (tid <= 0) return true;
    if (ptrace(PTRACE_DETACH, tid, nullptr, nullptr) == -1) {
        if (errno == ESRCH) return true;
        return false;
    }
    return true;
}

// Bounded child reaping: send SIGKILL then poll for up to 5 seconds.
// If the child doesn't exit (e.g. stuck in ptrace stop), log and give up
// rather than blocking forever.
static void ReapChild(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    for (int i = 0; i < 50; ++i) {  // 50 x 100ms = 5s max
        int status = 0;
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) return;
        if (w == -1 && errno == ECHILD) return;
        // w == 0: still running, or stopped by ptrace
        // Try detaching in case it's in a ptrace stop
        (void)ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        usleep(100000);  // 100ms
    }
    std::fprintf(stderr, "ReapChild: pid=%d did not exit after 5s\n", pid);
}

// Prepare a software-breakpoint return stub by temporarily patching the
// first byte of SelftestEcho (unused by every TestSpec) with 0xCC. The
// original 8-byte word is returned in *saved_word and must be restored with
// RestoreInt3Stub before detach.
static bool PrepareInt3Stub(pid_t pid, pid_t tid,
                            const std::string& exe_path,
                            std::uintptr_t* stub, long* saved_word) {
    if (stub == nullptr || saved_word == nullptr) return false;
    const std::uintptr_t echo = ResolveRemoteFunction(pid, exe_path,
                                                      "SelftestEcho");
    if (echo == 0) {
        std::fprintf(stderr, "int3 stub: SelftestEcho unresolved\n");
        return false;
    }
    long original = 0;
    if (!PeekRemoteWord(tid, echo, &original)) {
        std::fprintf(stderr, "int3 stub: peek echo failed\n");
        return false;
    }
    const std::uint8_t int3 = 0xCC;
    if (!WriteRemote(tid, echo, &int3, sizeof(int3))) {
        std::fprintf(stderr, "int3 stub: patch echo failed\n");
        return false;
    }
    *saved_word = original;
    *stub = echo;
    return true;
}

static bool RestoreInt3Stub(pid_t tid, std::uintptr_t stub, long saved_word) {
    if (stub == 0) return false;
    return WriteRemote(tid, stub, &saved_word, sizeof(saved_word));
}

static TestOutcome RunOneTest(pid_t pid, const std::string& exe_path,
                              const TestSpec& spec) {
    TestOutcome outcome{};
    outcome.name = spec.name;
    outcome.executed = false;

    const auto maybe_tid = ChooseCallThread(pid);
    if (!maybe_tid.has_value()) {
        outcome.note = "no call thread";
        return outcome;
    }
    const pid_t tid = *maybe_tid;
    const pid_t tgid = ThreadGroupIdOfTid(tid);

    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1) {
        outcome.note = std::string("attach failed: ") + std::strerror(errno);
        return outcome;
    }
    int attach_status = 0;
    if (waitpid(tid, &attach_status, __WALL) != tid || !WIFSTOPPED(attach_status)) {
        outcome.note = "attach wait failed";
        (void)ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return outcome;
    }

    const std::uintptr_t function = ResolveRemoteFunction(pid, exe_path, spec.symbol);
    if (function == 0) {
        outcome.note = std::string("resolve failed: ") + spec.symbol;
        outcome.report.result = CallResult::kInternalError;
        (void)DetachThread(tid);
        return outcome;
    }
    std::fprintf(stderr, "spec=%s resolved_function=0x%" PRIxPTR "\n",
                 spec.name, static_cast<std::uintptr_t>(function));

    std::uintptr_t int3_stub = 0;
    long int3_saved_word = 0;
    if (!PrepareInt3Stub(pid, tid, exe_path, &int3_stub, &int3_saved_word)) {
        outcome.note = "prepare int3 stub failed";
        outcome.report.result = CallResult::kInternalError;
        (void)DetachThread(tid);
        return outcome;
    }
    std::fprintf(stderr, "spec=%s int3_stub=0x%" PRIxPTR "\n",
                 spec.name, int3_stub);

    std::uint64_t result = 0;
    RemoteCallReport report{};
    outcome.executed = true;
    const char* mode_env = std::getenv("RC_CALL_MODE");
    const bool use_one_stage =
        mode_env != nullptr && std::strcmp(mode_env, "onestage") == 0;
    const bool rc_ok = use_one_stage
        ? RemoteCallInt3(tid, function, spec.args, int3_stub, &result, &report)
        : RemoteCallInt3TwoStage(tid, function, spec.args, int3_stub,
                                 &result, &report);
    outcome.rc_ok = rc_ok;
    outcome.result = result;
    outcome.report = report;
    const bool stub_restored =
        RestoreInt3Stub(tid, int3_stub, int3_saved_word);
    if (!stub_restored && report.stop_confirmed) {
        outcome.note = std::string(outcome.note).empty()
            ? "int3 stub restore failed"
            : outcome.note + "; int3 stub restore failed";
    }

    // --- Cleanup decision: based on ACTUAL state, not expected result ---
    // If the tracee is stopped and detach_safe, we detach.
    // If the tracee is stopped but NOT detach_safe, cleanup failed.
    // If the tracee has exited (thread or process), detach is not applicable.
    const bool thread_alive = IsPidTrulyAlive(tid);
    const bool process_alive = (tgid > 0) ? IsPidTrulyAlive(tgid) : thread_alive;

    if (report.stop_confirmed && report.detach_safe) {
        outcome.detach_attempted = true;
        const bool detached = DetachThread(tid);
        outcome.detach_succeeded = detached;
        outcome.cleanup_state = detached
            ? TestOutcome::CleanupState::kSucceeded
            : TestOutcome::CleanupState::kFailed;
    } else if (report.stop_confirmed && !report.detach_safe) {
        // Stopped but rollback failed or state not safe.
        outcome.detach_attempted = false;
        outcome.detach_succeeded = false;
        outcome.cleanup_state = TestOutcome::CleanupState::kFailed;
    } else if (!thread_alive && process_alive) {
        // Thread exited but process still alive.
        outcome.detach_attempted = false;
        outcome.detach_succeeded = false;
        outcome.cleanup_state = TestOutcome::CleanupState::kNotApplicable;
    } else if (!thread_alive && !process_alive) {
        // Process exited.
        outcome.detach_attempted = false;
        outcome.detach_succeeded = false;
        outcome.cleanup_state = TestOutcome::CleanupState::kNotApplicable;
    } else {
        // Thread alive but not stopped: unsafe to detach.
        outcome.detach_attempted = false;
        outcome.detach_succeeded = false;
        outcome.cleanup_state = TestOutcome::CleanupState::kFailed;
    }

    // --- Post-detach liveness (using zombie-aware check) ---
    outcome.post_detach_thread_alive = IsPidTrulyAlive(tid);
    if (tgid > 0) {
        outcome.post_detach_process_alive = IsPidTrulyAlive(tgid);
    } else {
        outcome.post_detach_process_alive = outcome.post_detach_thread_alive;
    }

    // --- Rollback hard gate ---
    // For any scenario where stop was confirmed, rollback must have been
    // attempted AND succeeded. "Not attempted" is NOT ok.
    const bool rollback_ok = report.stop_confirmed
        ? (report.rollback_attempted && report.rollback_succeeded)
        : true;  // If not stopped, rollback is N/A.

    // --- Final PASS evaluation ---
    const char* pass_mode_env = std::getenv("RC_CALL_MODE");
    const bool int3_one_stage =
        pass_mode_env != nullptr && std::strcmp(pass_mode_env, "onestage") == 0;
    const bool result_match = (spec.expected == report.result);
    const bool result_value_ok = (!spec.expect_result_value || result == spec.expected_value);
    bool pass = result_match && result_value_ok;

    // The int3 one-stage path never touches debug registers, so requiring
    // debug_registers_valid there is meaningless. Everything else (register
    // readback, verified stack word, rollback, safe detach, process alive)
    // is still mandatory.
    const bool diagnostics_ok = report.registers_valid &&
                                report.stack_word_valid &&
                                (int3_one_stage || report.debug_registers_valid);

    if (spec.expected == CallResult::kSuccess) {
        pass = pass && rc_ok && report.stop_confirmed && rollback_ok
               && outcome.cleanup_state == TestOutcome::CleanupState::kSucceeded
               && outcome.post_detach_process_alive;
    } else if (spec.expected == CallResult::kTimeout) {
        if (report.stop_confirmed) {
            // Timeout-recovered stop: must have diagnostics, rollback, detach.
            pass = pass && diagnostics_ok && rollback_ok
                   && outcome.cleanup_state == TestOutcome::CleanupState::kSucceeded
                   && outcome.post_detach_process_alive;
        } else {
            // Could not confirm stop: unsafe recovery, never PASS.
            pass = false;
        }
    } else if (spec.expected == CallResult::kThreadExited) {
        pass = pass && !outcome.post_detach_thread_alive
               && outcome.post_detach_process_alive;
    } else if (spec.expected == CallResult::kProcessExited) {
        pass = pass && !outcome.post_detach_thread_alive
               && !outcome.post_detach_process_alive;
    } else {
        // Signal / wrong-trap: must have stop, diagnostics, rollback, detach.
        pass = pass && report.stop_confirmed
               && diagnostics_ok
               && rollback_ok
               && outcome.cleanup_state == TestOutcome::CleanupState::kSucceeded
               && outcome.post_detach_process_alive;
    }

    outcome.passed = pass;
    return outcome;
}

static void PrintOutcome(const TestOutcome& out) {
    const char* cleanup_name = "not_applicable";
    switch (out.cleanup_state) {
        case TestOutcome::CleanupState::kNotApplicable: cleanup_name = "not_applicable"; break;
        case TestOutcome::CleanupState::kSucceeded: cleanup_name = "succeeded"; break;
        case TestOutcome::CleanupState::kFailed: cleanup_name = "failed"; break;
    }
    std::printf(
        "TEST name=%s executed=%d pass=%d result=%s stop=%s rc_ok=%d waitpid=%d status=0x%x "
        "rax=0x%llx rip=0x%llx rsp=0x%llx rdi=0x%llx rsi=0x%llx rdx=0x%llx rcx=0x%llx r8=0x%llx r9=0x%llx "
        "dr0=0x%llx dr6=0x%llx dr7=0x%llx "
        "regs_valid=%d dr_valid=%d stack_valid=%d stop_confirmed=%d "
        "rollback_attempted=%d rollback_succeeded=%d detach_safe=%d "
        "pre_alive_tid=%d pre_alive_pid=%d saved_tgid=%d "
        "detach_attempted=%d detach_succeeded=%d cleanup=%s "
        "post_alive_tid=%d post_alive_pid=%d forced_kill=%d "
        "stress_first_fail_iter=%llu elapsed_ns=%llu note=%s\n",
        out.name.c_str(), out.executed ? 1 : 0, out.passed ? 1 : 0,
        CallResultName(out.report.result), StopReasonName(out.report.stop_reason),
        out.rc_ok ? 1 : 0, out.report.waitpid_result, out.report.waitpid_status,
        static_cast<unsigned long long>(out.report.rax),
        static_cast<unsigned long long>(out.report.rip),
        static_cast<unsigned long long>(out.report.rsp),
        static_cast<unsigned long long>(out.report.rdi),
        static_cast<unsigned long long>(out.report.rsi),
        static_cast<unsigned long long>(out.report.rdx),
        static_cast<unsigned long long>(out.report.rcx),
        static_cast<unsigned long long>(out.report.r8),
        static_cast<unsigned long long>(out.report.r9),
        static_cast<unsigned long long>(out.report.dr0),
        static_cast<unsigned long long>(out.report.dr6),
        static_cast<unsigned long long>(out.report.dr7),
        out.report.registers_valid ? 1 : 0,
        out.report.debug_registers_valid ? 1 : 0,
        out.report.stack_word_valid ? 1 : 0,
        out.report.stop_confirmed ? 1 : 0,
        out.report.rollback_attempted ? 1 : 0,
        out.report.rollback_succeeded ? 1 : 0,
        out.report.detach_safe ? 1 : 0,
        out.report.pre_detach_target_thread_alive ? 1 : 0,
        out.report.pre_detach_target_alive ? 1 : 0,
        static_cast<int>(out.report.saved_tgid),
        out.detach_attempted ? 1 : 0,
        out.detach_succeeded ? 1 : 0,
        cleanup_name,
        out.post_detach_thread_alive ? 1 : 0,
        out.post_detach_process_alive ? 1 : 0,
        out.forced_target_kill ? 1 : 0,
        static_cast<unsigned long long>(out.stress_first_failure_iter),
        static_cast<unsigned long long>(out.report.elapsed_ns),
        out.note.c_str());
}

static std::vector<TestSpec> BuildSpecs() {
    return {
        {"ok", "SelftestOk", {1, 2, 3, 0, 0, 0}, CallResult::kSuccess, true, 13},
        {"segv", "SelftestSegv", {0, 0, 0, 0, 0, 0}, CallResult::kTargetSegv, false, 0},
        {"ill", "SelftestIll", {0, 0, 0, 0, 0, 0}, CallResult::kTargetIll, false, 0},
        {"wrong trap", "SelftestTrap", {0, 0, 0, 0, 0, 0}, CallResult::kWrongTrap, false, 0},
        {"exit-thread", "SelftestExitThread", {0, 0, 0, 0, 0, 0}, CallResult::kThreadExited, false, 0},
        {"exit-process", "SelftestExitProcess", {42, 0, 0, 0, 0, 0}, CallResult::kProcessExited, false, 0},
        {"hang", "SelftestHang", {0, 0, 0, 0, 0, 0}, CallResult::kTimeout, false, 0},
        {"stress", "SelftestStress", {0, 0, 0, 0, 0, 0}, CallResult::kSuccess, false, 0},
        {"libc-like", "SelftestLibcLike", {1, 2, 3, 0, 0, 0},
         CallResult::kSuccess, true, UINT64_C(0x9E3779B97F4A5025)},
    };
}

static int RunAll(const std::string& target_path) {
    const auto specs = BuildSpecs();
    std::vector<TestOutcome> outcomes;
    outcomes.reserve(specs.size());

    for (const auto& spec : specs) {
        const pid_t pid = SpawnTarget(target_path);
        if (pid <= 0) {
            TestOutcome out{};
            out.name = spec.name;
            out.note = "spawn failed";
            outcomes.push_back(out);
            continue;
        }

        TargetResolution res{};
        if (!WaitForExecMapping(pid, fs::path(target_path).filename().string(), &res)) {
            TestOutcome out{};
            out.name = spec.name;
            out.note = "target mapping not ready";
            ReapChild(pid);
            outcomes.push_back(out);
            continue;
        }

        if (std::strcmp(spec.name, "stress") == 0) {
            TestOutcome stress_out{};
            stress_out.name = spec.name;
            stress_out.executed = true;
            const auto maybe_tid = ChooseCallThread(pid);
            if (!maybe_tid.has_value()) {
                stress_out.note = "no call thread";
                outcomes.push_back(stress_out);
                ReapChild(pid);
                continue;
            }
            const pid_t tid = *maybe_tid;
            if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1) {
                stress_out.note = std::string("attach failed: ") + std::strerror(errno);
                outcomes.push_back(stress_out);
                ReapChild(pid);
                continue;
            }
            int st = 0;
            if (waitpid(tid, &st, __WALL) != tid || !WIFSTOPPED(st)) {
                stress_out.note = "attach wait failed";
                (void)ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
                outcomes.push_back(stress_out);
                ReapChild(pid);
                continue;
            }
            const std::uintptr_t fn = ResolveRemoteFunction(pid, target_path, spec.symbol);
            std::uintptr_t int3_stub = 0;
            long int3_saved_word = 0;
            if (!PrepareInt3Stub(pid, tid, target_path, &int3_stub,
                                 &int3_saved_word)) {
                stress_out.note = "prepare int3 stub failed";
                outcomes.push_back(stress_out);
                ReapChild(pid);
                continue;
            }
            const pid_t stress_tgid = ThreadGroupIdOfTid(tid);
            std::uint64_t sum = 0;
            bool all_ok = true;
            std::array<std::uint64_t, 6> zero_args{0, 0, 0, 0, 0, 0};
            RemoteCallReport last_report{};
            std::uint64_t first_failure_iter = 0;
            const char* mode_env = std::getenv("RC_CALL_MODE");
            const bool use_one_stage =
                mode_env != nullptr && std::strcmp(mode_env, "onestage") == 0;
            for (std::uint64_t i = 0; i < 1000; ++i) {
                std::uint64_t result = 0;
                RemoteCallReport report{};
                const bool rc_ok = use_one_stage
                    ? RemoteCallInt3(tid, fn, zero_args.data(), int3_stub,
                                     &result, &report)
                    : RemoteCallInt3TwoStage(tid, fn, zero_args.data(),
                                             int3_stub, &result, &report);
                last_report = report;
                const bool iter_ok = rc_ok
                    && report.result == CallResult::kSuccess
                    && report.stop_confirmed
                    && report.registers_valid
                    && report.rollback_succeeded
                    && report.detach_safe;
                if (!iter_ok) {
                    all_ok = false;
                    first_failure_iter = i + 1;
                    stress_out.report = report;
                    break;
                }
                sum += result;
            }
            // Save last report even on success (H0-BLOCKER-2 fix).
            if (all_ok) {
                stress_out.report = last_report;
            }
            // On failure, stress_out.report was already set to the failing
            // iteration's report inside the loop.
            stress_out.stress_first_failure_iter = first_failure_iter;
            stress_out.result = sum;
            (void)RestoreInt3Stub(tid, int3_stub, int3_saved_word);

            // Detach must use the most recent report (the one that reflects
            // the current state of the tracee). On success, that's last_report.
            // On failure, that's the failing iteration's report (already in
            // stress_out.report).
            const RemoteCallReport& detach_report = all_ok ? last_report : stress_out.report;
            if (detach_report.detach_safe) {
                stress_out.detach_attempted = true;
                const bool detached = DetachThread(tid);
                stress_out.detach_succeeded = detached;
                stress_out.cleanup_state = detached
                    ? TestOutcome::CleanupState::kSucceeded
                    : TestOutcome::CleanupState::kFailed;
            } else {
                stress_out.cleanup_state = TestOutcome::CleanupState::kFailed;
            }

            // Post-detach liveness (zombie-aware).
            stress_out.post_detach_thread_alive = IsPidTrulyAlive(tid);
            if (stress_tgid > 0) {
                stress_out.post_detach_process_alive = IsPidTrulyAlive(stress_tgid);
            } else {
                stress_out.post_detach_process_alive = stress_out.post_detach_thread_alive;
            }

            stress_out.passed = all_ok
                && stress_out.cleanup_state == TestOutcome::CleanupState::kSucceeded
                && stress_out.post_detach_process_alive;
            // Harness teardown: force kill and reap child.
            stress_out.forced_target_kill = true;
            outcomes.push_back(stress_out);
            PrintOutcome(stress_out);
            ReapChild(pid);
            continue;
        }

        TestOutcome out = RunOneTest(pid, target_path, spec);
        // Harness teardown: force kill and reap child.
        out.forced_target_kill = true;
        outcomes.push_back(out);
        PrintOutcome(out);
        ReapChild(pid);
    }

    const bool all_pass = std::all_of(outcomes.begin(), outcomes.end(), [](const TestOutcome& o) {
        return o.passed;
    });
    std::printf("SUMMARY pass=%d count=%zu\n", all_pass ? 1 : 0, outcomes.size());
    return all_pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /absolute/path/to/remote_call_selftest_target\n", argv[0]);
        return 2;
    }
    SetThreadName("h0-rc-controller");
    const std::string target_path = argv[1];
    if (!fs::path(target_path).is_absolute()) {
        std::fprintf(stderr, "target path must be absolute\n");
        return 2;
    }
    return RunAll(target_path);
}
