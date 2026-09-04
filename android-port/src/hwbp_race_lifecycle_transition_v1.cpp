// BUILD-ONLY one-shot observer for the authoritative Android race start.
//
// DR0 watches the uniquely resolved central race object's +0x2D8 phase field
// as write/4.  The observer accepts only the game-owned 2 -> 3 transition.
// It never writes game memory; its target-side mutations are x86_64 debug
// registers required for the hardware data watchpoint.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "race_lifecycle_object_resolver_v1.h"

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_ONE_RACE_LIFECYCLE_2_TO_3_WATCH_V1";

bool ReadProcessStartTicksLifecycle(pid_t pid, std::uint64_t* output) {
  if (pid <= 0 || output == nullptr) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buffer[4096]{};
  const ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (count <= 0) return false;
  buffer[count] = '\0';
  char* cursor = std::strrchr(buffer, ')');
  if (cursor == nullptr) return false;
  ++cursor;
  for (int field = 3; field <= 22; ++field) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0' || *cursor == '\n') return false;
    char* end = cursor;
    while (*end != '\0' && *end != '\n' && *end != ' ') ++end;
    if (field == 22) {
      char* parsed_end = nullptr;
      errno = 0;
      const unsigned long long value = std::strtoull(cursor, &parsed_end, 10);
      if (errno != 0 || parsed_end != end || value == 0) return false;
      *output = static_cast<std::uint64_t>(value);
      return true;
    }
    cursor = end;
  }
  return false;
}

bool CreateReadyMarker(const char* path, pid_t pid,
                       std::uint64_t start_ticks,
                       std::uintptr_t object,
                       std::uintptr_t state_address,
                       std::size_t attached_threads,
                       std::uint32_t freeze_passes) {
  if (path == nullptr || attached_threads == 0) return false;
  const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  char payload[320]{};
  const int length = std::snprintf(
      payload, sizeof(payload),
      "READY_RACE_LIFECYCLE_V1 pid=%d start_ticks=%" PRIu64
      " controller_pid=%d object=0x%" PRIxPTR
       " state_address=0x%" PRIxPTR " state=2 attached_threads=%zu"
       " all_target_threads_frozen=1 thread_set_stable=1 freeze_passes=%u"
       " gameplay_writes=0\n",
       static_cast<int>(pid), start_ticks, static_cast<int>(getpid()), object,
       state_address, attached_threads, freeze_passes);
  bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(payload);
  std::size_t written = 0;
  while (ok && written < static_cast<std::size_t>(length)) {
    const ssize_t amount =
        write(fd, payload + written,
              static_cast<std::size_t>(length) - written);
    if (amount <= 0)
      ok = false;
    else
      written += static_cast<std::size_t>(amount);
  }
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!ok) unlink(path);
  return ok;
}

