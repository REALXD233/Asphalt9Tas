// FC-2 three-frame natural deferred-registration transaction controller.
//
// Default builds are inert.  The complete path is compiled only into an
// unlinked review object when A9TAS_FC2_LIVE_CANDIDATE=1.  It performs one
// CarPhysicsState vptr swap and no gameplay/action/physics write.  Success
// requires exact callback-list membership 0 -> 1 -> 0 across three natural
// dispatch-open boundaries and a dedicated-evidence HWBP hit on the same TID.

#include <cstdio>

#ifndef A9TAS_FC2_LIVE_CANDIDATE
#define A9TAS_FC2_LIVE_CANDIDATE 0
#endif

#ifndef A9TAS_FC3_TAIL_CANDIDATE
#define A9TAS_FC3_TAIL_CANDIDATE 0
#endif

#ifndef A9TAS_FC3_PHASE_MAP_CANDIDATE
#define A9TAS_FC3_PHASE_MAP_CANDIDATE 0
#endif

#ifndef A9TAS_FC2_CONTROLLER_NO_MAIN
#define A9TAS_FC2_CONTROLLER_NO_MAIN 0
#endif

#if A9TAS_FC2_LIVE_CANDIDATE != 0 && A9TAS_FC2_LIVE_CANDIDATE != 1
#error "A9TAS_FC2_LIVE_CANDIDATE must be 0 or 1"
#endif

#if A9TAS_FC3_TAIL_CANDIDATE != 0 && A9TAS_FC3_TAIL_CANDIDATE != 1
#error "A9TAS_FC3_TAIL_CANDIDATE must be 0 or 1"
#endif

#if A9TAS_FC3_PHASE_MAP_CANDIDATE != 0 && \
    A9TAS_FC3_PHASE_MAP_CANDIDATE != 1
#error "A9TAS_FC3_PHASE_MAP_CANDIDATE must be 0 or 1"
#endif

#if A9TAS_FC3_TAIL_CANDIDATE == 1 && A9TAS_FC2_LIVE_CANDIDATE != 1
#error "FC-3 tail requires the FC-2 successor transaction controller"
#endif

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1 && \
    A9TAS_FC3_TAIL_CANDIDATE != 1
#error "FC-3 phase map requires the FC-3 successor transaction controller"
#endif

#if A9TAS_FC2_CONTROLLER_NO_MAIN != 0 && \
    A9TAS_FC2_CONTROLLER_NO_MAIN != 1
#error "A9TAS_FC2_CONTROLLER_NO_MAIN must be 0 or 1"
#endif

#if A9TAS_FC2_LIVE_CANDIDATE == 1

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"
#include "fc2_payload_elf_resolver_v1.h"
#include "fc2_transaction_report_v1.h"

#include <array>
#include <string>
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
#include <climits>
#endif

#if A9TAS_FC3_TAIL_CANDIDATE == 1
#include "fc3_identity_resolver_v1.h"
#include "fc3_observer_event_core_v1.h"
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
#include "fc3_phase_map_v1.h"
#endif

#include <memory>

extern "C" int a9tas_fc3_resolve_identities_review_v1(
    pid_t pid, int process_mem, std::uintptr_t base,
    a9tas::fc3_identity_v1::Layout* output);
#endif

namespace {

using a9tas::fc2_report_v1::PayloadControl;
using a9tas::fc2_report_v1::PayloadEvidence;
using a9tas::fc2_report_v1::Report;
using a9tas::fc2_report_v1::Phase;

constexpr std::uintptr_t kPhysicsContextVtable = 0x8103830;
constexpr std::uintptr_t kPhysicsContextAdd = 0x38B77CC;
constexpr std::uintptr_t kPhysicsContextRemove = 0x38B7840;
constexpr std::uintptr_t kCarPhysicsPrimaryVtable = 0x7EE8D18;
constexpr std::uintptr_t kPrimaryPrefix = 0x7EE8CC0;
constexpr std::uintptr_t kOriginalCallback = 0x367D66C;
constexpr std::size_t kShadowSize = 0x908;
constexpr std::size_t kPrefixSize = 0x58;
constexpr std::size_t kCallbackSlot = 0x10;
constexpr std::uintptr_t kCallbackListOffset = 0x180;
constexpr std::uintptr_t kCallbackFlagsOffset = 0x1A0;
constexpr std::uintptr_t kAddVslotOffset = 0x50;
constexpr std::uintptr_t kRemoveVslotOffset = 0x58;
constexpr std::uint64_t kMinimumTimeoutMs = 250;
constexpr std::uint64_t kMaximumTimeoutMs = 3000;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 0
constexpr std::uint64_t kTransactionDeadlineNs = 250000000ULL;
#endif
constexpr char kAcknowledgement[] =
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    "I_ACCEPT_FC3_PHASE_MAP_OBSERVE_ONLY_V1";
#elif A9TAS_FC3_TAIL_CANDIDATE == 1
    "I_ACCEPT_FC2_FC3_SUCCESSOR_OBSERVE_ONLY_V1";
#else
    "I_ACCEPT_FC2_THREE_FRAME_DEFERRED_REGISTRATION_OBSERVE_ONLY_V1";
#endif

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
constexpr char kEntryStabilityAcknowledgement[] =
    "I_ACCEPT_FC3_ENTRY_STABILITY_ATTACH_READ_DETACH_V1";
#endif

enum RejectReason : std::uint32_t {
  kRejectUnexpectedWaitEvent = 1u << 0,
  kRejectWrongStopSignal = 1u << 1,
  kRejectDr6Read = 1u << 2,
  kRejectUnexpectedBreakpoint = 1u << 3,
  kRejectFlagsRead = 1u << 4,
  kRejectThreadName = 1u << 5,
  kRejectListRead = 1u << 6,
  kRejectDispatchState = 1u << 7,
  kRejectCarMembership = 1u << 8,
  kRejectDedicatedMembership = 1u << 9,
  kRejectCarVptr = 1u << 10,
  kRejectRegistrationEvidence = 1u << 11,
  kRejectDedicatedEvidenceHit = 1u << 12,
  kRejectRemovalEvidence = 1u << 13,
  kRejectUnexpectedWaitState = 1u << 14,
  kRejectThreadSignaled = 1u << 15,
};

struct CallbackListHeader {
  std::uintptr_t begin;
  std::uintptr_t end;
  std::uintptr_t capacity_end;
  std::uintptr_t active_end;
  std::uint8_t dispatching;
  std::uint8_t deferred;
  std::uint8_t padding[6];
  std::uint64_t member_function;
  std::uint64_t this_adjustment;
};

struct CallbackEntry {
  std::uint64_t reserved;
  std::uintptr_t object;
};

struct DebugState {
  unsigned long dr[4];
  unsigned long dr6;
  unsigned long dr7;
  bool valid;
};

struct WatchedThread {
  pid_t tid;
  bool live;
  bool stopped;
  DebugState original;
};

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
enum EntryStabilityFlags : std::uint32_t {
  kEntryTargetVerified = 1u << 0,
  kEntryStartTimeMatched = 1u << 1,
  kEntryStableSetAttached = 1u << 2,
  kEntryCleanDetach = 1u << 3,
  kEntryProcessAlive = 1u << 4,
  kEntryTracerClear = 1u << 5,
  kEntryNoDebugWrites = 1u << 6,
  kEntryNoGameWrites = 1u << 7,
};

#pragma pack(push, 1)
struct EntryStabilityReport {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t cleanup_disposition;
  std::uint64_t pid;
  std::uint64_t expected_start_time;
  std::uint64_t attach_start_ns;
  std::uint64_t attach_end_ns;
  std::uint32_t initial_threads;
  std::uint32_t final_threads;
  std::uint32_t detached_threads;
  std::uint32_t retired_threads;
  std::uint64_t ptrace_errors;
  std::uint64_t semantic_errors;
  std::uint64_t debug_write_attempts;
  std::uint64_t game_write_attempts;
};
#pragma pack(pop)

static_assert(sizeof(EntryStabilityReport) == 104,
              "FC-3 entry-stability report ABI");
#endif

#if A9TAS_FC3_TAIL_CANDIDATE == 1
constexpr std::uintptr_t kFc3ActionVectorOffset = 0x1360;
constexpr std::uintptr_t kFc3DirectModeOffset = 0x1378;

struct Fc3TailRuntime {
  a9tas::fc3_identity_v1::Layout identity{};
  a9tas::fc3_replay_observer_v1::Report report{};
  std::unique_ptr<a9tas::fc3_observer_event_core_v1::Core> core;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  std::uintptr_t final_control_owner = 0;
  std::uintptr_t c98_address = 0;
  std::uintptr_t c9c_address = 0;
  std::uintptr_t f64_address = 0;
  std::uintptr_t world_commit_address = 0;
  bool watching_world_commit = false;
  bool watching_f64 = false;
  bool watching_c9c = false;
  a9tas::fc3_phase_map_v1::Report phase_report{};
  std::unique_ptr<a9tas::fc3_phase_map_v1::Core> phase_core;
#endif
};

Fc3TailRuntime* g_fc3_tail = nullptr;
#endif

static_assert(sizeof(CallbackListHeader) == 56, "callback list ABI");
static_assert(sizeof(CallbackEntry) == 16, "callback entry ABI");
static_assert(offsetof(CallbackListHeader, dispatching) == 0x20,
              "callback flags offset");
static_assert(offsetof(PayloadEvidence, dedicated_entries) == 56,
              "dedicated evidence HWBP offset");

constexpr std::uint64_t kExpectedPrefix[11] = {
    0x17D8, 0x17A8, 0x1398, 0x1778, 0x13C8, 0x1748,
    0x1748, 0x13C8, 0x1398, 0,      0,
};
constexpr std::uint64_t kExpectedSlots[4] = {
    0x367AB34, 0x367AD84, 0x367D66C, 0x36818A0,
};

bool WriteExactVerified(int mem, std::uintptr_t address, const void* data,
                        std::size_t size) {
  if (size == 0 || pwrite(mem, data, size, static_cast<off_t>(address)) !=
                       static_cast<ssize_t>(size))
    return false;
  std::vector<std::uint8_t> verify(size);
  return ReadExact(mem, address, verify.data(), size) &&
         std::memcmp(verify.data(), data, size) == 0;
}

bool InstallShadowVptrWhileFrozen(int mem, std::uintptr_t car,
                                  std::uintptr_t original_vptr,
                                  std::uintptr_t shadow_vptr,
                                  Report* report) {
  ++report->game_write_attempts;
  const ssize_t written =
      pwrite(mem, &shadow_vptr, sizeof(shadow_vptr),
             static_cast<off_t>(car));
  std::uintptr_t observed = 0;
  const bool installed =
      written == static_cast<ssize_t>(sizeof(shadow_vptr)) &&
      ReadExact(mem, car, &observed, sizeof(observed)) &&
      observed == shadow_vptr;
  if (installed) return true;
  ++report->game_write_failures;
  ++report->rollback_attempts;
  if (!WriteExactVerified(mem, car, &original_vptr, sizeof(original_vptr)))
    ++report->rollback_failures;
  return false;
}

bool ReadCallbackList(int mem, const std::vector<Mapping>& maps,
                      std::uintptr_t list, CallbackListHeader* output) {
  CallbackListHeader header{};
  if (!ReadExact(mem, list, &header, sizeof(header)) || header.begin == 0 ||
      header.begin > header.active_end || header.active_end > header.end ||
      header.end > header.capacity_end ||
      ((header.active_end - header.begin) % sizeof(CallbackEntry)) != 0 ||
      ((header.end - header.begin) % sizeof(CallbackEntry)) != 0 ||
      header.end - header.begin > 4096 * sizeof(CallbackEntry) ||
      !FindMapping(maps, header.begin,
                   static_cast<std::size_t>(header.end - header.begin)) ||
      header.dispatching > 1 || header.deferred > 1 ||
      header.member_function != 0x10 || header.this_adjustment != 1)
    return false;
  *output = header;
  return true;
}

bool CountObject(int mem, const CallbackListHeader& header,
                 std::uintptr_t object, bool active_only,
                 std::uint32_t* output) {
  const std::uintptr_t limit = active_only ? header.active_end : header.end;
  std::uint32_t matches = 0;
  for (std::uintptr_t cursor = header.begin; cursor < limit;
       cursor += sizeof(CallbackEntry)) {
    CallbackEntry entry{};
    if (!ReadExact(mem, cursor, &entry, sizeof(entry))) return false;
    if (entry.object != object) continue;
    if (entry.reserved != 0) return false;
    ++matches;
  }
  *output = matches;
  return true;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool FindUniqueActiveObjectIndex(int mem, const CallbackListHeader& header,
                                 std::uintptr_t object,
                                 std::uint32_t* output) {
  std::uint32_t matches = 0;
  std::uint32_t found = UINT32_MAX;
  std::uint32_t index = 0;
  for (std::uintptr_t cursor = header.begin; cursor < header.active_end;
       cursor += sizeof(CallbackEntry), ++index) {
    CallbackEntry entry{};
    if (!ReadExact(mem, cursor, &entry, sizeof(entry))) return false;
    if (entry.object != object) continue;
    if (entry.reserved != 0) return false;
    found = index;
    ++matches;
  }
  if (matches != 1) return false;
  *output = found;
  return true;
}
#endif

bool ValidateIdentity(int mem, pid_t pid, std::uintptr_t base,
                      std::uintptr_t context, std::uintptr_t car,
                      std::uintptr_t dedicated, bool require_closed,
                      std::uint32_t expected_dedicated,
                      CallbackListHeader* list_out) {
  std::vector<Mapping> maps;
  std::uintptr_t adapter = 0, world = 0, context_vptr = 0, car_vptr = 0;
  if (!ReadMaps(pid, &maps) ||
      !ValidatePhysicsContext(mem, maps, base, context, &adapter, &world) ||
      !ReadExact(mem, context, &context_vptr, sizeof(context_vptr)) ||
      !ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) ||
      context_vptr != base + kPhysicsContextVtable ||
      car_vptr != base + kCarPhysicsPrimaryVtable)
    return false;
  CallbackListHeader header{};
  std::uint32_t car_count = 0, car_count_full = 0, dedicated_count = 0;
  if (!ReadCallbackList(mem, maps, context + kCallbackListOffset, &header) ||
      (require_closed && (header.dispatching != 0 || header.deferred != 0)) ||
      !CountObject(mem, header, car, true, &car_count) || car_count != 1 ||
      !CountObject(mem, header, car, false, &car_count_full) ||
      car_count_full != 1 ||
      !CountObject(mem, header, dedicated, false, &dedicated_count) ||
      dedicated_count != expected_dedicated)
    return false;
  *list_out = header;
  return true;
}

bool ResolveIdentity(pid_t pid, int mem, std::uintptr_t base,
                     std::uintptr_t dedicated, std::uintptr_t* context_out,
                     std::uintptr_t* car_out) {
  std::uintptr_t context = 0, adapter = 0, world = 0;
  if (!ResolvePhysicsContext(pid, mem, base, 0, &context, &adapter, &world))
    return false;
  a9tas::vehicle_state_v1::Layout vehicle{};
  if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle)) return false;
  CallbackListHeader header{};
  if (!ValidateIdentity(mem, pid, base, context, vehicle.physics_base,
                        dedicated, true, 0, &header))
    return false;
  *context_out = context;
  *car_out = vehicle.physics_base;
  return true;
}

