// Observation-only thread-affinity probe for CarPhysicsState direct_mode.
//
// DR0 watches command_owner+0x1378 as write/byte on every game thread. The
// probe never calls a game function and never writes guest memory.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include <fstream>

namespace {

constexpr std::uintptr_t kDirectModeOffset = 0x1378;
constexpr unsigned long kDirectModeDr7 = 1UL | (1UL << 16);

bool WritableByte(const std::vector<Mapping>& maps, std::uintptr_t address) {
    const Mapping* mapping = FindMapping(maps, address, 1);
    return mapping != nullptr && mapping->perms[0] == 'r' &&
           mapping->perms[1] == 'w';
}

std::string CurrentThreadName(pid_t pid, pid_t tid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/task/" +
                             std::to_string(tid) + "/comm";
    std::ifstream input(path);
    std::string name;
    std::getline(input, name);
    return name;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s PID OWNER_HEX DURATION_MS\n", argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, owner_value = 0, duration_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &owner_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) || pid_value == 0 ||
        owner_value == 0 || duration_value < 1000 || duration_value > 30000) {
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto owner = static_cast<std::uintptr_t>(owner_value);
    const auto direct = owner + kDirectModeOffset;
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps) || !WritableByte(maps, direct)) {
        std::fprintf(stderr, "direct_mode address is not writable\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;
    std::uint8_t initial = 0xff;
    if (!ReadExact(mem, direct, &initial, sizeof(initial)) || initial > 1) {
        close(mem);
        return 3;
    }

    std::vector<TracedThread> threads;
    std::uint64_t ptrace_errors = 0;
    std::uint64_t unexpected_stops = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t events = 0;
    std::uint64_t to_one = 0;
    std::uint64_t to_zero = 0;
    std::uint64_t thread_additions = 0;
    std::uint64_t exited_threads = 0;
    const std::size_t initial_threads = AttachNewThreads(
        pid, direct, 0, 0, 0, &threads, &ptrace_errors, kDirectModeDr7);
    if (initial_threads == 0 || ptrace_errors != 0) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        return 5;
    }
    const std::uint64_t started = MonotonicNs();
    const std::uint64_t deadline = started + duration_value * 1000000ULL;
    std::uint64_t next_rescan = started + 250000000ULL;
    std::printf(
        "DIRECT_MODE_AFFINITY_V1_ARMED pid=%d owner=0x%" PRIxPTR
        " direct=0x%" PRIxPTR " initial=%u threads=%zu duration_ms=%" PRIu64
        " write_scope=debug-registers-only activations=0\n",
        pid, owner, direct, static_cast<unsigned>(initial), threads.size(),
        duration_value);
    std::fflush(stdout);

    while (MonotonicNs() < deadline) {
        const std::uint64_t now = MonotonicNs();
        if (now >= next_rescan) {
            std::uint64_t failures = 0;
            thread_additions += AttachNewThreads(
                pid, direct, 0, 0, 0, &threads, &failures, kDirectModeDr7);
            ptrace_errors += failures;
            next_rescan = now + 250000000ULL;
        }
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (tid < 0) {
            if (errno != EINTR && errno != ECHILD) ++ptrace_errors;
            continue;
        }
        TracedThread* tracked = FindThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked) {
                tracked->live = false;
                tracked->stopped = false;
                ++exited_threads;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        if (signal == SIGTRAP && PeekDebug(tid, 6, &dr6) &&
            (dr6 & 1UL) != 0) {
            std::uint8_t value = 0xff;
            user_regs_struct regs{};
            const bool read_ok =
                ReadExact(mem, direct, &value, sizeof(value)) && value <= 1;
            const bool regs_ok =
                ptrace(PTRACE_GETREGS, tid, nullptr, &regs) != -1;
            if (!read_ok) ++read_errors;
            if (!regs_ok) ++ptrace_errors;
            if (read_ok && value == 1) ++to_one;
            if (read_ok && value == 0) ++to_zero;
            ++events;
            const std::string name = CurrentThreadName(pid, tid);
            std::printf(
                "DIRECT_MODE_EVENT seq=%" PRIu64 " ns=%" PRIu64
                " tid=%d name=%s value=%u rip=0x%llx read_ok=%d regs_ok=%d\n",
                events - 1, MonotonicNs() - started, tid,
                name.empty() ? "<unknown>" : name.c_str(),
                static_cast<unsigned>(value),
                static_cast<unsigned long long>(regs.rip), read_ok ? 1 : 0,
                regs_ok ? 1 : 0);
            std::fflush(stdout);
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            ++unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

    std::uint64_t detached = 0;
    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped))
            ++ptrace_errors;
        else
            ++detached;
        thread.live = false;
    }
    std::uint8_t final_value = 0xff;
    if (!ReadExact(mem, direct, &final_value, sizeof(final_value))) ++read_errors;
    close(mem);
    const bool clean = ptrace_errors == 0 && read_errors == 0 &&
                       unexpected_stops == 0 &&
                       detached + exited_threads ==
                           initial_threads + thread_additions;
    std::printf(
        "DIRECT_MODE_AFFINITY_V1_DONE events=%" PRIu64
        " to_one=%" PRIu64 " to_zero=%" PRIu64
        " initial_threads=%zu additions=%" PRIu64 " exited=%" PRIu64
        " detached=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " unexpected_stops=%" PRIu64 " final=%u clean=%d activations=0\n",
        events, to_one, to_zero, initial_threads, thread_additions,
        exited_threads, detached, read_errors, ptrace_errors,
        unexpected_stops,
        static_cast<unsigned>(final_value), clean ? 1 : 0);
    return clean && events > 0 && to_one > 0 ? 0 : 6;
}