bool WaitForMarkerRemoval(const char* path, std::uint64_t timeout_ms) {
  const std::uint64_t deadline = MonotonicNs() + timeout_ms * 1000000ULL;
  while (MonotonicNs() < deadline) {
    if (access(path, F_OK) != 0) return errno == ENOENT;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return false;
}

unsigned long RacePhaseDr7() {
  // Local DR0, write-only, four-byte length.
  return 1UL | (1UL << 16) | (3UL << 18);
}

bool FreezeStableThreadSet(pid_t pid, std::uintptr_t state_address,
                           std::vector<TracedThread>* threads,
                           std::uint64_t* failures,
                           std::uint32_t* completed_passes) {
  if (threads == nullptr || failures == nullptr || completed_passes == nullptr)
    return false;
  *completed_passes = 0;
  for (std::uint32_t pass = 0; pass < 4; ++pass) {
    for (auto& thread : *threads) {
      if (!thread.live || thread.stopped) continue;
      if (!StopThread(thread.tid)) {
        ++*failures;
        return false;
      }
      thread.stopped = true;
    }
    const std::size_t added = AttachNewThreadsStopped(
        pid, state_address, 0, 0, 0, threads, failures, RacePhaseDr7());
    *completed_passes = pass + 1;
    if (*failures != 0) return false;
    if (added == 0) return !threads->empty();
  }
  return false;
}

bool ContinueAll(std::vector<TracedThread>* threads,
                 std::uint64_t* failures) {
  bool ok = true;
  for (auto& thread : *threads) {
    if (!thread.live || !thread.stopped) continue;
    if (!ContinueThread(thread.tid)) {
      ++*failures;
      ok = false;
    } else {
      thread.stopped = false;
    }
  }
  return ok;
}

bool FreezeAllExcept(std::vector<TracedThread>* threads, pid_t except,
                     std::uint64_t* failures) {
  bool ok = true;
  for (auto& thread : *threads) {
    if (!thread.live || thread.stopped || thread.tid == except) continue;
    if (!StopThread(thread.tid)) {
      ++*failures;
      ok = false;
    } else {
      thread.stopped = true;
    }
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9 || std::strcmp(argv[8], kAcknowledgement) != 0) {
    std::fprintf(
        stderr,
        "usage: %s PID LIB_BASE_HEX TIMEOUT_MS START_TICKS OBJECT_HEX "
        "STATE_ADDRESS_HEX READY_MARKER "
        "I_ACCEPT_ONE_RACE_LIFECYCLE_2_TO_3_WATCH_V1\n",
        argv[0]);
    return 2;
  }
  if (access(argv[7], F_OK) == 0) {
    std::fprintf(stderr, "ready marker already exists; no process attached\n");
    return 2;
  }
  std::uint64_t pid_value = 0, base_value = 0, timeout_value = 0;
  std::uint64_t expected_start_ticks = 0, object_value = 0, state_value = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &timeout_value) ||
      !ParseUnsigned(argv[4], 10, &expected_start_ticks) ||
      !ParseUnsigned(argv[5], 16, &object_value) ||
      !ParseUnsigned(argv[6], 16, &state_value) ||
      pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
      base_value == 0 || expected_start_ticks == 0 || object_value == 0 ||
      state_value == 0 || timeout_value < 5000 || timeout_value > 60000) {
    std::fprintf(stderr, "invalid lifecycle-watch arguments\n");
    return 2;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  const auto base = static_cast<std::uintptr_t>(base_value);
  const auto object = static_cast<std::uintptr_t>(object_value);
  const auto state_address = static_cast<std::uintptr_t>(state_value);
  const auto timeout_ms = static_cast<std::uint64_t>(timeout_value);
  if (object > UINTPTR_MAX - a9tas::race_lifecycle_v1::kPhaseStateOffset ||
      state_address !=
          object + a9tas::race_lifecycle_v1::kPhaseStateOffset ||
      (state_address & 3u) != 0) {
    std::fprintf(stderr, "lifecycle object/state binding mismatch\n");
    return 2;
  }
  std::uint64_t observed_start_ticks = 0;
  if (!ReadProcessStartTicksLifecycle(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr, "lifecycle PID/start-time mismatch; no attach\n");
    return 3;
  }
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return 4;
  if (!a9tas::race_lifecycle_v1::VerifyTargetBuild(mem, base)) {
    close(mem);
    std::fprintf(stderr, "lifecycle target signature mismatch; no attach\n");
    return 3;
  }
  std::uintptr_t vptr = 0;
  a9tas::race_lifecycle_v1::Candidate candidate{};
  if (!a9tas::race_lifecycle_v1::ReadExact(
          mem, object, &vptr, sizeof(vptr)) ||
      !a9tas::race_lifecycle_v1::ValidateCandidate(
          mem, base, object, vptr, &candidate) ||
      candidate.state_address != state_address ||
      candidate.state != a9tas::race_lifecycle_v1::kCountdownState) {
    close(mem);
    std::fprintf(stderr, "lifecycle countdown candidate no longer valid\n");
    return 3;
  }

  std::vector<TracedThread> threads;
  std::uint64_t ptrace_errors = 0;
  std::uint64_t semantic_errors = 0;
  std::uint64_t read_errors = 0;
  std::uint64_t thread_additions = 0;
  std::uint64_t exited_threads = 0;
  std::uint32_t freeze_passes = 0;
  const bool stable_thread_set = FreezeStableThreadSet(
      pid, state_address, &threads, &ptrace_errors, &freeze_passes);
  const std::size_t initial = threads.size();
  std::uint32_t armed_state = 0;
  const bool armed_state_read = a9tas::race_lifecycle_v1::ReadExact(
      mem, state_address, &armed_state, sizeof(armed_state));
  bool ready_created = false;
  if (initial != 0 && stable_thread_set && ptrace_errors == 0 &&
      armed_state_read &&
      armed_state == a9tas::race_lifecycle_v1::kCountdownState) {
    ready_created = CreateReadyMarker(
        argv[7], pid, observed_start_ticks, object, state_address,
        threads.size(), freeze_passes);
  }
  if (initial == 0 || !stable_thread_set || ptrace_errors != 0 ||
      !armed_state_read ||
      armed_state != a9tas::race_lifecycle_v1::kCountdownState ||
      !ready_created) {
    std::fprintf(
        stderr,
        "RACE_LIFECYCLE_TRANSITION_PREARM_FAILURE initial=%zu stable=%u"
        " freeze_passes=%u ptrace_errors=%" PRIu64
        " state_read=%u armed_state=%u ready_created=%u\n",
        initial, stable_thread_set ? 1u : 0u, freeze_passes, ptrace_errors,
        armed_state_read ? 1u : 0u, armed_state, ready_created ? 1u : 0u);
    for (auto& thread : threads)
      if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
    close(mem);
    unlink(argv[7]);
    return 5;
  }
  std::printf(
      "RACE_LIFECYCLE_TRANSITION_ARMED pid=%d start_ticks=%" PRIu64
       " object=0x%" PRIxPTR " state_address=0x%" PRIxPTR
       " state=%u threads=%zu all_target_threads_frozen=1"
       " thread_set_stable=1 freeze_passes=%u gameplay_writes=0\n",
       static_cast<int>(pid), observed_start_ticks, object, state_address,
       armed_state, threads.size(), freeze_passes);
  std::fflush(stdout);

  bool accepted = false;
  bool released = false;
  pid_t event_tid = 0;
  std::uint64_t event_ns = 0;
  std::uint64_t event_rip = 0;
  std::uint32_t observed_before = armed_state;
  std::uint32_t observed_after = armed_state;
  std::uint64_t event_count = 0;
  std::uint64_t unexpected_stops = 0;
  if (!WaitForMarkerRemoval(argv[7], timeout_ms)) {
    ++semantic_errors;
  } else if (!ContinueAll(&threads, &ptrace_errors)) {
    ++semantic_errors;
  } else {
    released = true;
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 100000000ULL;
    while (MonotonicNs() < deadline_ns && !accepted &&
           ptrace_errors == 0 && semantic_errors == 0) {
      const std::uint64_t now_ns = MonotonicNs();
      if (now_ns >= next_rescan_ns) {
        std::uint64_t failures = 0;
        thread_additions += AttachNewThreads(
            pid, state_address, 0, 0, 0, &threads, &failures, RacePhaseDr7());
        ptrace_errors += failures;
        next_rescan_ns = now_ns + 100000000ULL;
      }
      int status = 0;
      const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
      if (tid == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      if (tid < 0) {
        if (errno == EINTR) continue;
        if (errno != ECHILD) ++ptrace_errors;
        continue;
      }
      TracedThread* tracked = FindThread(&threads, tid);
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        if (tracked && tracked->live) {
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
        ++event_count;
        user_regs_struct regs{};
        if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1)
          ++ptrace_errors;
        if (!a9tas::race_lifecycle_v1::ReadExact(
                mem, state_address, &observed_after,
                sizeof(observed_after))) {
          ++read_errors;
        } else if (event_count != 1 ||
                   observed_before !=
                       a9tas::race_lifecycle_v1::kCountdownState ||
                   observed_after !=
                       a9tas::race_lifecycle_v1::kRacingState) {
          ++semantic_errors;
        } else {
          accepted = true;
          event_tid = tid;
          event_ns = MonotonicNs() - start_ns;
          event_rip = static_cast<std::uint64_t>(regs.rip);
          FreezeAllExcept(&threads, tid, &ptrace_errors);
        }
      } else {
        ++unexpected_stops;
        const int deliver = signal == SIGTRAP ? 0 : signal;
        if (!ContinueThread(tid, deliver))
          ++ptrace_errors;
        else if (tracked)
          tracked->stopped = false;
      }
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
  std::uint32_t final_state = 0;
  if (!a9tas::race_lifecycle_v1::ReadExact(
          mem, state_address, &final_state, sizeof(final_state)))
    ++read_errors;
  close(mem);
  unlink(argv[7]);
  const bool clean =
      released && accepted && observed_before == 2 && observed_after == 3 &&
      final_state == 3 && event_count == 1 && ptrace_errors == 0 &&
      read_errors == 0 && semantic_errors == 0 && unexpected_stops == 0 &&
      detached + exited_threads == initial + thread_additions;
  std::printf(
      "RACE_LIFECYCLE_TRANSITION_DONE accepted=%u before=%u after=%u "
      "final=%u events=%" PRIu64 " event_tid=%d event_ns=%" PRIu64
      " event_rip=0x%" PRIx64 " initial_threads=%zu additions=%" PRIu64
      " exited=%" PRIu64 " detached=%" PRIu64
      " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
      " semantic_errors=%" PRIu64 " unexpected_stops=%" PRIu64
      " clean_detach=%u target_memory_write_attempts=0"
      " gameplay_writes=0\n",
      accepted ? 1u : 0u, observed_before, observed_after, final_state,
      event_count, static_cast<int>(event_tid), event_ns, event_rip, initial,
      thread_additions, exited_threads, detached, read_errors, ptrace_errors,
      semantic_errors, unexpected_stops, clean ? 1u : 0u);
  return clean ? 0 : 6;
}