bool PreparePayload(int mem, std::uintptr_t base, std::uintptr_t context,
                    std::uintptr_t car,
                    const a9tas::fc2_payload_elf_v1::Layout& payload,
                    std::uintptr_t* shadow_vptr_out) {
  std::array<std::uint8_t, kShadowSize> table{};
  if (!ReadExact(mem, base + kPrimaryPrefix, table.data(), table.size()) ||
      std::memcmp(table.data(), kExpectedPrefix, sizeof(kExpectedPrefix)) != 0)
    return false;
  for (std::size_t index = 0; index < std::size(kExpectedSlots); ++index) {
    std::uintptr_t slot = 0;
    std::memcpy(&slot, table.data() + kPrefixSize + index * sizeof(slot),
                sizeof(slot));
    if (slot != base + kExpectedSlots[index]) return false;
  }
  std::uintptr_t context_vptr = 0, add = 0, remove = 0, car_vptr = 0;
  if (!ReadExact(mem, context, &context_vptr, sizeof(context_vptr)) ||
      context_vptr != base + kPhysicsContextVtable ||
      !ReadExact(mem, context_vptr + kAddVslotOffset, &add, sizeof(add)) ||
      !ReadExact(mem, context_vptr + kRemoveVslotOffset, &remove,
                 sizeof(remove)) ||
      add != base + kPhysicsContextAdd ||
      remove != base + kPhysicsContextRemove ||
      !ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) ||
      car_vptr != base + kCarPhysicsPrimaryVtable)
    return false;

  PayloadControl initial_control{};
  PayloadEvidence initial_evidence{};
  if (!ReadExact(mem, payload.control, &initial_control,
                 sizeof(initial_control)) ||
      !ReadExact(mem, payload.evidence, &initial_evidence,
                 sizeof(initial_evidence)))
    return false;
  PayloadControl expected_control{};
  const char control_magic[8] = {'A', '9', 'F', 'C', '2', 'C', '1', 0};
  std::memcpy(expected_control.magic, control_magic, 8);
  expected_control.version = 1;
  expected_control.size = sizeof(expected_control);
  PayloadEvidence expected_evidence{};
  const char evidence_magic[8] = {'A', '9', 'F', 'C', '2', 'E', '1', 0};
  std::memcpy(expected_evidence.magic, evidence_magic, 8);
  expected_evidence.version = 1;
  expected_evidence.size = sizeof(expected_evidence);
  if (std::memcmp(&initial_control, &expected_control,
                  sizeof(initial_control)) != 0 ||
      std::memcmp(&initial_evidence, &expected_evidence,
                  sizeof(initial_evidence)) != 0)
    return false;

  const std::uintptr_t shadow_vptr = payload.shadow + kPrefixSize;
  std::memcpy(table.data() + kPrefixSize + kCallbackSlot, &payload.wrapper,
              sizeof(payload.wrapper));
  PayloadControl control = expected_control;
  control.expected_car = car;
  control.original_car_vptr = car_vptr;
  control.shadow_car_vptr = shadow_vptr;
  control.original_car_callback = base + kOriginalCallback;
  control.physics_context = context;
  control.expected_context_vptr = context_vptr;
  control.expected_add_method = add;
  control.expected_remove_method = remove;
  control.dedicated_object = payload.dedicated_object;
  control.dedicated_vptr = payload.dedicated_vptr;
  if (!WriteExactVerified(mem, payload.shadow, table.data(), table.size()) ||
      !WriteExactVerified(mem, payload.evidence, &expected_evidence,
                          sizeof(expected_evidence)) ||
      !WriteExactVerified(mem, payload.control, &control, sizeof(control)))
    return false;
  *shadow_vptr_out = shadow_vptr;
  return true;
}

bool ReadDebugState(pid_t tid, DebugState* output) {
  DebugState state{};
  bool ok = true;
  for (int index = 0; index < 4; ++index)
    ok = PeekDebug(tid, index, &state.dr[index]) && ok;
  ok = PeekDebug(tid, 6, &state.dr6) && ok;
  ok = PeekDebug(tid, 7, &state.dr7) && ok;
  state.valid = ok;
  *output = state;
  return ok;
}

bool DebugUnused(const DebugState& state) {
  return state.valid && state.dr[0] == 0 && state.dr[1] == 0 &&
         state.dr[2] == 0 && state.dr[3] == 0 && state.dr7 == 0;
}

bool RestoreDebug(pid_t tid, const DebugState& state) {
  if (!state.valid) return false;
  bool ok = PokeDebug(tid, 7, 0);
  for (int index = 0; index < 4; ++index)
    ok = PokeDebug(tid, index, state.dr[index]) && ok;
  ok = PokeDebug(tid, 6, state.dr6) && ok;
  ok = PokeDebug(tid, 7, state.dr7) && ok;
  return ok;
}

unsigned long FlagsOnlyDr7() {
  return 1UL | (1UL << 16) | (1UL << 18);
}

unsigned long FlagsAndDedicatedDr7() {
  // DR0 write/2; DR1 write/8 (LEN encoding 2).
  return FlagsOnlyDr7() | (1UL << 2) | (1UL << 20) | (2UL << 22);
}

#if A9TAS_FC3_TAIL_CANDIDATE == 1
constexpr std::uint64_t kFc3StopWaitNs =
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    750000000ULL;
#else
    100000000ULL;
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
constexpr std::uint64_t kFc3PhaseMapTransactionDeadlineNs = 3000000000ULL;
#endif

bool Fc3WaitForStopUntil(pid_t tid, std::uint64_t deadline_ns,
                         int* status_out) {
  if (!status_out) return false;
  while (MonotonicNs() < deadline_ns) {
    int status = 0;
    const pid_t waited = waitpid(tid, &status, __WALL | WNOHANG);
    if (waited == tid) {
      *status_out = status;
      return true;
    }
    if (waited < 0 && errno != EINTR) return false;
    usleep(100);
  }
  return false;
}

bool Fc3InterruptAndWaitUntil(pid_t tid, std::uint64_t deadline_ns,
                              int* status_out) {
  return ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != -1 &&
         Fc3WaitForStopUntil(tid, deadline_ns, status_out);
}

bool Fc3StopThreadUntil(pid_t tid, std::uint64_t deadline_ns) {
  int status = 0;
  if (!Fc3InterruptAndWaitUntil(tid, deadline_ns, &status) ||
      !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP ||
      (static_cast<unsigned int>(status) >> 16) != PTRACE_EVENT_STOP)
    return false;
  unsigned long dr6 = 0;
  return PeekDebug(tid, 6, &dr6) && (dr6 & 0xFu) == 0;
}

bool Fc3StopThreadBounded(pid_t tid) {
  return Fc3StopThreadUntil(tid, MonotonicNs() + kFc3StopWaitNs);
}

#define A9TAS_STOP_THREAD(tid) Fc3StopThreadBounded(tid)
#define A9TAS_STOP_THREAD_UNTIL(tid, deadline_ns) \
  Fc3StopThreadUntil(tid, deadline_ns)

bool Fc3CheckedAdd(std::uintptr_t base, std::uintptr_t offset,
                   std::uintptr_t* output) {
  if (!output || base > UINTPTR_MAX - offset) return false;
  *output = base + offset;
  return true;
}

bool Fc3ReadableWritable(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
  const Mapping* mapping = FindMapping(maps, address, size);
  return mapping && mapping->perms[0] == 'r' && mapping->perms[1] == 'w';
}

unsigned long Fc3FourWatchDr7() {
  // Local enable DR0..DR3.  Each slot is write-only.  Length encodings are
  // DR0=2 bytes, DR1=8 bytes, DR2=8 bytes, DR3=4 bytes.
  return (1UL << 0) | (1UL << 2) | (1UL << 4) | (1UL << 6) |
         (1UL << 16) | (1UL << 18) |
         (1UL << 20) | (2UL << 22) |
         (1UL << 24) | (2UL << 26) |
         (1UL << 28) | (3UL << 30);
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
unsigned long Fc3PhaseMapDr7(const Fc3TailRuntime& tail) {
  // DR0 callback flags: write/2. DR1 dedicated: write/8, then world: write/4.
  // DR2 DT: write/8, then F64: write/4. DR3 C98/C9C: write/4.
  const unsigned long dr1_len = tail.watching_world_commit ? 3UL : 2UL;
  const unsigned long dr2_len = tail.watching_f64 ? 3UL : 2UL;
  return (1UL << 0) | (1UL << 2) | (1UL << 4) | (1UL << 6) |
         (1UL << 16) | (1UL << 18) |
         (1UL << 20) | (dr1_len << 22) |
         (1UL << 24) | (dr2_len << 26) |
         (1UL << 28) | (3UL << 30);
}
#endif

bool ArmFc3WatchPlan(pid_t tid, const Fc3TailRuntime& tail,
                     bool nitro_after_dedicated) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  (void)nitro_after_dedicated;
  std::uintptr_t dr1 = 0;
  const std::uintptr_t dr2 = tail.watching_f64
      ? tail.f64_address
      : static_cast<std::uintptr_t>(tail.identity.replay_fixed_delta_input);
  const std::uintptr_t final_control =
      tail.watching_c9c ? tail.c9c_address : tail.c98_address;
  return Fc3CheckedAdd(tail.identity.payload_evidence,
                       offsetof(PayloadEvidence, dedicated_entries),
                       &dr1) &&
         (!tail.watching_world_commit ||
          (dr1 = tail.world_commit_address) != 0) &&
         (tail.identity.callback_flags & 1u) == 0 &&
         (dr1 & (tail.watching_world_commit ? 3u : 7u)) == 0 &&
         (dr2 & (tail.watching_f64 ? 3u : 7u)) == 0 &&
         (final_control & 3u) == 0 &&
         PokeDebug(tid, 7, 0) &&
         PokeDebug(tid, 0, tail.identity.callback_flags) &&
         PokeDebug(tid, 1, dr1) && PokeDebug(tid, 2, dr2) &&
         PokeDebug(tid, 3, final_control) && PokeDebug(tid, 6, 0) &&
         PokeDebug(tid, 7, Fc3PhaseMapDr7(tail));
#else
  std::uintptr_t dr1 = tail.identity.nitro_active_address;
  if (!nitro_after_dedicated &&
      !Fc3CheckedAdd(tail.identity.payload_evidence,
                     offsetof(PayloadEvidence, dedicated_entries), &dr1))
    return false;
  std::uintptr_t action_vector = 0, dr2 = 0;
  return (tail.identity.callback_flags & 1u) == 0 && (dr1 & 7u) == 0 &&
         Fc3CheckedAdd(tail.identity.action_owner, kFc3ActionVectorOffset,
                       &action_vector) &&
         Fc3CheckedAdd(action_vector, 8, &dr2) &&
         (dr2 & 7u) == 0 &&
         (tail.identity.phase_witness_accumulator & 3u) == 0 &&
         PokeDebug(tid, 7, 0) &&
         PokeDebug(tid, 0, tail.identity.callback_flags) &&
         PokeDebug(tid, 1, dr1) && PokeDebug(tid, 2, dr2) &&
         PokeDebug(tid, 3, tail.identity.phase_witness_accumulator) &&
         PokeDebug(tid, 6, 0) && PokeDebug(tid, 7, Fc3FourWatchDr7());
#endif
}

bool ArmFc3BeforeDedicated(std::vector<WatchedThread>* threads,
                           const Fc3TailRuntime& tail) {
  for (auto& thread : *threads) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    // A clean retirement proved while freezing remains in the enrolled vector
    // for exact final accounting, but it no longer owns debug registers.
    if (!thread.live) continue;
    if (!thread.stopped || !ArmFc3WatchPlan(thread.tid, tail, false)) {
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=arm_before_dedicated_failed "
                   "tid=%d stopped=%d\n",
                   thread.tid, thread.stopped);
      return false;
    }
#else
    if (!thread.live || !thread.stopped ||
        !ArmFc3WatchPlan(thread.tid, tail, false))
      return false;
#endif
  }
  return true;
}

bool RearmFc3NitroAndResumeOthers(std::vector<WatchedThread>* threads,
                                  pid_t owner,
                                  const Fc3TailRuntime& tail) {
  for (auto& thread : *threads) {
    if (!thread.live || !thread.stopped) return false;
    unsigned long dr6 = 0;
    if (!PeekDebug(thread.tid, 6, &dr6) ||
        (thread.tid != owner && (dr6 & 0xFu) != 0) ||
        !ArmFc3WatchPlan(thread.tid, tail, true))
      return false;
  }
  for (auto& thread : *threads) {
    if (!thread.live || thread.tid == owner) continue;
    if (!ContinueThread(thread.tid)) return false;
    thread.stopped = false;
  }
  return true;
}

bool AllLiveThreadsStopped(const std::vector<WatchedThread>& threads) {
  for (const auto& thread : threads)
    if (thread.live && !thread.stopped) return false;
  return true;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool ResumeAllLiveExcept(std::vector<WatchedThread>* threads,
                         pid_t stopped_tid, const char* stage) {
  if (!threads || stopped_tid <= 0 || !stage) return false;
  bool stopped_tid_found = false;
  // Validate the complete ledger before the first irreversible continue.  A
  // malformed anchor or running worker must not produce a preventable partial
  // release.  A kernel failure during the second pass can still be partial;
  // the caller's terminal rollback re-freezes every live task.
  for (const auto& thread : *threads) {
    if (!thread.live) continue;
    if (thread.tid == stopped_tid) {
      if (!thread.stopped) {
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=%s_anchor_not_stopped anchor=%d\n",
                     stage, stopped_tid);
        return false;
      }
      stopped_tid_found = true;
      continue;
    }
    if (!thread.stopped) {
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=%s_worker_not_stopped "
                   "tid=%d anchor=%d\n",
                   stage, thread.tid, stopped_tid);
      return false;
    }
  }
  if (!stopped_tid_found) {
    std::fprintf(stderr,
                 "FC3_PHASE_DIAG stage=%s_anchor_missing anchor=%d\n",
                 stage, stopped_tid);
    return false;
  }
  for (auto& thread : *threads) {
    if (!thread.live || thread.tid == stopped_tid) continue;
    if (!ContinueThread(thread.tid)) {
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=%s_resume_others_failed "
                   "tid=%d anchor=%d stopped=%d\n",
                   stage, thread.tid, stopped_tid, thread.stopped);
      return false;
    }
    thread.stopped = false;
  }
  return true;
}
#endif

