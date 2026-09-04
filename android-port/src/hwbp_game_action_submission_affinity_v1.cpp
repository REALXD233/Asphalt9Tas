// Observation-only producer-affinity probe for the game-owned action route.
//
// DR0: CarPhysicsState command completion-vector end (+0x1368), write/8
// DR1: NitroState active byte (+0x188), write/1
// DR2: NitroState mode (+0x18C), write/4
// DR3: MainTimeSource fixed-delta accumulator (+0x150), write/8
//
// Target-process writes are limited to x86_64 debug registers.  The probe
// performs no game call, input injection, guest patch, or gameplay-value write.

#define A9TAS_NITRO_AFFINITY_ENTRY A9TasNitroAffinityMain_NotUsed
#include "hwbp_nitro_thread_affinity_observer_v1.cpp"
#undef A9TAS_NITRO_AFFINITY_ENTRY

#include <fstream>

namespace {

constexpr std::uintptr_t kCommandVectorOffset = 0x1360;
constexpr std::uintptr_t kDirectModeOffset = 0x1378;
constexpr std::uintptr_t kActionFallbackVtableRva = 0x7EEA080;
constexpr std::uintptr_t kActionFallbackVfunc90Rva = 0x367B548;
constexpr std::uintptr_t kActionOwnerAdjustMetadataDelta = 0xA0;
constexpr std::uintptr_t kActionDispatchAdjustMetadataDelta = 0x230;
constexpr std::uintptr_t kActionDispatchVtableRva = 0x7EEFE68;
constexpr std::uintptr_t kActionDispatchVfunc158Rva = 0x36A9CAC;
constexpr std::size_t kMaxTokenCount = 4096;

struct QueueSnapshot {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uintptr_t capacity{};
    std::uintptr_t last_token{};
    std::uint64_t count{};
    std::uint8_t direct_mode{0xff};
    std::uint8_t last_completion{0xff};
    bool shape_ok{};
    bool completion_ok{};
};

std::string SubmissionThreadName(pid_t pid, pid_t tid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/task/" +
                             std::to_string(tid) + "/comm";
    std::ifstream input(path);
    std::string name;
    std::getline(input, name);
    return name;
}

bool ReadQueueSnapshot(int mem, std::uintptr_t owner,
                       QueueSnapshot* output) {
    *output = {};
    output->direct_mode = 0xff;
    output->last_completion = 0xff;
    const bool header_ok =
        ReadExact(mem, owner + kCommandVectorOffset, &output->begin,
                  sizeof(output->begin)) &&
        ReadExact(mem, owner + kCommandVectorOffset + 8, &output->end,
                  sizeof(output->end)) &&
        ReadExact(mem, owner + kCommandVectorOffset + 16, &output->capacity,
                  sizeof(output->capacity)) &&
        ReadExact(mem, owner + kDirectModeOffset, &output->direct_mode,
                  sizeof(output->direct_mode));
    if (!header_ok || output->direct_mode > 1) return false;
    output->shape_ok =
        output->begin <= output->end && output->end <= output->capacity &&
        ((output->end - output->begin) % sizeof(std::uintptr_t)) == 0 &&
        ((output->capacity - output->begin) % sizeof(std::uintptr_t)) == 0;
    if (!output->shape_ok) return true;
    output->count =
        (output->end - output->begin) / sizeof(std::uintptr_t);
    if (output->count > kMaxTokenCount) {
        output->shape_ok = false;
        return true;
    }
    if (output->count == 0) return true;
    if (!ReadExact(mem, output->end - sizeof(std::uintptr_t),
                   &output->last_token, sizeof(output->last_token)) ||
        output->last_token == 0)
        return true;
    output->completion_ok = ReadExact(
        mem, output->last_token + 0x11, &output->last_completion,
        sizeof(output->last_completion));
    return true;
}

bool ValidateActionOwner(int mem, const std::vector<Mapping>& maps,
                         std::uintptr_t base, std::uintptr_t owner) {
    QueueSnapshot queue{};
    std::uintptr_t command_interface = 0;
    std::uintptr_t interface_head = 0;
    std::int64_t interface_adjustment = 0;
    std::uintptr_t dispatch_this = 0;
    std::uintptr_t dispatch_vtable = 0;
    std::uintptr_t dispatch_vfunc_158 = 0;
    if (owner == 0 || (owner & 7u) != 0 ||
        !Writable(maps, owner + 0x30, sizeof(command_interface)) ||
        !ReadExact(mem, owner + 0x30, &command_interface,
                   sizeof(command_interface)) ||
        !ReadQueueSnapshot(mem, owner, &queue) || !queue.shape_ok ||
        !FindMapping(maps, command_interface, sizeof(interface_head)) ||
        !ReadExact(mem, command_interface, &interface_head,
                   sizeof(interface_head)) ||
        interface_head < kActionDispatchAdjustMetadataDelta ||
        !FindMapping(maps,
                     interface_head - kActionDispatchAdjustMetadataDelta,
                     sizeof(interface_adjustment)) ||
        !ReadExact(mem,
                   interface_head - kActionDispatchAdjustMetadataDelta,
                   &interface_adjustment, sizeof(interface_adjustment)))
        return false;

    const auto signed_interface =
        static_cast<std::intptr_t>(command_interface);
    if ((interface_adjustment > 0 &&
         signed_interface > INTPTR_MAX - interface_adjustment) ||
        (interface_adjustment < 0 &&
         signed_interface < INTPTR_MIN - interface_adjustment))
        return false;
    const auto signed_dispatch = signed_interface + interface_adjustment;
    if (signed_dispatch <= 0) return false;
    dispatch_this = static_cast<std::uintptr_t>(signed_dispatch);
    return FindMapping(maps, dispatch_this, sizeof(dispatch_vtable)) &&
           ReadExact(mem, dispatch_this, &dispatch_vtable,
                     sizeof(dispatch_vtable)) &&
           dispatch_vtable == base + kActionDispatchVtableRva &&
           FindMapping(maps, dispatch_vtable + 0x158,
                       sizeof(dispatch_vfunc_158)) &&
           ReadExact(mem, dispatch_vtable + 0x158, &dispatch_vfunc_158,
                     sizeof(dispatch_vfunc_158)) &&
           dispatch_vfunc_158 == base + kActionDispatchVfunc158Rva;
}

bool ResolveActionOwner(pid_t pid, std::uintptr_t base,
                        std::uintptr_t explicit_owner,
                        std::uintptr_t* action_owner) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;
    if (explicit_owner != 0) {
        const bool valid = ValidateActionOwner(mem, maps, base, explicit_owner);
        close(mem);
        if (!valid)
            std::fprintf(stderr,
                         "explicit action owner failed validation\n");
        if (valid) *action_owner = explicit_owner;
        return valid;
    }