bool Fc3ThreadSetMatches(pid_t pid,
                         const std::vector<WatchedThread>& threads) {
  const std::vector<pid_t> current = ListThreads(pid);
  std::size_t live = 0;
  for (const auto& thread : threads)
    if (thread.live) ++live;
  if (current.size() != live) return false;
  for (const pid_t tid : current) {
    bool found = false;
    for (const auto& thread : threads) {
      if (thread.live && thread.tid == tid) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

bool Fc3FinalSnapshotBarrier(pid_t pid, pid_t owner,
                              std::uint64_t deadline_ns,
                              std::vector<WatchedThread>* threads) {
  WatchedThread* owner_thread = nullptr;
  for (auto& thread : *threads) {
    if (thread.tid == owner) {
      owner_thread = &thread;
      break;
    }
  }
  if (!owner_thread || !owner_thread->live || !owner_thread->stopped ||
      !Fc3ThreadSetMatches(pid, *threads))
    return false;
  for (auto& thread : *threads) {
    if (!thread.live || thread.tid == owner) continue;
    int status = 0;
    if (thread.stopped ||
        !Fc3InterruptAndWaitUntil(thread.tid, deadline_ns, &status))
      return false;
    thread.stopped = WIFSTOPPED(status);
    const unsigned int event = static_cast<unsigned int>(status) >> 16;
    unsigned long dr6 = 0;
    if (!thread.stopped || WSTOPSIG(status) != SIGTRAP ||
        event != PTRACE_EVENT_STOP || !PeekDebug(thread.tid, 6, &dr6) ||
        (dr6 & 0xFu) != 0)
      return false;
  }
  unsigned long owner_dr6 = 0;
  return AllLiveThreadsStopped(*threads) && Fc3ThreadSetMatches(pid, *threads) &&
         PeekDebug(owner, 6, &owner_dr6) && (owner_dr6 & 0xFu) == 0x1u;
}

bool BuildFc3Stop(int mem, pid_t tid, unsigned long dr6,
                  const Fc3TailRuntime& tail,
                  a9tas::fc3_observer_event_core_v1::Stop* output) {
  using a9tas::fc3_replay_observer_v1::Snapshot;
  Snapshot snapshot{};
  snapshot.monotonic_ns = MonotonicNs();
  snapshot.tid = tid;
  std::uintptr_t begin = 0, end = 0, capacity = 0;
  std::uintptr_t action_vector = 0, action_vector_end = 0;
  std::uintptr_t action_vector_capacity = 0, direct_mode = 0;
  std::uint64_t dedicated_entries = 0;
  PayloadEvidence evidence{};
  std::vector<Mapping> maps;
  if (!Fc3CheckedAdd(tail.identity.action_owner, kFc3ActionVectorOffset,
                     &action_vector) ||
      !Fc3CheckedAdd(action_vector, 8, &action_vector_end) ||
      !Fc3CheckedAdd(action_vector, 16, &action_vector_capacity) ||
      !Fc3CheckedAdd(tail.identity.action_owner, kFc3DirectModeOffset,
                     &direct_mode) ||
      !ReadMaps(static_cast<pid_t>(tail.identity.pid), &maps) ||
      !Fc3ReadableWritable(maps, tail.identity.callback_flags,
                           sizeof(snapshot.callback_flags)) ||
      !Fc3ReadableWritable(maps, direct_mode,
                           sizeof(snapshot.direct_mode)) ||
      !Fc3ReadableWritable(maps, tail.identity.nitro_active_address,
                           sizeof(snapshot.nitro_active)) ||
      !Fc3ReadableWritable(maps, tail.identity.nitro_mode_address,
                           sizeof(snapshot.nitro_mode)) ||
      !Fc3ReadableWritable(maps, tail.identity.phase_witness_accumulator,
                           sizeof(snapshot.accumulator_bits)) ||
      !Fc3ReadableWritable(maps, action_vector, 3 * sizeof(std::uintptr_t)) ||
      !Fc3ReadableWritable(maps, tail.identity.payload_evidence,
                           sizeof(evidence)) ||
      !Fc3ReadableWritable(maps, tail.identity.callback_list,
                           sizeof(CallbackListHeader)) ||
      !ReadExact(mem, tail.identity.callback_flags, &snapshot.callback_flags,
                  sizeof(snapshot.callback_flags)) ||
      !ReadExact(mem, direct_mode,
                  &snapshot.direct_mode, sizeof(snapshot.direct_mode)) ||
      !ReadExact(mem, tail.identity.nitro_active_address,
                 &snapshot.nitro_active, sizeof(snapshot.nitro_active)) ||
      !ReadExact(mem, tail.identity.nitro_mode_address, &snapshot.nitro_mode,
                 sizeof(snapshot.nitro_mode)) ||
      !ReadExact(mem, tail.identity.phase_witness_accumulator,
                 &snapshot.accumulator_bits,
                 sizeof(snapshot.accumulator_bits)) ||
      !ReadExact(mem, action_vector,
                  &begin, sizeof(begin)) ||
      !ReadExact(mem, action_vector_end,
                  &end, sizeof(end)) ||
      !ReadExact(mem, action_vector_capacity,
                  &capacity, sizeof(capacity)) ||
      !ReadExact(mem, tail.identity.payload_evidence, &evidence,
                 sizeof(evidence)))
    return false;
  dedicated_entries = evidence.dedicated_entries;
  snapshot.action_queue_begin = begin;
  snapshot.action_queue_end = end;
  snapshot.action_queue_capacity = capacity;
  snapshot.action_queue_count =
      begin <= end && ((end - begin) & 7u) == 0 ? (end - begin) / 8u
                                                : UINT64_MAX;
  snapshot.dedicated_entries = dedicated_entries;

  CallbackListHeader header{};
  std::uint32_t dedicated_full = UINT32_MAX;
  if (!ReadCallbackList(mem, maps, tail.identity.callback_list, &header) ||
      !CountObject(mem, header, tail.identity.dedicated_object, false,
                   &dedicated_full))
    return false;

  output->dr6 = static_cast<std::uint32_t>(dr6 & 0xFu);
  output->snapshot = snapshot;
  output->dedicated_members_full = dedicated_full;
  output->payload_failures =
      evidence.failures == 0 && evidence.recursive_entries == 0 ? 0u : 1u;
  return true;
}

bool Fc3TailIdentityMatchesFc2(const Fc3TailRuntime& tail,
                               const Report& fc2) {
  return tail.identity.pid == fc2.pid &&
         tail.identity.library_base == fc2.library_base &&
         tail.identity.physics_context == fc2.physics_context &&
         tail.identity.callback_list == fc2.callback_list &&
         tail.identity.callback_flags == fc2.callback_flags &&
         tail.identity.car_physics_state == fc2.car_physics_state &&
         tail.identity.payload_evidence == fc2.payload_evidence &&
         tail.identity.dedicated_object == fc2.dedicated_object;
}

bool InitializeFc3Report(Fc3TailRuntime* tail) {
  const char magic[8] = {'A', '9', 'F', 'C', '3', 'R', '1', 0};
  std::memcpy(tail->report.magic, magic, sizeof(magic));
  tail->report.version = 1;
  tail->report.size = sizeof(tail->report);
  tail->report.flags = a9tas::fc3_replay_observer_v1::kTargetVerified |
                       a9tas::fc3_replay_observer_v1::kIdentityPinned |
                       a9tas::fc3_replay_observer_v1::kPayloadPrepared;
  tail->report.final_phase = static_cast<std::uint32_t>(
      a9tas::fc3_replay_observer_v1::Phase::kPreflight);
  tail->report.pid = tail->identity.pid;
  tail->report.library_base = tail->identity.library_base;
  tail->report.physics_context = tail->identity.physics_context;
  tail->report.callback_flags = tail->identity.callback_flags;
  tail->report.callback_list = tail->identity.callback_list;
  tail->report.car_physics_state = tail->identity.car_physics_state;
  tail->report.payload_evidence = tail->identity.payload_evidence;
  tail->report.dedicated_object = tail->identity.dedicated_object;
  tail->report.action_owner = tail->identity.action_owner;
  std::uintptr_t action_vector = 0;
  if (!Fc3CheckedAdd(tail->identity.action_owner, kFc3ActionVectorOffset,
                     &action_vector) ||
      !Fc3CheckedAdd(action_vector, 8, &tail->report.action_queue_end))
    return false;
  tail->report.nitro_state = tail->identity.nitro_state;
  tail->report.nitro_active_address = tail->identity.nitro_active_address;
  tail->report.nitro_mode_address = tail->identity.nitro_mode_address;
  tail->report.physics_backend_world = tail->identity.physics_backend_world;
  tail->report.phase_witness_accumulator =
      tail->identity.phase_witness_accumulator;
  return true;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool InitializeFc3PhaseMap(Fc3TailRuntime* tail) {
  std::vector<Mapping> maps;
  if (!tail || tail->final_control_owner == 0 ||
      !Fc3CheckedAdd(tail->final_control_owner, kC98Offset,
                     &tail->c98_address) ||
      !Fc3CheckedAdd(tail->final_control_owner, kC9COffset,
                     &tail->c9c_address) ||
      !Fc3CheckedAdd(tail->identity.car_physics_state,
                     kCarPhysicsF64Offset, &tail->f64_address) ||
      !Fc3CheckedAdd(tail->identity.physics_backend_world,
                     kWorldAccumulatorOffset, &tail->world_commit_address) ||
      tail->c9c_address != tail->c98_address + 4 ||
      (tail->c98_address & 3u) != 0 || (tail->f64_address & 3u) != 0 ||
      (tail->world_commit_address & 3u) != 0 ||
      !ReadMaps(static_cast<pid_t>(tail->identity.pid), &maps) ||
      !Fc3ReadableWritable(maps, tail->identity.replay_fixed_delta_input, 8) ||
      !Fc3ReadableWritable(maps, tail->c98_address, 8) ||
      !Fc3ReadableWritable(maps, tail->f64_address, 4) ||
      !Fc3ReadableWritable(maps, tail->world_commit_address, 4))
    return false;
  auto& phase = tail->phase_report;
  const char magic[8] = {'A', '9', 'F', 'C', '3', 'P', '1', 0};
  std::memcpy(phase.magic, magic, sizeof(magic));
  phase.version = 1;
  phase.size = sizeof(phase);
  phase.flags = a9tas::fc3_phase_map_v1::kTargetVerified |
                a9tas::fc3_phase_map_v1::kIdentityPinned;
  phase.final_phase = static_cast<std::uint32_t>(
      a9tas::fc3_phase_map_v1::Phase::kPreflight);
  phase.pid = tail->identity.pid;
  phase.library_base = tail->identity.library_base;
  phase.main_time_source_owner = tail->identity.main_time_source_owner;
  phase.final_control_owner = tail->final_control_owner;
  phase.fixed_delta_address = tail->identity.replay_fixed_delta_input;
  phase.c98_address = tail->c98_address;
  phase.c9c_address = tail->c9c_address;
  phase.callback_flags_address = tail->identity.callback_flags;
  phase.callback_list_address = tail->identity.callback_list;
  phase.car_physics_state = tail->identity.car_physics_state;
  phase.dedicated_object = tail->identity.dedicated_object;
  phase.f64_address = tail->f64_address;
  phase.world_commit_address = tail->world_commit_address;
  return true;
}
#endif
#else
#define A9TAS_STOP_THREAD(tid) StopThread(tid)
#define A9TAS_STOP_THREAD_UNTIL(tid, deadline_ns) StopThread(tid)
#endif

WatchedThread* FindWatched(std::vector<WatchedThread>* threads, pid_t tid) {
  for (auto& thread : *threads)
    if (thread.tid == tid) return &thread;
  return nullptr;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool Fc3TaskIsGone(pid_t pid, pid_t tid) {
  char task_path[96]{};
  const int path_size = std::snprintf(task_path, sizeof(task_path),
                                      "/proc/%d/task/%d", pid, tid);
  if (path_size <= 0 ||
      static_cast<std::size_t>(path_size) >= sizeof(task_path))
    return false;
  errno = 0;
  return access(task_path, F_OK) == -1 && errno == ENOENT;
}

enum class Fc3FreezeStopResult : std::uint8_t {
  kStopped,
  kCleanRetired,
  kFailed,
};

Fc3FreezeStopResult Fc3FreezeThreadUntil(pid_t pid, pid_t tid,
                                         std::uint64_t deadline_ns,
                                         int* status_out) {
  if (status_out) *status_out = -1;
  if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1) {
    return Fc3FreezeStopResult::kFailed;
  }
  int status = 0;
  if (!Fc3WaitForStopUntil(tid, deadline_ns, &status)) {
    return Fc3FreezeStopResult::kFailed;
  }
  if (status_out) *status_out = status;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      Fc3TaskIsGone(pid, tid)) {
    return Fc3FreezeStopResult::kCleanRetired;
  }
  if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP ||
      (static_cast<unsigned int>(status) >> 16) != PTRACE_EVENT_STOP) {
    return Fc3FreezeStopResult::kFailed;
  }
  unsigned long dr6 = 0;
  if (!PeekDebug(tid, 6, &dr6) || (dr6 & 0xFu) != 0) {
    return Fc3FreezeStopResult::kFailed;
  }
  return Fc3FreezeStopResult::kStopped;
}
#endif

bool AttachOne(pid_t tid, std::uintptr_t flags_address, bool run,
               WatchedThread* output
#if A9TAS_FC3_TAIL_CANDIDATE == 1
               , std::uint64_t stop_deadline_ns
#endif
               ) {
  WatchedThread thread{tid, true, false, {}};
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  // TRACECLONE closes the otherwise undetectable spawn+exit gap between
  // userspace thread-list scans. EXITKILL is the final fresh-process safety
  // boundary if a newly auto-traced child cannot be incorporated into normal
  // cleanup after lifecycle drift has already rejected the transaction.
  void* seize_options = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
      PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL));
#else
  void* seize_options = nullptr;
#endif
  if (ptrace(PTRACE_SEIZE, tid, nullptr, seize_options) == -1) return false;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  if (!A9TAS_STOP_THREAD_UNTIL(tid, stop_deadline_ns)) {
#else
  if (!A9TAS_STOP_THREAD(tid)) {
#endif
    (void)ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    return false;
  }
  thread.stopped = true;
  const bool arm_flags = flags_address != 0;
  if (!ReadDebugState(tid, &thread.original) ||
      !DebugUnused(thread.original) ||
      (arm_flags &&
       (!PokeDebug(tid, 0, flags_address) || !PokeDebug(tid, 6, 0) ||
        !PokeDebug(tid, 7, FlagsOnlyDr7()))) ||
      (run && !ContinueThread(tid))) {
    if (arm_flags) (void)RestoreDebug(tid, thread.original);
    (void)ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    return false;
  }
  thread.stopped = !run;
  *output = thread;
  return true;
}

bool AttachCurrentThreads(pid_t pid, std::uintptr_t flags_address, bool run,
                           std::vector<WatchedThread>* threads
#if A9TAS_FC3_TAIL_CANDIDATE == 1
                           , std::uint64_t stop_deadline_ns = 0
#endif
                           ) {
  bool ok = true;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  if (stop_deadline_ns == 0)
    stop_deadline_ns = MonotonicNs() + kFc3StopWaitNs;
#endif
  for (const pid_t tid : ListThreads(pid)) {
    if (FindWatched(threads, tid)) continue;
    WatchedThread thread{};
    if (!AttachOne(tid, flags_address, run, &thread
#if A9TAS_FC3_TAIL_CANDIDATE == 1
                   , stop_deadline_ns
#endif
                   )) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      const int attach_errno = errno;
      // A task can disappear after the one-shot /proc task listing but before
      // PTRACE_SEIZE (or while its initial interrupt is being collected).  It
      // was never part of the completed watch-set baseline, so a confirmed
      // ENOENT is lifecycle churn rather than a transport failure.  Any task
      // that still exists remains a hard failure.
      if (Fc3TaskIsGone(pid, tid)) {
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=initial_attach_retired tid=%d "
                     "errno=%d watched=%zu\n",
                     tid, attach_errno, threads->size());
        continue;
      }
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=attach_current_failed tid=%d "
                   "errno=%d watched=%zu\n",
                   tid, attach_errno, threads->size());
#endif
      ok = false;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
      return false;
#else
      continue;
#endif
    }
    threads->push_back(thread);
  }
  return ok;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool AttachStableInitialThreadSet(pid_t pid, std::uintptr_t flags_address,
                                  bool resume,
                                  std::vector<WatchedThread>* threads) {
  if (!threads || !threads->empty()) return false;
  const std::uint64_t stop_deadline = MonotonicNs() + kFc3StopWaitNs;
  for (int pass = 0; pass < 5 && MonotonicNs() < stop_deadline; ++pass) {
    const std::size_t before = threads->size();
    // Keep every enrolled task stopped.  Once a parent is stopped it cannot
    // create more children, so repeated task-list passes converge instead of
    // racing a fully running 250+ thread process.
    if (!AttachCurrentThreads(pid, flags_address, false, threads, stop_deadline)) {
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=initial_stable_attach_failed "
                   "pass=%d watched=%zu errno=%d\n",
                   pass, threads->size(), errno);
      return false;
    }
    if (threads->size() == before && Fc3ThreadSetMatches(pid, *threads)) {
      if (!resume) {
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=initial_stable_attached_stopped "
                     "passes=%d watched=%zu\n",
                     pass + 1, threads->size());
        return true;
      }
      for (auto& thread : *threads) {
        if (!thread.live || !thread.stopped ||
            !ContinueThread(thread.tid)) {
          std::fprintf(stderr,
                       "FC3_PHASE_DIAG stage=initial_stable_resume_failed "
                       "pass=%d tid=%d watched=%zu errno=%d\n",
                       pass, thread.tid, threads->size(), errno);
          return false;
        }
        thread.stopped = false;
      }
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=initial_stable_attached "
                   "passes=%d watched=%zu\n",
                   pass + 1, threads->size());
      return true;
    }
  }
  std::fprintf(stderr,
               "FC3_PHASE_DIAG stage=initial_stable_not_converged "
               "watched=%zu\n",
               threads->size());
  return false;
}
#endif

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
struct PendingCloneChild {
  pid_t tid;
  int status;
};

bool WaitForAutoTracedChild(pid_t child_tid, std::uint64_t deadline_ns,
                            int* status_out) {
  if (!status_out) return false;
  while (MonotonicNs() < deadline_ns) {
    int status = 0;
    const pid_t waited = waitpid(child_tid, &status, __WALL | WNOHANG);
    if (waited == child_tid) {
      *status_out = status;
      return true;
    }
    if (waited < 0 && errno != EINTR) return false;
    usleep(100);
  }
  return false;
}

bool IncorporateAutoTracedClone(pid_t parent_tid, int parent_status,
                                bool phase_watch_plan,
                                std::uintptr_t flags_address,
                                std::vector<WatchedThread>* threads,
                                std::vector<PendingCloneChild>* pending_children,
                                std::vector<pid_t>* preowner_retired_candidates,
                                Fc3TailRuntime* tail,
                                pid_t* child_tid_out,
                                bool* child_retired_out,
                                bool* child_already_counted_out) {
  if (!threads || !pending_children || !preowner_retired_candidates || !tail ||
      !child_tid_out || !child_retired_out || !child_already_counted_out ||
      (static_cast<unsigned int>(parent_status) >> 16) !=
          PTRACE_EVENT_CLONE)
    return false;
  WatchedThread* parent = FindWatched(threads, parent_tid);
  if (!parent || !parent->live || !WIFSTOPPED(parent_status) ||
      WSTOPSIG(parent_status) != SIGTRAP)
    return false;
  parent->stopped = true;
  const DebugState clean_state = parent->original;

  unsigned long child_message = 0;
  if (ptrace(PTRACE_GETEVENTMSG, parent_tid, nullptr, &child_message) == -1 ||
      child_message == 0 || child_message > static_cast<unsigned long>(INT_MAX))
    return false;
  const pid_t child_tid = static_cast<pid_t>(child_message);
  if (FindWatched(threads, child_tid)) return false;
  *child_retired_out = false;
  *child_already_counted_out = false;

  for (std::size_t index = 0;
       index < preowner_retired_candidates->size(); ++index) {
    if ((*preowner_retired_candidates)[index] != child_tid) continue;
    preowner_retired_candidates->erase(
        preowner_retired_candidates->begin() + index);
    if (!ContinueThread(parent_tid)) return false;
    parent = FindWatched(threads, parent_tid);
    if (!parent) return false;
    parent->stopped = false;
    *child_tid_out = child_tid;
    *child_retired_out = true;
    *child_already_counted_out = true;
    return true;
  }

  int child_status = 0;
  bool pending_stop_found = false;
  for (std::size_t index = 0; index < pending_children->size(); ++index) {
    if ((*pending_children)[index].tid != child_tid) continue;
    child_status = (*pending_children)[index].status;
    pending_children->erase(pending_children->begin() + index);
    pending_stop_found = true;
    break;
  }
  if ((!pending_stop_found &&
       !WaitForAutoTracedChild(child_tid,
                               MonotonicNs() + kFc3StopWaitNs,
                               &child_status)))
    return false;
  if (WIFEXITED(child_status)) {
    if (WEXITSTATUS(child_status) != 0 || !ContinueThread(parent_tid))
      return false;
    parent = FindWatched(threads, parent_tid);
    if (!parent) return false;
    parent->stopped = false;
    *child_tid_out = child_tid;
    *child_retired_out = true;
    return true;
  }
  if (!WIFSTOPPED(child_status)) return false;

  // TRACECLONE inherits the tracer relationship and may inherit the parent's
  // active debug state.  Cleanup must restore the parent's pre-transaction
  // state, not treat inherited watchpoints as the child's original state.
  threads->push_back(WatchedThread{child_tid, true, true, clean_state});
  const bool armed = phase_watch_plan
      ? ArmFc3WatchPlan(child_tid, *tail, tail->watching_c9c)
      : (PokeDebug(child_tid, 7, 0) &&
         PokeDebug(child_tid, 0, flags_address) &&
         PokeDebug(child_tid, 6, 0) &&
         PokeDebug(child_tid, 7, FlagsOnlyDr7()));
  if (!armed || !ContinueThread(child_tid)) return false;
  WatchedThread* child = FindWatched(threads, child_tid);
  if (!child) return false;
  child->stopped = false;

  // push_back may reallocate the vector, so reacquire the parent pointer.
  parent = FindWatched(threads, parent_tid);
  if (!parent || !ContinueThread(parent_tid)) return false;
  parent->stopped = false;
  *child_tid_out = child_tid;
  return true;
}
#endif

bool FreezeAllExcept(pid_t pid, pid_t owner, std::uintptr_t flags_address,
                      std::vector<WatchedThread>* threads
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
                      , std::uint32_t* retired_threads
#endif
                      ) {
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  const std::uint64_t stop_deadline = MonotonicNs() + kFc3StopWaitNs;
#endif
  for (auto& thread : *threads) {
    if (!thread.live || thread.tid == owner || thread.stopped) continue;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    int freeze_status = -1;
    const Fc3FreezeStopResult freeze_result = Fc3FreezeThreadUntil(
        pid, thread.tid, stop_deadline, &freeze_status);
    if (freeze_result == Fc3FreezeStopResult::kCleanRetired) {
      if (!retired_threads) return false;
      thread.live = false;
      thread.stopped = false;
      ++*retired_threads;
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=freeze_existing_clean_retired "
                   "tid=%d owner=%d status=0x%x retired=%u threads=%zu\n",
                   thread.tid, owner, freeze_status, *retired_threads,
                   threads->size());
      continue;
    }
    if (freeze_result != Fc3FreezeStopResult::kStopped) {
#else
    if (!A9TAS_STOP_THREAD_UNTIL(thread.tid, stop_deadline)) {
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=freeze_existing tid=%d owner=%d "
                   "status=0x%x threads=%zu now=%" PRIu64
                   " deadline=%" PRIu64 "\n",
                   thread.tid, owner, freeze_status, threads->size(), MonotonicNs(),
                   stop_deadline);
#endif
      return false;
    }
    thread.stopped = true;
  }
  for (int pass = 0; pass < 3; ++pass) {
    const std::size_t before = threads->size();
    if (!AttachCurrentThreads(pid, flags_address, false, threads
#if A9TAS_FC3_TAIL_CANDIDATE == 1
                              , stop_deadline
#endif
                              )) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=freeze_attach_new owner=%d "
                   "pass=%d threads=%zu now=%" PRIu64 " deadline=%" PRIu64
                   " errno=%d\n",
                   owner, pass, threads->size(), MonotonicNs(), stop_deadline,
                   errno);
#endif
      return false;
    }
    for (auto& thread : *threads) {
      if (!thread.live || thread.tid == owner || thread.stopped) continue;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      int freeze_status = -1;
      const Fc3FreezeStopResult freeze_result = Fc3FreezeThreadUntil(
          pid, thread.tid, stop_deadline, &freeze_status);
      if (freeze_result == Fc3FreezeStopResult::kCleanRetired) {
        if (!retired_threads) return false;
        thread.live = false;
        thread.stopped = false;
        ++*retired_threads;
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=freeze_new_clean_retired "
                     "tid=%d owner=%d status=0x%x retired=%u pass=%d "
                     "threads=%zu\n",
                     thread.tid, owner, freeze_status, *retired_threads, pass,
                     threads->size());
        continue;
      }
      if (freeze_result != Fc3FreezeStopResult::kStopped) {
#else
      if (!A9TAS_STOP_THREAD_UNTIL(thread.tid, stop_deadline)) {
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=freeze_new tid=%d owner=%d "
                     "status=0x%x pass=%d threads=%zu now=%" PRIu64
                     " deadline=%" PRIu64 "\n",
                     thread.tid, owner, freeze_status, pass, threads->size(),
                     MonotonicNs(), stop_deadline);
#endif
        return false;
      }
      thread.stopped = true;
    }
    if (threads->size() == before) return true;
  }
  return false;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool RearmFc3PhaseMapAndResumeOthers(
    pid_t pid, pid_t stopped_tid, bool watch_world_commit, bool watch_f64,
    bool watch_c9c,
    std::vector<WatchedThread>* threads, std::uint32_t* retired_threads,
    Fc3TailRuntime* tail, Report* report) {
  if (!tail || !threads || !report) return false;
  const std::size_t enrolled_before = threads->size();
  const bool frozen = FreezeAllExcept(
      pid, stopped_tid, tail->identity.callback_flags, threads,
      retired_threads);
  const std::size_t newly_enrolled = threads->size() - enrolled_before;
  if (newly_enrolled > UINT32_MAX - report->initial_threads ||
      newly_enrolled > UINT32_MAX - tail->phase_report.initial_threads)
    return false;
  report->initial_threads += static_cast<std::uint32_t>(newly_enrolled);
  tail->phase_report.initial_threads +=
      static_cast<std::uint32_t>(newly_enrolled);
  if (!frozen) return false;
  tail->watching_world_commit = watch_world_commit;
  tail->watching_f64 = watch_f64;
  tail->watching_c9c = watch_c9c;
  for (auto& thread : *threads) {
    if (!thread.live) continue;
    if (!thread.stopped ||
        !ArmFc3WatchPlan(thread.tid, *tail, watch_c9c)) {
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=rearm_phase_map_failed "
                   "tid=%d stopped=%d world=%d f64=%d c9c=%d\n",
                   thread.tid, thread.stopped, watch_world_commit, watch_f64,
                   watch_c9c);
      return false;
    }
  }
  return ResumeAllLiveExcept(threads, stopped_tid, "rearm_phase_map");
}
#endif

bool ArmDedicatedWatch(pid_t tid, std::uintptr_t dedicated_entries_address) {
  return PokeDebug(tid, 1, dedicated_entries_address) &&
         PokeDebug(tid, 6, 0) && PokeDebug(tid, 7, FlagsAndDedicatedDr7());
}

bool ReadThreadName(pid_t pid, pid_t tid, std::string* output) {
  char path[96]{};
  std::snprintf(path, sizeof(path), "/proc/%d/task/%d/comm",
                static_cast<int>(pid), static_cast<int>(tid));
  FILE* file = std::fopen(path, "re");
  if (!file) return false;
  char name[128]{};
  const bool ok = std::fgets(name, sizeof(name), file) != nullptr;
  std::fclose(file);
  if (!ok) return false;
  name[std::strcspn(name, "\r\n")] = 0;
  *output = name;
  return !output->empty();
}

bool RestoreAndDetachAll(std::vector<WatchedThread>* threads,
                         std::uint32_t* detached
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
                         , pid_t pid, std::uint32_t* retired_threads
#endif
                         ) {
  bool ok = true;
  *detached = 0;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  const std::uint64_t stop_deadline = MonotonicNs() + kFc3StopWaitNs;
#endif
  for (auto& thread : *threads) {
    if (!thread.live) continue;
    if (!thread.stopped &&
        !A9TAS_STOP_THREAD_UNTIL(thread.tid, stop_deadline)) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      if (retired_threads && Fc3TaskIsGone(pid, thread.tid)) {
        thread.live = false;
        thread.stopped = false;
        ++*retired_threads;
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=cleanup_tracked_retired tid=%d "
                     "retired=%u\n",
                     thread.tid, *retired_threads);
        continue;
      }
#endif
      ok = false;
      continue;
    }
    thread.stopped = true;
    const bool restored = RestoreDebug(thread.tid, thread.original);
    const bool detached_ok =
        ptrace(PTRACE_DETACH, thread.tid, nullptr, nullptr) != -1;
    ok = restored && detached_ok && ok;
    if (restored && detached_ok) ++*detached;
    thread.live = false;
    thread.stopped = false;
  }
  return ok;
}

bool RegistrationEvidenceBase(const PayloadEvidence& evidence,
                              const Report& report) {
  const char magic[8] = {'A', '9', 'F', 'C', '2', 'E', '1', 0};
  const bool before_shape =
      evidence.list_begin_before != 0 &&
      evidence.list_begin_before <= evidence.list_active_before &&
      evidence.list_active_before <= evidence.list_end_before;
  const bool after_shape =
      evidence.list_begin_after_add != 0 &&
      evidence.list_begin_after_add <= evidence.list_active_after_add &&
      evidence.list_active_after_add <= evidence.list_end_after_add;
  const bool deferred_delta = before_shape && after_shape &&
      evidence.list_end_after_add - evidence.list_begin_after_add ==
          evidence.list_end_before - evidence.list_begin_before + 16 &&
      evidence.list_active_after_add - evidence.list_begin_after_add ==
          evidence.list_active_before - evidence.list_begin_before;
  return std::memcmp(evidence.magic, magic, 8) == 0 &&
         evidence.version == 1 && evidence.size == sizeof(evidence) &&
         evidence.bootstrap_entries == 1 && evidence.original_calls == 1 &&
         evidence.original_returns == 1 &&
         evidence.registration_attempts == 1 &&
         evidence.registration_returns == 1 &&
         evidence.recursive_entries == 0 &&
         evidence.failures == 0 && evidence.last_car == report.car_physics_state &&
         evidence.last_bootstrap_token != 0 &&
         evidence.observed_car_vptr == report.shadow_vptr &&
         evidence.restored_car_vptr == report.original_vptr &&
         evidence.observed_context_vptr ==
             report.library_base + kPhysicsContextVtable &&
         evidence.observed_add_method ==
             report.library_base + kPhysicsContextAdd &&
         evidence.observed_remove_method ==
             report.library_base + kPhysicsContextRemove &&
         evidence.registration_dispatching_before == 1 &&
         evidence.registration_deferred_before == 0 &&
         evidence.registration_dispatching_after == 1 &&
         evidence.registration_deferred_after == 1 && deferred_delta;
}

bool RegistrationEvidenceComplete(const PayloadEvidence& evidence,
                                  const Report& report) {
  return RegistrationEvidenceBase(evidence, report) &&
         evidence.dedicated_entries == 0 && evidence.removal_attempts == 0 &&
         evidence.removal_returns == 0 && evidence.last_status == 1 &&
         evidence.protocol_state == 1;
}

bool RemovalEvidenceComplete(const PayloadEvidence& evidence,
                             const Report& report) {
  return RegistrationEvidenceBase(evidence, report) &&
         evidence.dedicated_entries == 1 &&
         evidence.removal_attempts == 1 && evidence.removal_returns == 1 &&
         evidence.last_dedicated_object == report.dedicated_object &&
         evidence.last_dedicated_token != 0 &&
         evidence.list_begin_after_remove != 0 &&
         evidence.list_begin_after_remove <= evidence.list_end_after_remove &&
         evidence.removal_dispatching_before == 1 &&
         evidence.removal_deferred_before == 0 &&
         evidence.removal_dispatching_after == 1 &&
         evidence.removal_deferred_after == 1 &&
         evidence.last_status == 2 && evidence.protocol_state == 2;
}

bool ConditionalRollback(int mem, std::uintptr_t car,
                         std::uintptr_t shadow_vptr,
                         std::uintptr_t original_vptr, Report* report) {
  std::uintptr_t current = 0;
  if (!ReadExact(mem, car, &current, sizeof(current))) {
    ++report->read_errors;
    return false;
  }
  if (current == original_vptr) return true;
  if (current != shadow_vptr) {
    ++report->semantic_errors;
    return false;
  }
  ++report->rollback_attempts;
  if (!WriteExactVerified(mem, car, &original_vptr, sizeof(original_vptr))) {
    ++report->rollback_failures;
    return false;
  }
  return true;
}

#if A9TAS_FC3_TAIL_CANDIDATE == 1
bool WriteReportExclusive(const char* path, const void* data,
                          std::size_t size) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                               O_NOFOLLOW,
                      0600);
  if (fd < 0) return false;
  struct stat state{};
  if (fstat(fd, &state) != 0 || !S_ISREG(state.st_mode) ||
      state.st_nlink != 1) {
    close(fd);
    std::remove(path);
    return false;
  }
  FILE* file = fdopen(fd, "wb");
  if (!file) {
    close(fd);
    std::remove(path);
    return false;
  }
  const bool ok = std::fwrite(data, size, 1, file) == 1 &&
                  std::fflush(file) == 0 && fsync(fileno(file)) == 0 &&
                  std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    std::remove(path);
    return false;
  }
  return true;
}
#endif

bool WriteReport(const char* path, const Report& report) {
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  return WriteReportExclusive(path, &report, sizeof(report));
#else
  FILE* file = std::fopen(path, "wb");
  if (!file) return false;
  const bool ok = std::fwrite(&report, sizeof(report), 1, file) == 1 &&
                  std::fflush(file) == 0 && std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    std::remove(path);
    return false;
  }
  return true;
#endif
}