    const std::uintptr_t fallback_vtable =
        base + kActionFallbackVtableRva;
    std::uintptr_t fallback_vfunc_90 = 0;
    std::int64_t owner_adjustment = 0;
    if (!FindMapping(maps, fallback_vtable + 0x90,
                     sizeof(fallback_vfunc_90)) ||
        !ReadExact(mem, fallback_vtable + 0x90, &fallback_vfunc_90,
                   sizeof(fallback_vfunc_90)) ||
        fallback_vfunc_90 != base + kActionFallbackVfunc90Rva ||
        fallback_vtable < kActionOwnerAdjustMetadataDelta ||
        !FindMapping(maps,
                     fallback_vtable - kActionOwnerAdjustMetadataDelta,
                     sizeof(owner_adjustment)) ||
        !ReadExact(mem,
                   fallback_vtable - kActionOwnerAdjustMetadataDelta,
                   &owner_adjustment, sizeof(owner_adjustment))) {
        close(mem);
        return false;
    }

    std::vector<std::uintptr_t> candidates;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const auto& map : maps) {
        if (map.perms[0] != 'r' || map.perms[1] != 'w') continue;
        if (map.path == "[vvar]" || map.path == "[vdso]") continue;
        for (std::uintptr_t cursor = map.begin; cursor < map.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(buffer.size(), map.end - cursor));
            const ssize_t got = pread(mem, buffer.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                cursor += want;
                continue;
            }
            for (std::size_t off = 0;
                 off + sizeof(std::uintptr_t) <=
                     static_cast<std::size_t>(got);
                 off += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + off, sizeof(value));
                if (value != fallback_vtable) continue;
                const auto signed_fallback = static_cast<std::intptr_t>(
                    cursor + static_cast<std::uintptr_t>(off));
                if ((owner_adjustment > 0 &&
                     signed_fallback > INTPTR_MAX - owner_adjustment) ||
                    (owner_adjustment < 0 &&
                     signed_fallback < INTPTR_MIN - owner_adjustment))
                    continue;
                const auto signed_owner = signed_fallback + owner_adjustment;
                if (signed_owner <= 0) continue;
                const auto owner = static_cast<std::uintptr_t>(signed_owner);
                if (ValidateActionOwner(mem, maps, base, owner))
                    candidates.push_back(owner);
            }
            cursor += static_cast<std::uintptr_t>(got);
        }
    }
    close(mem);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() != 1) {
        std::fprintf(stderr,
                     "action owner candidates=%zu (required exactly 1)\n",
                     candidates.size());
        for (std::uintptr_t candidate : candidates)
            std::fprintf(stderr, "  candidate=0x%" PRIxPTR "\n", candidate);
        return false;
    }
    *action_owner = candidates.front();
    return true;
}