#if A9TAS_FC3_TAIL_CANDIDATE == 1
bool WriteReport(const char* path,
                 const a9tas::fc3_replay_observer_v1::Report& report) {
  return WriteReportExclusive(path, &report, sizeof(report));
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool WriteReport(const char* path,
                 const a9tas::fc3_phase_map_v1::Report& report) {
  const auto exclusive_writer = &WriteReportExclusive;
  return exclusive_writer(path, &report, sizeof(report));
}
#endif

bool Fc3ParseProcessStatStartTime(char* line, std::uint64_t* output) {
  if (!line || !output) return false;
  char* cursor = std::strrchr(line, ')');
  if (!cursor) return false;
  ++cursor;
  while (*cursor == ' ') ++cursor;
  if (*cursor == 0) return false;
  ++cursor;  // field 3: state
  for (int field = 4; field < 22; ++field) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == 0) return false;
    while (*cursor != 0 && *cursor != ' ') ++cursor;
  }
  while (*cursor == ' ') ++cursor;
  if (*cursor < '0' || *cursor > '9') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(cursor, &end, 10);
  if (errno != 0 || end == cursor || (*end != 0 && *end != '\n' &&
                                      *end != ' '))
    return false;
  *output = static_cast<std::uint64_t>(value);
  return true;
}

bool Fc3ReadProcessStartTime(pid_t pid, std::uint64_t* output) {
  if (!output) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (!file) return false;
  char line[4096]{};
  const bool read_ok = std::fgets(line, sizeof(line), file) != nullptr;
  const bool close_ok = std::fclose(file) == 0;
  return read_ok && close_ok && Fc3ParseProcessStatStartTime(line, output);
}

bool Fc3ProcessMatchesStartTime(pid_t pid, std::uint64_t expected) {
  std::uint64_t actual = 0;
  return expected != 0 && Fc3ReadProcessStartTime(pid, &actual) &&
         actual == expected;
}
#endif

bool ReadTracerPid(pid_t pid, std::uint32_t* output) {
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (!file) return false;
  char line[256]{};
  bool found = false;
  while (std::fgets(line, sizeof(line), file)) {
    unsigned int value = 0;
    if (std::sscanf(line, "TracerPid:\t%u", &value) == 1) {
      *output = value;
      found = true;
      break;
    }
  }
  std::fclose(file);
  return found;
}

bool ContinueOwner(WatchedThread* owner, Report* report) {
  if (!PokeDebug(owner->tid, 6, 0) || !ContinueThread(owner->tid)) {
    ++report->ptrace_errors;
    return false;
  }
  owner->stopped = false;
  return true;
}

#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
bool DetachStoppedWithoutDebugWrites(std::vector<WatchedThread>* threads,
                                     std::uint32_t* detached) {
  if (!threads || !detached) return false;
  *detached = 0;
  bool ok = true;
  for (auto& thread : *threads) {
    if (!thread.live) continue;
    if (!thread.stopped ||
        ptrace(PTRACE_DETACH, thread.tid, nullptr, nullptr) == -1) {
      ok = false;
      continue;
    }
    thread.live = false;
    thread.stopped = false;
    ++*detached;
  }
  return ok;
}

int RunFc3EntryStability(pid_t pid, std::uint64_t expected_start_time,
                         const char* report_path) {
  EntryStabilityReport report{};
  const char magic[8] = {'A', '9', 'F', 'C', '3', 'E', '1', 0};
  std::memcpy(report.magic, magic, sizeof(magic));
  report.version = 1;
  report.size = sizeof(report);
  report.pid = static_cast<std::uint64_t>(pid);
  report.expected_start_time = expected_start_time;
  report.flags = kEntryTargetVerified | kEntryStartTimeMatched |
                 kEntryNoDebugWrites | kEntryNoGameWrites;
  report.attach_start_ns = MonotonicNs();
  std::vector<WatchedThread> threads;
  if (!AttachStableInitialThreadSet(pid, 0, false, &threads) ||
      threads.empty()) {
    ++report.ptrace_errors;
  } else {
    report.flags |= kEntryStableSetAttached;
  }
  report.attach_end_ns = MonotonicNs();
  report.initial_threads = static_cast<std::uint32_t>(threads.size());
  std::uint32_t detached = 0;
  if (!DetachStoppedWithoutDebugWrites(&threads, &detached)) {
    ++report.ptrace_errors;
    report.cleanup_disposition = 1;
  } else if (detached == report.initial_threads) {
    report.flags |= kEntryCleanDetach;
  } else {
    ++report.semantic_errors;
    report.cleanup_disposition = 1;
  }
  report.detached_threads = detached;
  report.final_threads = detached;
  if (kill(pid, 0) == 0) report.flags |= kEntryProcessAlive;
  std::uint32_t tracer_pid = UINT32_MAX;
  if (ReadTracerPid(pid, &tracer_pid) && tracer_pid == 0)
    report.flags |= kEntryTracerClear;
  if ((report.flags & (kEntryProcessAlive | kEntryTracerClear)) !=
      (kEntryProcessAlive | kEntryTracerClear))
    report.cleanup_disposition = 1;
  const std::uint32_t required =
      kEntryTargetVerified | kEntryStartTimeMatched |
      kEntryStableSetAttached | kEntryCleanDetach | kEntryProcessAlive |
      kEntryTracerClear | kEntryNoDebugWrites | kEntryNoGameWrites;
  const bool success = report.flags == required &&
      report.cleanup_disposition == 0 && report.ptrace_errors == 0 &&
      report.semantic_errors == 0 && report.debug_write_attempts == 0 &&
      report.game_write_attempts == 0 && report.initial_threads != 0 &&
      report.final_threads == report.initial_threads;
  const bool report_ok =
      WriteReportExclusive(report_path, &report, sizeof(report));
  std::printf(
      "FC3_ENTRY_STABILITY_DONE success=%u flags=0x%x initial=%u "
      "final=%u detached=%u ptrace_errors=%" PRIu64
      " semantic_errors=%" PRIu64 " debug_writes=%" PRIu64
      " game_writes=%" PRIu64 " cleanup=%u report=%s\n",
      success, report.flags, report.initial_threads, report.final_threads,
      report.detached_threads, report.ptrace_errors, report.semantic_errors,
      report.debug_write_attempts, report.game_write_attempts,
      report.cleanup_disposition, report_path);
  return success && report_ok ? 0 : 8;
}
#endif

}  // namespace

#if A9TAS_FC2_CONTROLLER_NO_MAIN == 0
int main(int argc, char** argv) {
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  constexpr int kExpectedArgc = 8;
  constexpr int kAckIndex = 7;
  constexpr int kFc2ReportIndex = 5;
  constexpr int kFc3ReportIndex = 6;
  if (argc != kExpectedArgc) {
    std::fprintf(stderr,
                 "usage: %s PID EXPECTED_START_TIME LIB_BASE_HEX TIMEOUT_MS "
                 "FC2_REPORT "
                 "FC3_REPORT ACK\n",
                 argv[0]);
    return 2;
  }
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  const bool entry_stability_only =
      std::strcmp(argv[kAckIndex], kEntryStabilityAcknowledgement) == 0;
  if (!entry_stability_only &&
      std::strcmp(argv[kAckIndex], kAcknowledgement) != 0) {
#else
  if (std::strcmp(argv[kAckIndex], kAcknowledgement) != 0) {
#endif
#else
  if (argc != 6) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX TIMEOUT_MS REPORT ACK\n",
                 argv[0]);
    return 2;
  }
  if (std::strcmp(argv[5], kAcknowledgement) != 0) {
#endif
    std::fprintf(stderr, "exact acknowledgement missing; no process opened\n");
    return 2;
  }
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  std::uint64_t values[4]{};
  const int bases[4] = {10, 10, 16, 10};
  for (int index = 0; index < 4; ++index) {
#else
  std::uint64_t values[3]{};
  const int bases[3] = {10, 16, 10};
  for (int index = 0; index < 3; ++index) {
#endif
    if (!ParseUnsigned(argv[index + 1], bases[index], &values[index])) {
      std::fprintf(stderr, "invalid numeric argument\n");
      return 2;
    }
  }
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  if (values[0] == 0 || values[0] > static_cast<std::uint64_t>(INT32_MAX) ||
      values[1] == 0 || values[2] == 0 ||
      values[3] < kMinimumTimeoutMs || values[3] > kMaximumTimeoutMs ||
      access(argv[kFc2ReportIndex], F_OK) == 0 ||
      access(argv[kFc3ReportIndex], F_OK) == 0 ||
      std::strcmp(argv[kFc2ReportIndex], argv[kFc3ReportIndex]) == 0) {
#else
  if (values[0] == 0 || values[0] > static_cast<std::uint64_t>(INT32_MAX) ||
      values[1] == 0 || values[2] < kMinimumTimeoutMs ||
      values[2] > kMaximumTimeoutMs || access(argv[4], F_OK) == 0) {
#endif
    std::fprintf(stderr, "invalid range or report already exists\n");
    return 2;
  }

  const pid_t pid = static_cast<pid_t>(values[0]);
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  const std::uint64_t expected_start_time = values[1];
  const auto base = static_cast<std::uintptr_t>(values[2]);
  const auto timeout_ns = values[3] * 1000000ULL;
  if (!Fc3ProcessMatchesStartTime(pid, expected_start_time)) return 3;
#else
  const auto base = static_cast<std::uintptr_t>(values[1]);
  const auto timeout_ns = values[2] * 1000000ULL;
#endif
  Report report{};
  const char report_magic[8] = {'A', '9', 'F', 'C', '2', 'R', '1', 0};
  std::memcpy(report.magic, report_magic, 8);
  report.version = 1;
  report.size = sizeof(report);
  report.final_phase = static_cast<std::uint32_t>(Phase::kPreflight);
  report.pid = static_cast<std::uint64_t>(pid);
  report.library_base = base;

  if (!VerifyTargetBuild(pid, base)) return 3;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  if (entry_stability_only)
    return RunFc3EntryStability(pid, expected_start_time,
                                argv[kFc3ReportIndex]);
#endif
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return 4;
  a9tas::fc2_payload_elf_v1::Layout payload{};
  if (!a9tas::fc2_payload_elf_v1::Resolve(pid, mem, &payload)) {
    close(mem);
    return 3;
  }
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  Fc3TailRuntime fc3_tail{};
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  std::uintptr_t phase_main = 0;
  std::uintptr_t phase_fixed_delta = 0;
  fc3_tail.identity.pid = static_cast<std::uint64_t>(pid);
  fc3_tail.identity.library_base = base;
  fc3_tail.identity.payload_load_bias = payload.load_bias;
  fc3_tail.identity.payload_evidence = payload.evidence;
  fc3_tail.identity.dedicated_object = payload.dedicated_object;
  if (!ResolveMainObject(pid, base, 0, &phase_main) ||
      !Fc3CheckedAdd(phase_main, kAccumulatorOffset,
                     &phase_fixed_delta) ||
      !ResolveFinalOwner(pid, base, 0, &fc3_tail.final_control_owner)) {
    close(mem);
    return 3;
  }
  fc3_tail.identity.main_time_source_owner = phase_main;
  fc3_tail.identity.replay_fixed_delta_input = phase_fixed_delta;
#else
  if (a9tas_fc3_resolve_identities_review_v1(
          pid, mem, base, &fc3_tail.identity) != 0) {
    close(mem);
    return 3;
  }
  if (!InitializeFc3Report(&fc3_tail)) {
    close(mem);
    return 3;
  }
#endif
  g_fc3_tail = &fc3_tail;
#endif
  report.payload_shadow = payload.shadow;
  report.payload_control = payload.control;
  report.payload_evidence = payload.evidence;
  report.wrapper = payload.wrapper;
  report.dedicated_observer = payload.dedicated_observer;
  report.dedicated_object = payload.dedicated_object;
  report.dedicated_vptr = payload.dedicated_vptr;
  report.flags |= a9tas::fc2_report_v1::kTargetVerified;

  std::uintptr_t context = 0, car = 0;
  if (!ResolveIdentity(pid, mem, base, payload.dedicated_object, &context,
                       &car)) {
    close(mem);
    return 3;
  }
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  std::uintptr_t phase_adapter = 0;
  std::uintptr_t phase_world = 0;
  if (!ResolvePhysicsContext(pid, mem, base, context, &context,
                             &phase_adapter, &phase_world)) {
    close(mem);
    return 3;
  }
  fc3_tail.identity.physics_backend_world = phase_world;
#endif
  close(mem);
  report.physics_context = context;
  report.callback_list = context + kCallbackListOffset;
  report.callback_flags = context + kCallbackFlagsOffset;
  report.car_physics_state = car;
  report.original_vptr = base + kCarPhysicsPrimaryVtable;
  report.dedicated_members_before = 0;
  report.flags |= a9tas::fc2_report_v1::kIdentityPinned;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  fc3_tail.identity.physics_context = context;
  fc3_tail.identity.callback_list = report.callback_list;
  fc3_tail.identity.callback_flags = report.callback_flags;
  fc3_tail.identity.car_physics_state = car;
  if (!InitializeFc3PhaseMap(&fc3_tail)) {
    g_fc3_tail = nullptr;
    return 3;
  }
#endif
  if (!Fc3TailIdentityMatchesFc2(fc3_tail, report)) {
    g_fc3_tail = nullptr;
    return 3;
  }
#endif

#if A9TAS_FC3_TAIL_CANDIDATE == 1
  if (!Fc3ProcessMatchesStartTime(pid, expected_start_time)) {
    g_fc3_tail = nullptr;
    return 3;
  }
#endif
  mem = open(mem_path, O_RDWR | O_CLOEXEC);
  if (mem < 0) return 4;
  CallbackListHeader pinned_header{};
  if (!ValidateIdentity(mem, pid, base, context, car,
                        payload.dedicated_object, true, 0, &pinned_header) ||
      !PreparePayload(mem, base, context, car, payload, &report.shadow_vptr)) {
    close(mem);
    return 3;
  }
  report.flags |= a9tas::fc2_report_v1::kPayloadPrepared;

  std::vector<WatchedThread> threads;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  if (!Fc3ProcessMatchesStartTime(pid, expected_start_time)) {
    close(mem);
    g_fc3_tail = nullptr;
    return 3;
  }
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  std::vector<PendingCloneChild> pending_clone_children;
  std::vector<pid_t> preowner_retired_candidates;
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  if (!AttachStableInitialThreadSet(pid, report.callback_flags, true,
                                    &threads) ||
      threads.empty())
#else
  if (!AttachCurrentThreads(pid, report.callback_flags, true, &threads) ||
      threads.empty())
#endif
    ++report.ptrace_errors;
  report.initial_threads = static_cast<std::uint32_t>(threads.size());
  report.final_phase = static_cast<std::uint32_t>(Phase::kWaitingBootstrapOpen);

  WatchedThread* owner = nullptr;
  std::uint32_t retired_threads = 0;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  // Subset of retired_threads that never entered `threads`.  Tracked
  // retirements are already represented by threads.size(); untracked clone
  // retirements must be added separately to both sides of the accounting
  // invariant.
  std::uint32_t untracked_retired_threads = 0;
#endif
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  bool fc3_rollback_frozen = false;
#endif
  const std::uint64_t wait_deadline = MonotonicNs() + timeout_ns;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE != 1
  std::uint64_t next_rescan = MonotonicNs() + 250000000ULL;
#endif
  while (report.ptrace_errors == 0 && report.semantic_errors == 0 &&
         MonotonicNs() < wait_deadline && owner == nullptr) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE != 1
    if (MonotonicNs() >= next_rescan) {
      if (!AttachCurrentThreads(pid, report.callback_flags, true, &threads))
        ++report.ptrace_errors;
      next_rescan = MonotonicNs() + 250000000ULL;
    }
#endif
    int status = 0;
    const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
    if (tid == 0) {
      usleep(100);
      continue;
    }
    if (tid < 0) {
      if (errno == EINTR) continue;
      ++report.ptrace_errors;
      break;
    }
    WatchedThread* thread = FindWatched(&threads, tid);
#if A9TAS_FC3_TAIL_CANDIDATE == 1
    // A known traced parent may legitimately create a game thread while the
    // race is running.  Incorporate that kernel-stopped child into the exact
    // same watch plan; unknown stops and non-clone ptrace events still reject.
    const unsigned int preowner_ptrace_event =
        static_cast<unsigned int>(status) >> 16;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    const bool pending_child_stop =
        WIFSTOPPED(status) &&
        (WSTOPSIG(status) == SIGSTOP || WSTOPSIG(status) == SIGTRAP);
    if (!thread && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        preowner_retired_candidates.size() < 64) {
      if (Fc3TaskIsGone(pid, tid)) {
        preowner_retired_candidates.push_back(tid);
        ++report.initial_threads;
        ++retired_threads;
        ++untracked_retired_threads;
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=preowner_ephemeral_retired "
                     "tid=%d status=0x%x retired=%u candidates=%zu\n",
                     tid, status, retired_threads,
                     preowner_retired_candidates.size());
        continue;
      }
    }
    if (!thread && pending_child_stop &&
        pending_clone_children.size() < 64) {
      bool duplicate = false;
      for (const auto& pending : pending_clone_children)
        if (pending.tid == tid) duplicate = true;
      if (!duplicate) {
        pending_clone_children.push_back(PendingCloneChild{tid, status});
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=preowner_child_first tid=%d "
                     "status=0x%x event=%u stopped=%d exited=%d pending=%zu\n",
                     tid, status, preowner_ptrace_event, WIFSTOPPED(status),
                     WIFEXITED(status),
                     pending_clone_children.size());
        continue;
      }
    }
    if (thread && preowner_ptrace_event == PTRACE_EVENT_CLONE) {
      pid_t child_tid = 0;
      bool child_retired = false;
      bool child_already_counted = false;
      if (!IncorporateAutoTracedClone(
              tid, status, false, report.callback_flags, &threads,
              &pending_clone_children, &preowner_retired_candidates,
              &fc3_tail, &child_tid, &child_retired,
              &child_already_counted)) {
        std::fprintf(stderr,
                     "FC3_PHASE_DIAG stage=preowner_clone_incorporate "
                     "parent=%d status=0x%x event=%u errno=%d pending=%zu\n",
                     tid, status, preowner_ptrace_event, errno,
                     pending_clone_children.size());
        fc3_tail.phase_report.cleanup_disposition = 1;
        report.cleanup_disposition = 1;
        report.reject_reasons |= kRejectUnexpectedWaitEvent;
        ++fc3_tail.phase_report.ptrace_errors;
        ++report.ptrace_errors;
        break;
      }
      if (!child_already_counted) {
        ++report.initial_threads;
        if (child_retired) {
          ++retired_threads;
          ++untracked_retired_threads;
        }
      }
      continue;
    }
#endif
    if (!thread || preowner_ptrace_event != 0) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      std::fprintf(stderr,
                   "FC3_PHASE_DIAG stage=preowner_unexpected tid=%d "
                   "known=%d status=0x%x event=%u errno=%d pending=%zu\n",
                   tid, thread != nullptr, status, preowner_ptrace_event,
                   errno, pending_clone_children.size());
#endif
      fc3_tail.report.reject_reasons |=
          a9tas::fc3_replay_observer_v1::kRejectThreadLifecycle;
      fc3_tail.report.cleanup_disposition = 1;
      report.cleanup_disposition = 1;
      report.reject_reasons |= kRejectUnexpectedWaitEvent;
      ++fc3_tail.report.semantic_errors;
      ++report.semantic_errors;
      break;
    }
#endif
    if (WIFEXITED(status)) {
      if (WEXITSTATUS(status) != 0) {
        report.reject_reasons |= kRejectUnexpectedWaitState;
        ++report.semantic_errors;
        break;
      }
      if (thread && thread->live) {
        thread->live = false;
        thread->stopped = false;
        ++retired_threads;
      }
      continue;
    }
    if (!thread || !thread->live) {
      report.reject_reasons |= kRejectUnexpectedWaitEvent;
      ++report.semantic_errors;
      break;
    }
    if (WIFSIGNALED(status)) {
      report.reject_reasons |= kRejectThreadSignaled;
      ++report.semantic_errors;
      break;
    }
    if (!WIFSTOPPED(status)) {
      report.reject_reasons |= kRejectUnexpectedWaitState;
      ++report.semantic_errors;
      break;
    }
    thread->stopped = true;
    unsigned long dr6 = 0;
    std::uint16_t flags = 0;
    const bool signal_ok = WSTOPSIG(status) == SIGTRAP;
    const bool dr6_ok = signal_ok && PeekDebug(tid, 6, &dr6);
    const bool dr0_set = dr6_ok && (dr6 & 1UL) != 0;
    const bool flags_ok = dr0_set &&
        ReadExact(mem, report.callback_flags, &flags, sizeof(flags));
    if (!signal_ok || !dr6_ok || !dr0_set || !flags_ok) {
      if (!signal_ok)
        report.reject_reasons |= kRejectWrongStopSignal;
      else if (!dr6_ok)
        report.reject_reasons |= kRejectDr6Read;
      else if (!dr0_set)
        report.reject_reasons |= kRejectUnexpectedBreakpoint;
      else
        report.reject_reasons |= kRejectFlagsRead;
      ++report.semantic_errors;
      break;
    }
    if ((flags & 0xFFu) != 1) {
      if (!ContinueOwner(thread, &report)) break;
      continue;
    }
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    if (!pending_clone_children.empty()) {
      if (!ContinueOwner(thread, &report)) break;
      continue;
    }