unsigned long SubmissionAffinityDr7() {
    // Local write breakpoints. Length encoding: 8=2, 1=0, 4=3, 8=2.
    return 1UL | (1UL << 16) | (2UL << 18) |
           (1UL << 2) | (1UL << 20) |
           (1UL << 4) | (1UL << 24) | (3UL << 26) |
           (1UL << 6) | (1UL << 28) | (2UL << 30);
}

void WritePreflightResult(const char* path, const char* stage,
                          std::uintptr_t owner = 0,
                          std::uintptr_t service = 0,
                          std::uintptr_t queue_end = 0) {
    FILE* file = std::fopen(path, "wb");
    if (!file) return;
    std::fprintf(file,
                 "GAME_ACTION_SUBMISSION_PREFLIGHT stage=%s owner=0x%" PRIxPTR
                 " service=0x%" PRIxPTR " queue_end=0x%" PRIxPTR
                 " attached=0 game_calls=0 input_writes=0\n",
                 stage, owner, service, queue_end);
    std::fclose(file);
}

void WriteSnapshotFailure(const char* path, std::uintptr_t owner,
                          std::uintptr_t service,
                          std::uintptr_t queue_end,
                          const QueueSnapshot& queue, bool queue_read,
                          bool nitro_read) {
    FILE* file = std::fopen(path, "wb");
    if (!file) return;
    std::fprintf(
        file,
        "GAME_ACTION_SUBMISSION_PREFLIGHT stage=initial_snapshot_failed"
        " owner=0x%" PRIxPTR " service=0x%" PRIxPTR
        " queue_end=0x%" PRIxPTR " begin=0x%" PRIxPTR
        " end=0x%" PRIxPTR " capacity=0x%" PRIxPTR
        " count=%" PRIu64 " direct=%u queue_read=%u shape_ok=%u"
        " nitro_read=%u attached=0 game_calls=0 input_writes=0\n",
        owner, service, queue_end, queue.begin, queue.end, queue.capacity,
        queue.count, static_cast<unsigned>(queue.direct_mode),
        queue_read ? 1u : 0u, queue.shape_ok ? 1u : 0u,
        nitro_read ? 1u : 0u);
    std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "[MAIN_OBJECT_HEX] [FINAL_OWNER_HEX]\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t main_value = 0, owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        (argc >= 6 && !ParseUnsigned(argv[5], 16, &main_value)) ||
        (argc == 7 && !ParseUnsigned(argv[6], 16, &owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 1000 || duration_value > 60000)
        return 2;

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    if (!VerifyTargetBuild(pid, base)) {
        WritePreflightResult(argv[4], "target_build_failed");
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }
    std::uintptr_t main_object = 0, physics_owner = 0, owner = 0;
    if (!ResolveMainObject(pid, base,
                           static_cast<std::uintptr_t>(main_value),
                           &main_object)) {
        WritePreflightResult(argv[4], "main_object_failed");
        std::fprintf(stderr, "scheduler main-object resolution failed\n");
        return 3;
    }
    if (!ResolveFinalOwner(pid, base, 0, &physics_owner)) {
        WritePreflightResult(argv[4], "physics_owner_failed");
        std::fprintf(stderr, "physics owner resolution failed\n");
        return 3;
    }
    if (!ResolveActionOwner(pid, base,
                            static_cast<std::uintptr_t>(owner_value), &owner)) {
        WritePreflightResult(argv[4], "action_owner_failed");
        std::fprintf(stderr, "action owner resolution failed\n");
        return 3;
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;
    std::vector<Mapping> maps;
    std::uintptr_t service = 0;
    if (!ReadMaps(pid, &maps) ||
        !ResolveNitroService(mem, maps, base, physics_owner, &service)) {
        WritePreflightResult(argv[4], "nitro_service_failed", owner);
        close(mem);
        return 3;
    }
    const std::uintptr_t state = service + kNitroStateOffset;
    const std::uintptr_t queue_end = owner + kCommandVectorOffset + 8;
    const std::uintptr_t active = state + kActiveOffset;
    const std::uintptr_t mode = state + kModeOffset;
    const std::uintptr_t delta = main_object + kAccumulatorOffset;
    if ((queue_end & 7u) != 0 || (mode & 3u) != 0 || (delta & 7u) != 0 ||
        !Writable(maps, queue_end, 8) || !Writable(maps, active, 1) ||
        !Writable(maps, mode, 4) || !Writable(maps, delta, 8)) {
        WritePreflightResult(argv[4], "watch_validation_failed", owner,
                             service, queue_end);
        std::fprintf(stderr, "submission-affinity watch validation failed\n");
        close(mem);
        return 3;
    }
    QueueSnapshot initial_queue{};
    NitroSnapshot initial_nitro{};
    const bool initial_queue_read =
        ReadQueueSnapshot(mem, owner, &initial_queue);
    const bool initial_nitro_read =
        ReadSnapshot(mem, state, &initial_nitro);
    if (!initial_queue_read || !initial_queue.shape_ok ||
        !initial_nitro_read) {
        WriteSnapshotFailure(argv[4], owner, service, queue_end,
                             initial_queue, initial_queue_read,
                             initial_nitro_read);
        close(mem);
        return 3;
    }
    const char* preflight_only =
        std::getenv("A9TAS_SUBMISSION_PREFLIGHT_ONLY");
    if (preflight_only && std::strcmp(preflight_only, "1") == 0) {
        WritePreflightResult(argv[4], "ready", owner, service, queue_end);
        std::printf(
            "GAME_ACTION_SUBMISSION_PREFLIGHT stage=ready owner=0x%" PRIxPTR
            " service=0x%" PRIxPTR " queue_end=0x%" PRIxPTR
            " count=%" PRIu64 " direct=%u attached=0 game_calls=0"
            " input_writes=0\n",
            owner, service, queue_end, initial_queue.count,
            static_cast<unsigned>(initial_queue.direct_mode));
        close(mem);
        return 0;
    }
    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 4;
    }

    std::vector<TracedThread> threads;
    std::uint64_t ptrace_errors = 0, read_errors = 0;
    std::uint64_t unexpected_stops = 0, additions = 0, exited = 0;
    std::uint64_t events = 0, queue_hits = 0, appends = 0, cleanups = 0;
    std::uint64_t active_hits = 0, mode_hits = 0, delta_hits = 0;
    std::uint64_t previous_count = initial_queue.count;
    const std::size_t initial_threads = AttachNewThreads(
        pid, queue_end, active, mode, delta, &threads, &ptrace_errors,
        SubmissionAffinityDr7());
    if (initial_threads == 0 || ptrace_errors != 0) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        std::fclose(out);
        close(mem);
        return 5;
    }

    const std::uint64_t started = MonotonicNs();
    const std::uint64_t deadline = started + duration_value * 1000000ULL;
    std::uint64_t next_rescan = started + 250000000ULL;
    char line[2048]{};
    std::snprintf(
        line, sizeof(line),
        "GAME_ACTION_SUBMISSION_AFFINITY_V1_ARMED pid=%d owner=0x%" PRIxPTR
        " queue_end=0x%" PRIxPTR " active=0x%" PRIxPTR
        " mode=0x%" PRIxPTR " delta=0x%" PRIxPTR
        " initial_count=%" PRIu64 " threads=%zu duration_ms=%" PRIu64
        " writes=debug-registers-only game_calls=0 input_writes=0\n",
        pid, owner, queue_end, active, mode, delta, initial_queue.count,
        threads.size(), duration_value);
    std::fputs(line, stdout);
    std::fputs(line, out);
    std::fflush(stdout);
    std::fflush(out);

    while (MonotonicNs() < deadline) {
        const std::uint64_t now = MonotonicNs();
        if (now >= next_rescan) {
            std::uint64_t failures = 0;
            additions += AttachNewThreads(
                pid, queue_end, active, mode, delta, &threads, &failures,
                SubmissionAffinityDr7());
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
            if (tracked && tracked->live) {
                tracked->live = false;
                tracked->stopped = false;
                ++exited;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        if (signal == SIGTRAP && PeekDebug(tid, 6, &dr6) &&
            (dr6 & 15UL) != 0) {
            QueueSnapshot queue{};
            NitroSnapshot nitro{};
            std::int64_t delta_value = 0;
            user_regs_struct regs{};
            const bool read_ok =
                ReadQueueSnapshot(mem, owner, &queue) && queue.shape_ok &&
                ReadSnapshot(mem, state, &nitro) &&
                ReadExact(mem, delta, &delta_value, sizeof(delta_value));
            const bool regs_ok =
                ptrace(PTRACE_GETREGS, tid, nullptr, &regs) != -1;
            if (!read_ok) ++read_errors;
            if (!regs_ok) ++ptrace_errors;
            const bool queue_hit = (dr6 & 1UL) != 0;
            if (queue_hit) {
                ++queue_hits;
                if (read_ok && queue.count == previous_count + 1) ++appends;
                if (read_ok && queue.count < previous_count) ++cleanups;
                if (read_ok) previous_count = queue.count;
            }
            if (dr6 & 2UL) ++active_hits;
            if (dr6 & 4UL) ++mode_hits;
            if (dr6 & 8UL) ++delta_hits;
            const std::string name = SubmissionThreadName(pid, tid);
            std::snprintf(
                line, sizeof(line),
                "GAME_ACTION_SUBMISSION_EVENT seq=%" PRIu64
                " ns=%" PRIu64 " tid=%d name=%s flags=%c%c%c%c"
                " count=%" PRIu64 " direct=%u token=0x%" PRIxPTR
                " completion=%u completion_ok=%u active=%u mode=%u"
                " delta=%" PRId64 " rip=0x%llx read_ok=%u regs_ok=%u\n",
                events++, MonotonicNs() - started, tid,
                name.empty() ? "<unknown>" : name.c_str(),
                queue_hit ? 'Q' : '-', (dr6 & 2UL) ? 'A' : '-',
                (dr6 & 4UL) ? 'M' : '-', (dr6 & 8UL) ? 'D' : '-',
                queue.count, static_cast<unsigned>(queue.direct_mode),
                queue.last_token,
                static_cast<unsigned>(queue.last_completion),
                queue.completion_ok ? 1u : 0u,
                static_cast<unsigned>(nitro.active_188), nitro.mode_18c,
                delta_value, static_cast<unsigned long long>(regs.rip),
                read_ok ? 1u : 0u, regs_ok ? 1u : 0u);
            std::fputs(line, stdout);
            std::fputs(line, out);
            std::fflush(stdout);
            std::fflush(out);
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
    close(mem);
    const bool clean = ptrace_errors == 0 && read_errors == 0 &&
                       unexpected_stops == 0 &&
                       detached + exited == initial_threads + additions;
    std::snprintf(
        line, sizeof(line),
        "GAME_ACTION_SUBMISSION_AFFINITY_V1_DONE events=%" PRIu64
        " queue_hits=%" PRIu64 " appends=%" PRIu64
        " cleanups=%" PRIu64 " active=%" PRIu64 " mode=%" PRIu64
        " delta=%" PRIu64 " read_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " clean=%u game_calls=0 input_writes=0\n",
        events, queue_hits, appends, cleanups, active_hits, mode_hits,
        delta_hits, read_errors, ptrace_errors, unexpected_stops,
        clean ? 1u : 0u);
    std::fputs(line, stdout);
    std::fputs(line, out);
    std::fflush(stdout);
    const bool output_ok = std::fflush(out) == 0 && std::ferror(out) == 0 &&
                           std::fclose(out) == 0;
    return clean && output_ok ? 0 : 6;
}