#endif
    std::string name;
    std::vector<Mapping> maps;
    CallbackListHeader header{};
    std::uint32_t car_count = 0, car_count_full = 0, dedicated_count = 0;
    std::uintptr_t car_vptr = 0;
    const bool valid = ReadThreadName(pid, tid, &name) &&
        name.rfind("Thread-", 0) == 0 && ReadMaps(pid, &maps) &&
        ReadCallbackList(mem, maps, report.callback_list, &header) &&
        header.dispatching == 1 && header.deferred == 0 &&
        CountObject(mem, header, car, true, &car_count) && car_count == 1 &&
        CountObject(mem, header, car, false, &car_count_full) &&
        car_count_full == 1 &&
        CountObject(mem, header, payload.dedicated_object, false,
                    &dedicated_count) && dedicated_count == 0 &&
        ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) &&
        car_vptr == report.original_vptr;
    if (!valid) {
      report.reject_reasons |= kRejectThreadName | kRejectListRead |
                               kRejectDispatchState | kRejectCarMembership |
                               kRejectDedicatedMembership | kRejectCarVptr;
      ++report.semantic_errors;
      break;
    }
    owner = thread;
    report.owner_tid = tid;
    report.bootstrap_open_ns = MonotonicNs();
    report.flags |= a9tas::fc2_report_v1::kBootstrapOpenSeen;
  }

  if (owner && report.ptrace_errors == 0 && report.semantic_errors == 0) {
    const pid_t transaction_tid = owner->tid;
    const std::size_t initial_freeze_enrolled_before = threads.size();
    const bool initial_frozen = FreezeAllExcept(
        pid, transaction_tid, report.callback_flags, &threads,
        &retired_threads);
    const std::size_t initial_freeze_newly_enrolled =
        threads.size() - initial_freeze_enrolled_before;
    const bool initial_freeze_accounting_ok =
        initial_freeze_newly_enrolled <=
        UINT32_MAX - report.initial_threads;
    if (initial_freeze_accounting_ok) {
      report.initial_threads +=
          static_cast<std::uint32_t>(initial_freeze_newly_enrolled);
    }
    if (!initial_frozen || !initial_freeze_accounting_ok) {
      ++report.ptrace_errors;
    } else {
      owner = FindWatched(&threads, transaction_tid);
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      report.initial_threads =
          static_cast<std::uint32_t>(threads.size() +
                                     untracked_retired_threads);
#endif
      report.final_phase = static_cast<std::uint32_t>(Phase::kOthersFrozen);
      CallbackListHeader header{};
      std::vector<Mapping> maps;
      std::uintptr_t car_vptr = 0;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
      if (!owner || !owner->stopped || !ReadMaps(pid, &maps) ||
          !ReadCallbackList(mem, maps, report.callback_list, &header) ||
          header.dispatching != 1 || header.deferred != 0 ||
          !ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) ||
          car_vptr != report.original_vptr ||
          !ArmFc3BeforeDedicated(&threads, fc3_tail)) {
#else
      if (!owner || !owner->stopped || !ReadMaps(pid, &maps) ||
          !ReadCallbackList(mem, maps, report.callback_list, &header) ||
          header.dispatching != 1 || header.deferred != 0 ||
          !ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) ||
          car_vptr != report.original_vptr ||
          !ArmDedicatedWatch(owner->tid,
              payload.evidence + offsetof(PayloadEvidence, dedicated_entries))) {
#endif
        ++report.semantic_errors;
      } else if (InstallShadowVptrWhileFrozen(
                     mem, car, report.original_vptr, report.shadow_vptr,
                     &report)) {
        report.flags |= a9tas::fc2_report_v1::kSingleSwapWritten;
        report.final_phase = static_cast<std::uint32_t>(Phase::kShadowInstalled);
#if A9TAS_FC3_TAIL_CANDIDATE == 1
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 0
        fc3_tail.core = std::make_unique<
            a9tas::fc3_observer_event_core_v1::Core>(transaction_tid);
        fc3_tail.report.owner_tid = transaction_tid;
        fc3_tail.report.initial_threads =
            static_cast<std::uint32_t>(threads.size());
        fc3_tail.report.final_phase = static_cast<std::uint32_t>(
            a9tas::fc3_replay_observer_v1::Phase::kWaitingDedicatedHit);
#else
        fc3_tail.watching_world_commit = false;
        fc3_tail.watching_f64 = false;
        fc3_tail.watching_c9c = false;
        fc3_tail.phase_core =
            std::make_unique<a9tas::fc3_phase_map_v1::Core>(transaction_tid);
        fc3_tail.phase_report.callback_owner_tid = transaction_tid;
        fc3_tail.phase_report.initial_threads =
            static_cast<std::uint32_t>(threads.size() +
                                       untracked_retired_threads);
        fc3_tail.phase_report.flags |=
            a9tas::fc3_phase_map_v1::kWatchPlanArmed;
        fc3_tail.phase_report.final_phase = static_cast<std::uint32_t>(
            a9tas::fc3_phase_map_v1::Phase::kWaitingBootstrapClose);
#endif
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
        if (!ResumeAllLiveExcept(&threads, transaction_tid,
                                 "initial_transaction")) {
          ++fc3_tail.phase_report.ptrace_errors;
          ++report.ptrace_errors;
        } else
#endif
        if (ContinueOwner(owner, &report)) {
          report.final_phase =
              static_cast<std::uint32_t>(Phase::kWaitingRegistrationProof);
          enum class Await {
            kBootstrapClose,
            kRegistrationOpen,
            kDedicatedHit,
            kRemovalClose,
            kCompactionOpen,
            kDone,
          } await = Await::kBootstrapClose;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
          const bool transaction_deadline_valid =
              report.bootstrap_open_ns <=
                  UINT64_MAX - kFc3PhaseMapTransactionDeadlineNs;
          const std::uint64_t transaction_deadline =
              transaction_deadline_valid
                  ? report.bootstrap_open_ns +
                        kFc3PhaseMapTransactionDeadlineNs
                  : 0;
          if (!transaction_deadline_valid) ++report.semantic_errors;
          std::fprintf(stderr,
                       "FC3_PHASE_DIAG stage=transaction_window now=%" PRIu64
                       " owner_deadline=%" PRIu64 " phase_deadline=%" PRIu64
                       "\n",
                       MonotonicNs(), wait_deadline, transaction_deadline);
#else
          const std::uint64_t transaction_deadline = std::min<std::uint64_t>(
              wait_deadline,
              report.bootstrap_open_ns + kTransactionDeadlineNs);
#endif
          while (await != Await::kDone && report.ptrace_errors == 0 &&
                 report.semantic_errors == 0 &&
                 MonotonicNs() < transaction_deadline) {
            int status = 0;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
            const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
#else
            const pid_t tid = waitpid(owner->tid, &status, __WALL | WNOHANG);
#endif
            if (tid == 0) {
              usleep(100);
              continue;
            }
            if (tid < 0) {
              if (errno == EINTR) continue;
              ++report.ptrace_errors;
              break;
            }
#if A9TAS_FC3_TAIL_CANDIDATE == 1
            unsigned long dr6 = 0;
            WatchedThread* event_thread = FindWatched(&threads, tid);
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
            const unsigned int ptrace_event =
                static_cast<unsigned int>(status) >> 16;
            const bool pending_child_stop =
                WIFSTOPPED(status) &&
                (WSTOPSIG(status) == SIGSTOP || WSTOPSIG(status) == SIGTRAP);
            if (!event_thread && pending_child_stop &&
                pending_clone_children.size() < 64) {
              bool duplicate = false;
              for (const auto& pending : pending_clone_children)
                if (pending.tid == tid) duplicate = true;
              if (!duplicate) {
                pending_clone_children.push_back(PendingCloneChild{tid, status});
                std::fprintf(stderr,
                             "FC3_PHASE_DIAG stage=transaction_child_first "
                             "tid=%d status=0x%x event=%u stopped=%d exited=%d "
                             "pending=%zu\n",
                             tid, status, ptrace_event, WIFSTOPPED(status),
                             WIFEXITED(status),
                             pending_clone_children.size());
                continue;
              }
            }
#endif
            if (!event_thread || !event_thread->live) {
              fc3_tail.report.reject_reasons |=
                  a9tas::fc3_replay_observer_v1::kRejectThreadLifecycle;
              fc3_tail.report.cleanup_disposition = 1;
              report.cleanup_disposition = 1;
              report.reject_reasons |= kRejectUnexpectedWaitEvent;
              ++report.semantic_errors;
              break;
            }
            event_thread->stopped = WIFSTOPPED(status);
#if A9TAS_FC3_PHASE_MAP_CANDIDATE != 1
            const unsigned int ptrace_event =
                static_cast<unsigned int>(status) >> 16;
#endif
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
            if (ptrace_event == PTRACE_EVENT_CLONE) {
              pid_t child_tid = 0;
              bool child_retired = false;
              bool child_already_counted = false;
              if (!IncorporateAutoTracedClone(
                      tid, status, true, report.callback_flags, &threads,
                      &pending_clone_children, &preowner_retired_candidates,
                      &fc3_tail, &child_tid, &child_retired,
                      &child_already_counted)) {
                std::fprintf(stderr,
                             "FC3_PHASE_DIAG stage=transaction_clone_incorporate "
                             "parent=%d status=0x%x event=%u errno=%d pending=%zu\n",
                             tid, status, ptrace_event, errno,
                             pending_clone_children.size());
                fc3_tail.phase_report.cleanup_disposition = 1;
                report.cleanup_disposition = 1;
                report.reject_reasons |= kRejectUnexpectedWaitEvent;
                ++fc3_tail.phase_report.ptrace_errors;
                ++report.ptrace_errors;
                break;
              }
              if (!child_already_counted) {
                ++report.initial_threads;
                ++fc3_tail.phase_report.initial_threads;
                if (child_retired) {
                  ++retired_threads;
                  ++untracked_retired_threads;
                }
              }
              owner = FindWatched(&threads, transaction_tid);
              if (!owner) {
                ++report.ptrace_errors;
                break;
              }
              continue;
            }
#endif
            if (ptrace_event != 0) {
              fc3_tail.report.reject_reasons |=
                  a9tas::fc3_replay_observer_v1::kRejectThreadLifecycle;
              fc3_tail.report.cleanup_disposition = 1;
              report.cleanup_disposition = 1;
              report.reject_reasons |= kRejectUnexpectedWaitEvent;
              ++fc3_tail.report.semantic_errors;
              ++report.semantic_errors;
              break;
            }
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
            if (WIFEXITED(status)) {
              if (WEXITSTATUS(status) != 0) {
                report.reject_reasons |= kRejectUnexpectedWaitState;
                ++report.semantic_errors;
                break;
              }
              event_thread->live = false;
              event_thread->stopped = false;
              ++retired_threads;
              continue;
            }
            if (WIFSIGNALED(status)) {
              report.reject_reasons |= kRejectThreadSignaled;
              ++report.semantic_errors;
              break;
            }
#endif
            if (!event_thread->stopped || WSTOPSIG(status) != SIGTRAP ||
                !PeekDebug(event_thread->tid, 6, &dr6)) {
#else
            owner->stopped = WIFSTOPPED(status);
            unsigned long dr6 = 0;
            if (!owner->stopped || WSTOPSIG(status) != SIGTRAP ||
                !PeekDebug(owner->tid, 6, &dr6)) {
#endif
              report.reject_reasons |= kRejectUnexpectedWaitState;
              ++report.semantic_errors;
              break;
            }
#if A9TAS_FC3_TAIL_CANDIDATE == 1
            const unsigned long fc3_hits = dr6 & 0xFu;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
            if (fc3_hits == 0 || (fc3_hits & (fc3_hits - 1u)) != 0 ||
                !fc3_tail.phase_core) {
              fc3_tail.phase_report.reject_reasons |=
                  a9tas::fc3_phase_map_v1::kRejectWrongOrder;
              ++fc3_tail.phase_report.semantic_errors;
              ++report.semantic_errors;
              break;
            }
            if (fc3_tail.phase_core->proved()) {
              // FC3 proves the next physics cycle at next-C98, while FC2 must
              // still consume the following callback open to prove deferred
              // removal/compaction.  Do not feed that legitimate FC2 tail
              // event back into an already-proved FC3 core.
              const bool fc2_tail_event =
                  fc3_hits == 0x1u ||
                  (fc3_hits == 0x2u && !fc3_tail.watching_world_commit);
              if (!fc2_tail_event) {
                if (!PokeDebug(event_thread->tid, 6, 0) ||
                    !ContinueThread(event_thread->tid)) {
                  ++fc3_tail.phase_report.ptrace_errors;
                  ++report.ptrace_errors;
                  break;
                }
                event_thread->stopped = false;
                continue;
              }
            } else {
            a9tas::fc3_phase_map_v1::Event phase_event{};
            // Rearming may discover and append a newly created task.  Keep
            // stable ids across that vector mutation and reacquire pointers
            // before either the event thread or the callback owner is used.
            const pid_t phase_event_tid = event_thread->tid;
            phase_event.tid = phase_event_tid;
            phase_event.monotonic_ns = MonotonicNs();
            if (fc3_hits == 0x1u) {
              std::uint16_t observed_flags = 0;
              if (!ReadExact(mem, report.callback_flags, &observed_flags,
                             sizeof(observed_flags))) {
                ++fc3_tail.phase_report.read_errors;
                ++report.semantic_errors;
                break;
              }
              phase_event.kind =
                  a9tas::fc3_phase_map_v1::EventKind::kCallbackFlags;
              phase_event.callback_flags = observed_flags;
              phase_event.car_callback_index = UINT32_MAX;
              phase_event.dedicated_callback_index = UINT32_MAX;
              if (phase_event.callback_flags == 1u) {
                std::vector<Mapping> phase_maps;
                CallbackListHeader phase_header{};
                if (!ReadMaps(pid, &phase_maps) ||
                    !ReadCallbackList(mem, phase_maps, report.callback_list,
                                      &phase_header) ||
                    phase_header.dispatching != 1 ||
                    phase_header.deferred != 0 ||
                    !FindUniqueActiveObjectIndex(
                        mem, phase_header, report.car_physics_state,
                        &phase_event.car_callback_index) ||
                    !FindUniqueActiveObjectIndex(
                        mem, phase_header, report.dedicated_object,
                        &phase_event.dedicated_callback_index)) {
                  fc3_tail.phase_report.reject_reasons |=
                      a9tas::fc3_phase_map_v1::kRejectCallbackListOrder;
                  ++fc3_tail.phase_report.semantic_errors;
                  ++report.semantic_errors;
                  break;
                }
              }
            } else if (fc3_hits == 0x2u) {
              if (fc3_tail.watching_world_commit) {
                if (!ReadExact(mem, fc3_tail.world_commit_address,
                               &phase_event.observed_bits,
                               sizeof(phase_event.observed_bits))) {
                  ++fc3_tail.phase_report.read_errors;
                  ++report.semantic_errors;
                  break;
                }
                phase_event.kind =
                    a9tas::fc3_phase_map_v1::EventKind::kWorldCommit;
              } else {
                PayloadEvidence phase_evidence{};
                if (!ReadExact(mem, report.payload_evidence, &phase_evidence,
                               sizeof(phase_evidence)) ||
                    phase_evidence.dedicated_entries != 1) {
                  ++fc3_tail.phase_report.read_errors;
                  ++report.semantic_errors;
                  break;
                }
                phase_event.kind =
                    a9tas::fc3_phase_map_v1::EventKind::kDedicated;
              }
            } else if (fc3_hits == 0x4u) {
              if (fc3_tail.watching_f64) {
                if (!ReadExact(mem, fc3_tail.f64_address,
                               &phase_event.observed_bits,
                               sizeof(phase_event.observed_bits))) {
                  ++fc3_tail.phase_report.read_errors;
                  ++report.semantic_errors;
                  break;
                }
                phase_event.kind =
                    a9tas::fc3_phase_map_v1::EventKind::kF64;
              } else {
                std::int64_t accumulator = 0;
                if (!ReadExact(mem,
                               fc3_tail.identity.phase_witness_accumulator,
                               &accumulator, sizeof(accumulator))) {
                  ++fc3_tail.phase_report.read_errors;
                  ++report.semantic_errors;
                  break;
                }
                std::fprintf(
                    stderr,
                    "FC3_PHASE_DIAG stage=accumulator_write phase=%u "
                    "tid=%d value=%" PRId64 "\n",
                    static_cast<unsigned int>(
                        fc3_tail.phase_core->phase()),
                    phase_event_tid, accumulator);
                // The proven scheduler boundary writes this 64-bit field
                // twice: non-zero is PRE_PHYSICS/fixed delta, zero is the
                // POST_PHYSICS reset.  A reset from the prefix cycle may land
                // after bootstrap close and must not masquerade as the next
                // cycle's Delta.
                if (accumulator == 0) {
                  if (!PokeDebug(event_thread->tid, 6, 0) ||
                      !ContinueThread(event_thread->tid)) {
                    ++fc3_tail.phase_report.ptrace_errors;
                    ++report.ptrace_errors;
                    break;
                  }
                  event_thread->stopped = false;
                  continue;
                }
                phase_event.kind =
                    a9tas::fc3_phase_map_v1::EventKind::kDelta;
              }
            } else {
              phase_event.kind = fc3_tail.watching_c9c
                  ? a9tas::fc3_phase_map_v1::EventKind::kC9C
                  : a9tas::fc3_phase_map_v1::EventKind::kC98;
            }
            const auto phase_before = fc3_tail.phase_core->phase();
            const auto phase_decision = fc3_tail.phase_core->Consume(
                phase_event, &fc3_tail.phase_report);
            const bool ignored_callback_flag =
                fc3_hits == 0x1u &&
                phase_before == fc3_tail.phase_core->phase();
            if (phase_decision ==
                a9tas::fc3_phase_map_v1::Decision::kReject) {
              std::fprintf(stderr,
                           "FC3_PHASE_DIAG stage=phase_reject phase=%u "
                           "kind=%u tid=%d dr6=0x%lx flags=0x%x reasons=0x%x\n",
                           static_cast<unsigned int>(phase_before),
                           static_cast<unsigned int>(phase_event.kind),
                           phase_event.tid, dr6, phase_event.callback_flags,
                           fc3_tail.phase_report.reject_reasons);
              ++report.semantic_errors;
              break;
            }
            const bool must_rearm =
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmC9C ||
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmF64 ||
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmWorldCommit ||
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmNextCycle;
            const bool next_world =
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmWorldCommit ||
                (fc3_tail.watching_world_commit &&
                 phase_decision !=
                     a9tas::fc3_phase_map_v1::Decision::kRearmNextCycle);
            const bool next_f64 =
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmF64 ||
                (fc3_tail.watching_f64 &&
                 phase_decision !=
                     a9tas::fc3_phase_map_v1::Decision::kRearmNextCycle);
            const bool next_c9c =
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmC9C ||
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmF64 ||
                phase_decision ==
                    a9tas::fc3_phase_map_v1::Decision::kRearmWorldCommit;
            if (must_rearm) {
              if (!RearmFc3PhaseMapAndResumeOthers(
                      pid, phase_event_tid, next_world, next_f64, next_c9c,
                      &threads, &retired_threads, &fc3_tail, &report)) {
                ++fc3_tail.phase_report.ptrace_errors;
                ++report.ptrace_errors;
                break;
              }
              event_thread = FindWatched(&threads, phase_event_tid);
              owner = FindWatched(&threads, transaction_tid);
              const bool owner_should_be_stopped =
                  phase_event_tid == transaction_tid;
              if (!event_thread || !event_thread->live ||
                  !event_thread->stopped || !owner || !owner->live ||
                  owner->stopped != owner_should_be_stopped) {
                std::fprintf(
                    stderr,
                    "FC3_PHASE_DIAG stage=post_rearm_reacquire_failed "
                    "event=%d owner=%d event_found=%d event_live=%d "
                    "event_stopped=%d owner_found=%d owner_live=%d "
                    "owner_stopped=%d owner_should_be_stopped=%d\n",
                    phase_event_tid, transaction_tid, event_thread != nullptr,
                    event_thread ? event_thread->live : 0,
                    event_thread ? event_thread->stopped : 0, owner != nullptr,
                    owner ? owner->live : 0, owner ? owner->stopped : 0,
                    owner_should_be_stopped);
                ++fc3_tail.phase_report.ptrace_errors;
                ++report.ptrace_errors;
                break;
              }
            }
            const bool phase_only_event =
                phase_event.kind !=
                    a9tas::fc3_phase_map_v1::EventKind::kCallbackFlags &&
                phase_event.kind !=
                    a9tas::fc3_phase_map_v1::EventKind::kDedicated;
            if (phase_only_event || ignored_callback_flag) {
              if (!PokeDebug(event_thread->tid, 6, 0) ||
                  !ContinueThread(event_thread->tid)) {
                ++fc3_tail.phase_report.ptrace_errors;
                ++report.ptrace_errors;
                break;
              }
              event_thread->stopped = false;
              continue;
            }
            }
#else
            const bool final_snapshot_candidate =
                event_thread->tid == transaction_tid && fc3_hits == 0x1u &&
                fc3_tail.core->phase() ==
                    a9tas::fc3_replay_observer_v1::Phase::
                        kWaitingNextCallbackOpen;
            if (final_snapshot_candidate &&
                !Fc3FinalSnapshotBarrier(pid, transaction_tid,
                                         transaction_deadline, &threads)) {
              fc3_tail.report.reject_reasons |=
                  a9tas::fc3_replay_observer_v1::kRejectPtrace;
              fc3_tail.report.cleanup_disposition = 1;
              report.cleanup_disposition = 1;
              report.reject_reasons |= kRejectUnexpectedWaitEvent;
              ++fc3_tail.report.ptrace_errors;
              ++report.ptrace_errors;
              break;
            }
            a9tas::fc3_observer_event_core_v1::Stop fc3_stop{};
            if (!BuildFc3Stop(mem, event_thread->tid, dr6, fc3_tail,
                              &fc3_stop)) {
              ++fc3_tail.report.read_errors;
              ++report.semantic_errors;
              break;
            }
            const auto fc3_decision =
                fc3_tail.core->Consume(fc3_stop, &fc3_tail.report);
            if (fc3_decision ==
                a9tas::fc3_observer_event_core_v1::Decision::kReject) {
              ++report.semantic_errors;
              break;
            }
            if (fc3_decision == a9tas::fc3_observer_event_core_v1::Decision::
                                    kRearmNitroAndContinue &&
                !RearmFc3NitroAndResumeOthers(&threads, transaction_tid,
                                               fc3_tail)) {
              ++fc3_tail.report.ptrace_errors;
              ++report.ptrace_errors;
              break;
            }
            if (event_thread->tid != transaction_tid || fc3_hits == 0x8u) {
              if (!PokeDebug(event_thread->tid, 6, 0) ||
                  !ContinueThread(event_thread->tid)) {
                ++fc3_tail.report.ptrace_errors;
                ++report.ptrace_errors;
                break;
              }
              event_thread->stopped = false;
              continue;
            }
#endif
#endif
            const bool flags_hit = (dr6 & 1UL) != 0;
            const bool dedicated_hit = (dr6 & 2UL) != 0;
            std::uint16_t flags = 0;
            if (flags_hit && !ReadExact(mem, report.callback_flags, &flags,
                                        sizeof(flags))) {
              report.reject_reasons |= kRejectFlagsRead;
              ++report.semantic_errors;
              break;
            }

            if (await == Await::kBootstrapClose && flags_hit &&
                (flags & 0xFFu) == 0) {
              await = Await::kRegistrationOpen;
            } else if (await == Await::kRegistrationOpen && flags_hit &&
                       (flags & 0xFFu) == 1) {
              std::vector<Mapping> current_maps;
              CallbackListHeader current{};
              std::uint32_t dedicated_count = 0;
              std::uint32_t dedicated_count_full = 0;
              PayloadEvidence evidence{};
              if (!ReadMaps(pid, &current_maps) ||
                  !ReadCallbackList(mem, current_maps, report.callback_list,
                                    &current) ||
                  current.dispatching != 1 || current.deferred != 0 ||
                  !CountObject(mem, current, payload.dedicated_object, true,
                               &dedicated_count) || dedicated_count != 1 ||
                  !CountObject(mem, current, payload.dedicated_object, false,
                               &dedicated_count_full) ||
                  dedicated_count_full != 1 ||
                  !ReadExact(mem, payload.evidence, &evidence,
                             sizeof(evidence)) ||
                  !RegistrationEvidenceComplete(evidence, report)) {
                report.reject_reasons |= kRejectDedicatedMembership |
                                         kRejectRegistrationEvidence;
                ++report.semantic_errors;
                break;
              }
              report.dedicated_members_after_registration = dedicated_count;
              report.registration_proved_ns = MonotonicNs();
              report.flags |=
                  a9tas::fc2_report_v1::kRegistrationAcknowledged |
                  a9tas::fc2_report_v1::kRegistrationNextOpenSeen |
                  a9tas::fc2_report_v1::kDedicatedMembershipPresent;
              report.final_phase =
                  static_cast<std::uint32_t>(Phase::kRegistrationProved);
              await = Await::kDedicatedHit;
            } else if (await == Await::kDedicatedHit && dedicated_hit) {
              std::uint64_t dedicated_entries = 0;
              if (!ReadExact(mem,
                      payload.evidence +
                          offsetof(PayloadEvidence, dedicated_entries),
                      &dedicated_entries, sizeof(dedicated_entries)) ||
                  dedicated_entries != 1) {
                report.reject_reasons |= kRejectDedicatedEvidenceHit;
                ++report.semantic_errors;
                break;
              }
              report.flags |= a9tas::fc2_report_v1::kDedicatedOwnerHit;
              report.final_phase =
                  static_cast<std::uint32_t>(Phase::kWaitingRemovalProof);
              await = Await::kRemovalClose;
            } else if (await == Await::kRemovalClose && flags_hit &&
                       (flags & 0xFFu) == 0) {
              PayloadEvidence evidence{};
              if (!ReadExact(mem, payload.evidence, &evidence,
                             sizeof(evidence)) ||
                  !RemovalEvidenceComplete(evidence, report)) {
                report.reject_reasons |= kRejectRemovalEvidence;
                ++report.semantic_errors;
                break;
              }
              report.removal_requested_ns = MonotonicNs();
              report.flags |= a9tas::fc2_report_v1::kRemovalAcknowledged;
              report.final_phase =
                  static_cast<std::uint32_t>(Phase::kRemovalProved);
              await = Await::kCompactionOpen;
            } else if (await == Await::kCompactionOpen && flags_hit &&
                       (flags & 0xFFu) == 1) {
              std::vector<Mapping> current_maps;
              CallbackListHeader current{};
              std::uint32_t dedicated_count = UINT32_MAX;
              if (!ReadMaps(pid, &current_maps) ||
                  !ReadCallbackList(mem, current_maps, report.callback_list,
                                    &current) ||
                  current.dispatching != 1 || current.deferred != 0 ||
                  !CountObject(mem, current, payload.dedicated_object, false,
                               &dedicated_count) || dedicated_count != 0 ||
                  !ReadExact(mem, payload.evidence, &report.evidence,
                             sizeof(report.evidence)) ||
                  !RemovalEvidenceComplete(report.evidence, report)) {
                report.reject_reasons |= kRejectDedicatedMembership |
                                         kRejectRemovalEvidence;
                ++report.semantic_errors;
                break;
              }
              report.dedicated_members_after_removal = dedicated_count;
              report.compaction_proved_ns = MonotonicNs();
              report.flags |=
                  a9tas::fc2_report_v1::kCompactionNextOpenSeen |
                  a9tas::fc2_report_v1::kDedicatedMembershipAbsent;
              report.final_phase =
                  static_cast<std::uint32_t>(Phase::kCompactionProved);
              await = Await::kDone;
            } else {
              report.reject_reasons |= kRejectUnexpectedBreakpoint;
              ++report.semantic_errors;
              break;
            }
            if (await != Await::kDone && !ContinueOwner(owner, &report)) break;
          }
          if (await != Await::kDone && report.ptrace_errors == 0 &&
              report.semantic_errors == 0)
            ++report.semantic_errors;
          // Any failed freeze/rearm may have appended a task before returning
          // false, invalidating every vector element pointer.  Reacquire the
          // rollback owner from its stable tid even on error paths.
          owner = FindWatched(&threads, transaction_tid);
          if (!owner || !owner->live) {
            std::fprintf(stderr,
                         "FC3_PHASE_DIAG stage=rollback_owner_reacquire_failed "
                         "owner=%d found=%d live=%d\n",
                         transaction_tid, owner != nullptr,
                         owner ? owner->live : 0);
            ++report.ptrace_errors;
          } else if (!owner->stopped &&
                     !A9TAS_STOP_THREAD(owner->tid)) {
            ++report.ptrace_errors;
          } else {
            owner->stopped = true;
          }
#if A9TAS_FC3_TAIL_CANDIDATE == 1
          const pid_t rollback_owner_tid = transaction_tid;
          const bool owner_stopped_for_rollback = owner && owner->live &&
                                                   owner->stopped;
          const std::size_t rollback_enrolled_before = threads.size();
          const bool others_frozen = FreezeAllExcept(
              pid, rollback_owner_tid, report.callback_flags, &threads,
              &retired_threads);
          const std::size_t rollback_newly_enrolled =
              threads.size() - rollback_enrolled_before;
          const bool rollback_accounting_ok =
              rollback_newly_enrolled <= UINT32_MAX - report.initial_threads &&
              rollback_newly_enrolled <=
                  UINT32_MAX - fc3_tail.phase_report.initial_threads;
          if (rollback_accounting_ok) {
            report.initial_threads +=
                static_cast<std::uint32_t>(rollback_newly_enrolled);
            fc3_tail.phase_report.initial_threads +=
                static_cast<std::uint32_t>(rollback_newly_enrolled);
          }
          fc3_rollback_frozen = owner_stopped_for_rollback && others_frozen &&
                                rollback_accounting_ok &&
                                AllLiveThreadsStopped(threads);
          if (!fc3_rollback_frozen) {
            ++fc3_tail.report.ptrace_errors;
            ++report.ptrace_errors;
          }
          if (
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
              !fc3_tail.phase_core || !fc3_tail.phase_core->proved()
#else
              !fc3_tail.core || !fc3_tail.core->phase_proved()
#endif
          ) {
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
            if (fc3_tail.phase_report.reject_reasons == 0)
              fc3_tail.phase_report.reject_reasons |=
                  a9tas::fc3_phase_map_v1::kRejectWrongOrder;
            ++fc3_tail.phase_report.semantic_errors;
#else
            if (fc3_tail.report.reject_reasons == 0)
              fc3_tail.report.reject_reasons |=
                  a9tas::fc3_replay_observer_v1::kRejectTimeout;
            ++fc3_tail.report.semantic_errors;
#endif
          }
#endif
        }
      }
    }
  }

  report.final_phase = static_cast<std::uint32_t>(Phase::kRollback);
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  std::uintptr_t rollback_vptr = 0;
  if (!ReadExact(mem, car, &rollback_vptr, sizeof(rollback_vptr))) {
    ++report.read_errors;
  } else if (rollback_vptr == report.original_vptr) {
    report.flags |= a9tas::fc2_report_v1::kOriginalVptrFinal;
  } else if (rollback_vptr == report.shadow_vptr && fc3_rollback_frozen &&
             ConditionalRollback(mem, car, report.shadow_vptr,
                                 report.original_vptr, &report)) {
    std::uintptr_t final_vptr = 0;
    if (ReadExact(mem, car, &final_vptr, sizeof(final_vptr)) &&
        final_vptr == report.original_vptr)
      report.flags |= a9tas::fc2_report_v1::kOriginalVptrFinal;
    else
      ++report.semantic_errors;
  } else {
    // Never repair a target vptr while any live thread may still be running.
    // The future guarded runner must force-stop this confirmed fresh process.
    report.cleanup_disposition = 1;
    ++report.semantic_errors;
  }
#else
  if (ConditionalRollback(mem, car, report.shadow_vptr, report.original_vptr,
                          &report)) {
    std::uintptr_t final_vptr = 0;
    if (ReadExact(mem, car, &final_vptr, sizeof(final_vptr)) &&
        final_vptr == report.original_vptr)
      report.flags |= a9tas::fc2_report_v1::kOriginalVptrFinal;
    else
      ++report.semantic_errors;
  }
#endif

  if ((report.flags & a9tas::fc2_report_v1::kDedicatedMembershipAbsent) == 0) {
    std::vector<Mapping> maps;
    CallbackListHeader header{};
    std::uint32_t members = UINT32_MAX;
    if (!ReadMaps(pid, &maps) ||
        !ReadCallbackList(mem, maps, report.callback_list, &header) ||
        !CountObject(mem, header, report.dedicated_object, false, &members) ||
        members != 0)
      report.cleanup_disposition = 1;  // runner must stop fresh process
  }

  std::uint32_t detached = 0;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  // On failure, an auto-traced child may have reported its initial stop before
  // the parent's clone event.  Enroll it with the verified-clean initial debug
  // state so failure cleanup can restore/detach it instead of leaking a tracer.
  for (const auto& pending : pending_clone_children) {
    if (WIFSTOPPED(pending.status) && !FindWatched(&threads, pending.tid)) {
      DebugState verified_clean{};
      verified_clean.valid = true;
      threads.push_back(
          WatchedThread{pending.tid, true, true, verified_clean});
      ++report.initial_threads;
      if (fc3_tail.phase_report.initial_threads != 0)
        ++fc3_tail.phase_report.initial_threads;
    }
  }
  pending_clone_children.clear();
#endif
  const bool detach_clean = RestoreAndDetachAll(&threads, &detached
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
      , pid, &retired_threads
#endif
      ) &&
      static_cast<std::size_t>(detached + retired_threads) ==
          threads.size()
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
              + untracked_retired_threads
#endif
      ;
  if (detach_clean)
    report.flags |= a9tas::fc2_report_v1::kCleanDetach;
  else {
    ++report.ptrace_errors;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
    report.cleanup_disposition = 1;
    fc3_tail.report.cleanup_disposition = 1;
    ++fc3_tail.report.ptrace_errors;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    fc3_tail.phase_report.cleanup_disposition = 1;
    ++fc3_tail.phase_report.ptrace_errors;
#endif
#endif
  }
  report.final_threads = detached + retired_threads;
  report.final_phase = static_cast<std::uint32_t>(Phase::kDetached);
  close(mem);
  const bool process_alive = kill(pid, 0) == 0;
  if (process_alive)
    report.flags |= a9tas::fc2_report_v1::kProcessAlive;
  std::uint32_t tracer_pid = UINT32_MAX;
  const bool tracer_clear = ReadTracerPid(pid, &tracer_pid) && tracer_pid == 0;
  if (tracer_clear)
    report.flags |= a9tas::fc2_report_v1::kTracerClear;

#if A9TAS_FC3_TAIL_CANDIDATE == 1
  if (!process_alive || !tracer_clear) {
    report.cleanup_disposition = 1;
    fc3_tail.report.cleanup_disposition = 1;
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
    fc3_tail.phase_report.cleanup_disposition = 1;
#endif
  }
  fc3_tail.report.final_threads = detached + retired_threads;
  if ((report.flags & a9tas::fc2_report_v1::kCleanDetach) != 0)
    fc3_tail.report.flags |= a9tas::fc3_replay_observer_v1::kCleanDetach;
  if ((report.flags & a9tas::fc2_report_v1::kProcessAlive) != 0)
    fc3_tail.report.flags |= a9tas::fc3_replay_observer_v1::kProcessAlive;
  if ((report.flags & a9tas::fc2_report_v1::kTracerClear) != 0)
    fc3_tail.report.flags |= a9tas::fc3_replay_observer_v1::kTracerClear;
  fc3_tail.report.final_phase = static_cast<std::uint32_t>(
      a9tas::fc3_replay_observer_v1::Phase::kDetached);
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  fc3_tail.phase_report.final_threads = detached + retired_threads;
  if ((report.flags & a9tas::fc2_report_v1::kCleanDetach) != 0)
    fc3_tail.phase_report.flags |= a9tas::fc3_phase_map_v1::kCleanDetach;
  if ((report.flags & a9tas::fc2_report_v1::kProcessAlive) != 0)
    fc3_tail.phase_report.flags |= a9tas::fc3_phase_map_v1::kProcessAlive;
  if ((report.flags & a9tas::fc2_report_v1::kTracerClear) != 0)
    fc3_tail.phase_report.flags |= a9tas::fc3_phase_map_v1::kTracerClear;
  if (fc3_tail.phase_core && fc3_tail.phase_core->proved())
    fc3_tail.phase_report.final_phase = static_cast<std::uint32_t>(
        a9tas::fc3_phase_map_v1::Phase::kDetached);
#endif
#endif

  const bool success =
      report.flags == a9tas::fc2_report_v1::kRequiredFlags &&
      report.game_write_attempts == 1 && report.game_write_failures == 0 &&
      report.rollback_attempts == 0 && report.rollback_failures == 0 &&
      report.read_errors == 0 && report.ptrace_errors == 0 &&
      report.semantic_errors == 0 && report.reject_reasons == 0 &&
      report.dedicated_members_before == 0 &&
      report.dedicated_members_after_registration == 1 &&
      report.dedicated_members_after_removal == 0 &&
      report.cleanup_disposition == 0 && report.initial_threads != 0 &&
      report.final_threads == report.initial_threads;
#if A9TAS_FC3_TAIL_CANDIDATE == 1
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  const bool fc3_success =
      fc3_tail.phase_report.flags ==
          a9tas::fc3_phase_map_v1::kRequiredFinalFlags &&
      fc3_tail.phase_report.reject_reasons == 0 &&
      fc3_tail.phase_report.read_errors == 0 &&
      fc3_tail.phase_report.ptrace_errors == 0 &&
      fc3_tail.phase_report.semantic_errors == 0 &&
      fc3_tail.phase_report.game_write_attempts == 0 &&
      fc3_tail.phase_report.action_calls == 0 &&
      fc3_tail.phase_report.nitro_calls == 0 &&
      fc3_tail.phase_report.physics_writes == 0 &&
      fc3_tail.phase_report.callback_flag_hits >= 4 &&
      fc3_tail.phase_report.delta_hits == 2 &&
      fc3_tail.phase_report.c98_hits == 2 &&
      fc3_tail.phase_report.c9c_hits == 1 &&
      fc3_tail.phase_report.f64_hits == 1 &&
      fc3_tail.phase_report.dedicated_hits == 1 &&
      fc3_tail.phase_report.world_commit_hits == 1 &&
      fc3_tail.phase_report.cleanup_disposition == 0 &&
      fc3_tail.phase_report.initial_threads != 0 &&
      fc3_tail.phase_report.final_threads ==
          fc3_tail.phase_report.initial_threads &&
      fc3_tail.report.read_errors == 0 &&
      fc3_tail.report.ptrace_errors == 0 &&
      fc3_tail.report.semantic_errors == 0 &&
      fc3_tail.report.reject_reasons == 0;
  const bool fc3_report_ok = WriteReport(
      argv[kFc3ReportIndex], fc3_tail.phase_report);
#else
  const bool fc3_success =
      fc3_tail.report.flags ==
          a9tas::fc3_replay_observer_v1::kRequiredFlags &&
      fc3_tail.report.reject_reasons == 0 &&
      fc3_tail.report.action_queue_writes == 0 &&
      fc3_tail.report.nitro_writes == 0 &&
      fc3_tail.report.game_write_attempts == 0 &&
      fc3_tail.report.read_errors == 0 &&
      fc3_tail.report.ptrace_errors == 0 &&
      fc3_tail.report.semantic_errors == 0 &&
      fc3_tail.report.dedicated_hits == 1 &&
      fc3_tail.report.callback_close_hits == 1 &&
      fc3_tail.report.accumulator_hits == 1 &&
      fc3_tail.report.dedicated_members_final == 0 &&
      fc3_tail.report.cleanup_disposition == 0 &&
      fc3_tail.report.initial_threads != 0 &&
      fc3_tail.report.final_threads == fc3_tail.report.initial_threads;
  const bool fc3_report_ok =
      WriteReport(argv[kFc3ReportIndex], fc3_tail.report);
#endif
  g_fc3_tail = nullptr;
#endif
#if A9TAS_FC3_TAIL_CANDIDATE == 1
  const bool report_ok = WriteReport(argv[kFc2ReportIndex], report);
#else
  const bool report_ok = WriteReport(argv[4], report);
#endif
  std::printf(
      "FC2_DONE success=%u flags=0x%x owner=%d game_writes=%" PRIu64
      " membership=%u,%u,%u cleanup=%u read_errors=%" PRIu64
      " ptrace_errors=%" PRIu64 " semantic_errors=%" PRIu64
      " report=%s\n",
      success, report.flags, report.owner_tid, report.game_write_attempts,
      report.dedicated_members_before,
      report.dedicated_members_after_registration,
      report.dedicated_members_after_removal, report.cleanup_disposition,
      report.read_errors, report.ptrace_errors, report.semantic_errors,
#if A9TAS_FC3_TAIL_CANDIDATE == 1
      argv[kFc2ReportIndex]);
#else
      argv[4]);
#endif
#if A9TAS_FC3_TAIL_CANDIDATE == 1
#if A9TAS_FC3_PHASE_MAP_CANDIDATE == 1
  std::printf(
      "FC3_PHASE_MAP_DONE success=%u flags=0x%x owner=%d delta=%" PRIu64
      " c98=%" PRIu64 " c9c=%" PRIu64 " f64=%" PRIu64
      " dedicated=%" PRIu64 " world=%" PRIu64
      " game_writes=%" PRIu64 " action_calls=%" PRIu64
      " nitro_calls=%" PRIu64 " physics_writes=%" PRIu64
      " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
      " semantic_errors=%" PRIu64 " report=%s\n",
      fc3_success, fc3_tail.phase_report.flags,
      fc3_tail.phase_report.callback_owner_tid,
      fc3_tail.phase_report.delta_hits, fc3_tail.phase_report.c98_hits,
      fc3_tail.phase_report.c9c_hits, fc3_tail.phase_report.f64_hits,
      fc3_tail.phase_report.dedicated_hits,
      fc3_tail.phase_report.world_commit_hits,
      fc3_tail.phase_report.game_write_attempts,
      fc3_tail.phase_report.action_calls, fc3_tail.phase_report.nitro_calls,
      fc3_tail.phase_report.physics_writes,
      fc3_tail.phase_report.read_errors, fc3_tail.phase_report.ptrace_errors,
      fc3_tail.phase_report.semantic_errors, argv[kFc3ReportIndex]);
#else
  std::printf(
      "FC3_TAIL_DONE success=%u flags=0x%x owner=%d dedicated=%" PRIu64
      " close=%" PRIu64 " accumulator=%" PRIu64
      " action_writes=%" PRIu64 " nitro_writes=%" PRIu64
      " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
      " semantic_errors=%" PRIu64 " report=%s\n",
      fc3_success, fc3_tail.report.flags, fc3_tail.report.owner_tid,
      fc3_tail.report.dedicated_hits, fc3_tail.report.callback_close_hits,
      fc3_tail.report.accumulator_hits,
      fc3_tail.report.action_queue_writes, fc3_tail.report.nitro_writes,
      fc3_tail.report.read_errors, fc3_tail.report.ptrace_errors,
      fc3_tail.report.semantic_errors, argv[kFc3ReportIndex]);
#endif
  return success && fc3_success && report_ok && fc3_report_ok ? 0 : 8;
#else
  return success && report_ok ? 0 : 8;
#endif
}
#endif

#else

#if A9TAS_FC2_CONTROLLER_NO_MAIN == 0
int main() {
  std::puts("FC2_BUILD_ONLY runtime=disabled return=-100 device_access=0 "
            "attach=0 game_writes=0 registration=0 removal=0");
  return 100;
}
#endif

#endif
