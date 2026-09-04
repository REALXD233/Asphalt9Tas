// FC-1 one-frame shadow-vptr transaction controller.
//
// The default build is inert and returns before parsing arguments or touching
// /proc.  Live logic is compiled only into an unlinked review object when
// A9TAS_FC1_LIVE_CANDIDATE=1.  The separately guarded runner remains offline
// by default and requires explicit preparation/probe acknowledgements.

#include <cstdio>

#ifndef A9TAS_FC1_LIVE_CANDIDATE
#define A9TAS_FC1_LIVE_CANDIDATE 0
#endif

#if A9TAS_FC1_LIVE_CANDIDATE != 0 && A9TAS_FC1_LIVE_CANDIDATE != 1
#error "A9TAS_FC1_LIVE_CANDIDATE must be 0 or 1"
#endif

#if A9TAS_FC1_LIVE_CANDIDATE == 1

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"
#include "fc1_payload_elf_resolver_v1.h"

#include <array>
#include <string>

namespace {

constexpr std::uintptr_t kPhysicsContextVtable = 0x8103830;
constexpr std::uintptr_t kCarPhysicsPrimaryVtable = 0x7EE8D18;
constexpr std::uintptr_t kPrimaryPrefix = 0x7EE8CC0;
constexpr std::uintptr_t kOriginalCallback = 0x367D66C;
constexpr std::size_t kShadowSize = 0x908;
constexpr std::size_t kPrefixSize = 0x58;
constexpr std::size_t kCallbackSlot = 0x10;
constexpr std::uintptr_t kCallbackListOffset = 0x180;
constexpr std::uintptr_t kCallbackFlagsOffset = 0x1A0;
constexpr std::uint64_t kMinimumTimeoutMs = 100;
constexpr std::uint64_t kMaximumTimeoutMs = 2000;
constexpr std::uint64_t kDefaultFrameDeadlineNs = 100000000ULL;
constexpr char kAcknowledgement[] =
    "I_ACCEPT_FC1_ONE_FRAME_PASSTHROUGH_NO_ACTION_V1";
constexpr char kPayloadName[] =
    "liba9tas_frame_callback_bootstrap_v1_build_only.so";
constexpr std::uint8_t kWrapperSignature[24] = {
    0xfd, 0x7b, 0xbb, 0xa9, 0xfa, 0x67, 0x01, 0xa9,
    0xf8, 0x5f, 0x02, 0xa9, 0xf6, 0x57, 0x03, 0xa9,
    0xf4, 0x4f, 0x04, 0xa9, 0xfd, 0x03, 0x00, 0x91,
};

enum ReportFlag : std::uint32_t {
  kTargetVerified = 1u << 0,
  kIdentityPinned = 1u << 1,
  kPayloadPrepared = 1u << 2,
  kDispatchOpenSeen = 1u << 3,
  kSingleSwapWritten = 1u << 4,
  kWrapperAcknowledged = 1u << 5,
  kDispatchCloseSeen = 1u << 6,
  kOriginalVptrFinal = 1u << 7,
  kCleanDetach = 1u << 8,
  kProcessAlive = 1u << 9,
  kTracerClear = 1u << 10,
};

enum class Phase : std::uint32_t {
  kPreflight = 0,
  kWaitingOpen = 1,
  kOthersFrozen = 2,
  kShadowInstalled = 3,
  kWaitingClose = 4,
  kClosed = 5,
  kRollback = 6,
  kDetached = 7,
};

// Failure-only diagnostics stored in Fc1Report::reserved0.  A successful
// report must keep this field zero.  These bits do not relax any gate; they
// make a fail-closed live result attributable without another speculative
// retry.
enum OpenRejectReason : std::uint32_t {
  kRejectUnexpectedWaitEvent = 1u << 0,
  kRejectWrongStopSignal = 1u << 1,
  kRejectDr6Read = 1u << 2,
  kRejectDr0NotSet = 1u << 3,
  kRejectFlagsRead = 1u << 4,
  kRejectThreadNameRead = 1u << 5,
  kRejectThreadNamePrefix = 1u << 6,
  kRejectMapsRead = 1u << 7,
  kRejectCallbackListRead = 1u << 8,
  kRejectDispatchNotOpen = 1u << 9,
  kRejectUniqueObject = 1u << 10,
  kRejectObjectVptrRead = 1u << 11,
  kRejectObjectVptrMismatch = 1u << 12,
  kRejectUnexpectedWaitState = 1u << 13,
  kRejectThreadSignaled = 1u << 14,
};

struct alignas(64) PayloadControl {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uintptr_t expected_object;
  std::uintptr_t original_vptr;
  std::uintptr_t shadow_vptr;
  std::uintptr_t original_callback;
  std::uint64_t reserved[2];
};

struct alignas(64) PayloadEvidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t clean_returns;
  std::uint64_t recursive_entries;
  std::uint64_t failures;
  std::uintptr_t last_object;
  std::uintptr_t last_token;
  std::uintptr_t observed_vptr;
  std::uintptr_t restored_vptr;
  std::int64_t last_result;
  std::int32_t last_status;
  std::uint32_t reserved;
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

#pragma pack(push, 1)
struct Fc1Report {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t final_phase;
  std::uint64_t pid;
  std::uint64_t library_base;
  std::uint64_t physics_context;
  std::uint64_t callback_list;
  std::uint64_t callback_flags;
  std::uint64_t car_physics_state;
  std::uint64_t original_vptr;
  std::uint64_t shadow_vptr;
  std::uint64_t payload_shadow;
  std::uint64_t payload_control;
  std::uint64_t payload_evidence;
  std::uint64_t wrapper;
  std::int32_t owner_tid;
  std::uint32_t initial_threads;
  std::uint32_t final_threads;
  std::uint32_t reserved0;
  std::uint64_t open_ns;
  std::uint64_t close_ns;
  std::uint64_t game_write_attempts;
  std::uint64_t game_write_failures;
  std::uint64_t rollback_attempts;
  std::uint64_t rollback_failures;
  std::uint64_t read_errors;
  std::uint64_t ptrace_errors;
  std::uint64_t semantic_errors;
  PayloadEvidence evidence;
};
#pragma pack(pop)

static_assert(sizeof(PayloadControl) == 64, "FC-1 control ABI");
static_assert(sizeof(PayloadEvidence) == 128, "FC-1 evidence ABI");
static_assert(sizeof(CallbackListHeader) == 56, "callback list ABI");
static_assert(sizeof(CallbackEntry) == 16, "callback entry ABI");
static_assert(offsetof(CallbackListHeader, dispatching) == 0x20,
              "callback dispatching offset");
static_assert(offsetof(CallbackListHeader, member_function) == 0x28,
              "callback member-function offset");
static_assert(sizeof(Fc1Report) == 336, "FC-1 report ABI");

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

bool InstallShadowVptrWhileFrozen(int mem, std::uintptr_t object,
                                  std::uintptr_t original_vptr,
                                  std::uintptr_t shadow_vptr,
                                  Fc1Report* report) {
  ++report->game_write_attempts;
  const ssize_t written =
      pwrite(mem, &shadow_vptr, sizeof(shadow_vptr),
             static_cast<off_t>(object));
  std::uintptr_t observed = 0;
  const bool installed =
      written == static_cast<ssize_t>(sizeof(shadow_vptr)) &&
      ReadExact(mem, object, &observed, sizeof(observed)) &&
      observed == shadow_vptr;
  if (installed) return true;

  ++report->game_write_failures;
  ++report->rollback_attempts;
  if (!WriteExactVerified(mem, object, &original_vptr, sizeof(original_vptr))) {
    ++report->rollback_failures;
    return false;
  }
  return false;
}

bool MappingHas(const Mapping* mapping, char permission) {
  if (!mapping) return false;
  const char* found = std::strchr(mapping->perms, permission);
  return found != nullptr;
}

bool PayloadMapping(const Mapping* mapping) {
  return mapping && mapping->path.find(kPayloadName) != std::string::npos;
}

bool ReadCallbackList(int mem, const std::vector<Mapping>& maps,
                      std::uintptr_t list, CallbackListHeader* output) {
  CallbackListHeader header{};
  if (!ReadExact(mem, list, &header, sizeof(header)) ||
      header.begin == 0 || header.begin > header.active_end ||
      header.active_end > header.end || header.end > header.capacity_end ||
      ((header.active_end - header.begin) % sizeof(CallbackEntry)) != 0 ||
      header.active_end - header.begin > 4096 * sizeof(CallbackEntry) ||
      !FindMapping(maps, header.begin,
                   static_cast<std::size_t>(header.active_end - header.begin)) ||
      header.dispatching > 1 || header.deferred > 1 ||
      header.member_function != 0x10 || header.this_adjustment != 1)
    return false;
  *output = header;
  return true;
}

bool HasUniqueActiveObject(int mem, const CallbackListHeader& header,
                           std::uintptr_t object) {
  std::uint32_t matches = 0;
  for (std::uintptr_t cursor = header.begin; cursor < header.active_end;
       cursor += sizeof(CallbackEntry)) {
    CallbackEntry entry{};
    if (!ReadExact(mem, cursor, &entry, sizeof(entry))) return false;
    if (entry.object == object) {
      if (entry.reserved != 0) return false;
      ++matches;
    }
  }
  return matches == 1;
}

bool ValidatePinnedIdentity(int mem, pid_t pid, std::uintptr_t base,
                            std::uintptr_t context,
                            std::uintptr_t object,
                            bool require_closed,
                            CallbackListHeader* list_out) {
  std::vector<Mapping> maps;
  std::uintptr_t adapter = 0;
  std::uintptr_t world = 0;
  std::uintptr_t context_vptr = 0;
  std::uintptr_t object_vptr = 0;
  if (!ReadMaps(pid, &maps) ||
      !ValidatePhysicsContext(mem, maps, base, context, &adapter, &world) ||
      !ReadExact(mem, context, &context_vptr, sizeof(context_vptr)) ||
      !ReadExact(mem, object, &object_vptr, sizeof(object_vptr)) ||
      context_vptr != base + kPhysicsContextVtable ||
      object_vptr != base + kCarPhysicsPrimaryVtable)
    return false;
  CallbackListHeader header{};
  if (!ReadCallbackList(mem, maps, context + kCallbackListOffset, &header) ||
      (require_closed && (header.dispatching != 0 || header.deferred != 0)) ||
      !HasUniqueActiveObject(mem, header, object))
    return false;
  *list_out = header;
  return true;
}

bool ResolveAndPinIdentity(pid_t pid, int mem, std::uintptr_t base,
                           std::uintptr_t* context_out,
                           std::uintptr_t* object_out) {
  std::uintptr_t context = 0;
  std::uintptr_t adapter = 0;
  std::uintptr_t world = 0;
  if (!ResolvePhysicsContext(pid, mem, base, 0, &context, &adapter, &world))
    return false;
  a9tas::vehicle_state_v1::Layout vehicle{};
  if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle))
    return false;
  CallbackListHeader header{};
  if (!ValidatePinnedIdentity(mem, pid, base, context, vehicle.physics_base,
                              true, &header))
    return false;
  *context_out = context;
  *object_out = vehicle.physics_base;
  return true;
}

bool ValidatePayloadRanges(pid_t pid, std::uintptr_t shadow,
                           std::uintptr_t control, std::uintptr_t evidence,
                           std::uintptr_t wrapper, int mem) {
  std::vector<Mapping> maps;
  if (!ReadMaps(pid, &maps) || (shadow & 63u) != 0 || (control & 63u) != 0 ||
      (evidence & 63u) != 0 || (wrapper & 3u) != 0)
    return false;
  const Mapping* shadow_map = FindMapping(maps, shadow, kShadowSize);
  const Mapping* control_map = FindMapping(maps, control, sizeof(PayloadControl));
  const Mapping* evidence_map =
      FindMapping(maps, evidence, sizeof(PayloadEvidence));
  const Mapping* wrapper_map = FindMapping(maps, wrapper, 4);
  std::uint8_t wrapper_bytes[sizeof(kWrapperSignature)]{};
  return PayloadMapping(shadow_map) && PayloadMapping(control_map) &&
         PayloadMapping(evidence_map) && PayloadMapping(wrapper_map) &&
         MappingHas(shadow_map, 'r') && MappingHas(shadow_map, 'w') &&
         MappingHas(control_map, 'r') && MappingHas(control_map, 'w') &&
         MappingHas(evidence_map, 'r') && MappingHas(evidence_map, 'w') &&
         MappingHas(wrapper_map, 'r') && !MappingHas(wrapper_map, 'w') &&
         ReadExact(mem, wrapper, wrapper_bytes, sizeof(wrapper_bytes)) &&
         std::memcmp(wrapper_bytes, kWrapperSignature,
                     sizeof(kWrapperSignature)) == 0;
}

bool PreparePayload(int mem, std::uintptr_t base, std::uintptr_t object,
                    std::uintptr_t shadow, std::uintptr_t control_address,
                    std::uintptr_t evidence_address, std::uintptr_t wrapper,
                    std::uintptr_t* shadow_vptr_out) {
  std::array<std::uint8_t, kShadowSize> table{};
  if (!ReadExact(mem, base + kPrimaryPrefix, table.data(), table.size()) ||
      std::memcmp(table.data(), kExpectedPrefix, sizeof(kExpectedPrefix)) != 0)
    return false;
  // kExpectedSlots are RVAs from the hash-pinned guest ELF.  The live vtable
  // has already received its RELATIVE relocations, so validate each slot
  // against lib_base + RVA rather than comparing live pointers with raw RVAs.
  for (std::size_t index = 0; index < std::size(kExpectedSlots); ++index) {
    std::uintptr_t live_slot = 0;
    std::memcpy(&live_slot,
                table.data() + kPrefixSize + index * sizeof(live_slot),
                sizeof(live_slot));
    if (kExpectedSlots[index] > UINTPTR_MAX - base ||
        live_slot != base + kExpectedSlots[index])
      return false;
  }
  std::uintptr_t current_vptr = 0;
  PayloadControl initial_control{};
  PayloadEvidence initial_evidence{};
  if (!ReadExact(mem, object, &current_vptr, sizeof(current_vptr)) ||
      current_vptr != base + kCarPhysicsPrimaryVtable ||
      !ReadExact(mem, control_address, &initial_control,
                 sizeof(initial_control)) ||
      !ReadExact(mem, evidence_address, &initial_evidence,
                 sizeof(initial_evidence)))
    return false;
  const char control_magic[8] = {'A', '9', 'F', 'C', '0', 'C', '1', 0};
  const char evidence_magic[8] = {'A', '9', 'F', 'C', '0', 'E', '1', 0};
  if (std::memcmp(initial_control.magic, control_magic, 8) != 0 ||
      initial_control.version != 1 ||
      initial_control.size != sizeof(PayloadControl) ||
      initial_control.expected_object != 0 || initial_control.original_vptr != 0 ||
      initial_control.shadow_vptr != 0 ||
      initial_control.original_callback != 0 ||
      std::memcmp(initial_evidence.magic, evidence_magic, 8) != 0 ||
      initial_evidence.version != 1 ||
      initial_evidence.size != sizeof(PayloadEvidence) ||
      initial_evidence.wrapper_entries != 0 || initial_evidence.original_calls != 0 ||
      initial_evidence.clean_returns != 0 ||
      initial_evidence.recursive_entries != 0 || initial_evidence.failures != 0)
    return false;

  const std::uintptr_t shadow_vptr = shadow + kPrefixSize;
  std::memcpy(table.data() + kPrefixSize + kCallbackSlot, &wrapper,
              sizeof(wrapper));
  PayloadEvidence evidence{};
  std::memcpy(evidence.magic, evidence_magic, 8);
  evidence.version = 1;
  evidence.size = sizeof(evidence);
  PayloadControl control{};
  std::memcpy(control.magic, control_magic, 8);
  control.version = 1;
  control.size = sizeof(control);
  control.expected_object = object;
  control.original_vptr = current_vptr;
  control.shadow_vptr = shadow_vptr;
  control.original_callback = base + kOriginalCallback;

  if (!WriteExactVerified(mem, shadow, table.data(), table.size()) ||
      !WriteExactVerified(mem, evidence_address, &evidence, sizeof(evidence)) ||
      !WriteExactVerified(mem, control_address, &control, sizeof(control)))
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

unsigned long DispatchWrite2Dr7() {
  return 1UL | (1UL << 16) | (1UL << 18);
}

WatchedThread* FindWatched(std::vector<WatchedThread>* threads, pid_t tid) {
  for (auto& thread : *threads)
    if (thread.tid == tid) return &thread;
  return nullptr;
}

bool AttachOneFc1(pid_t tid, std::uintptr_t flags_address, bool run,
                  WatchedThread* output) {
  WatchedThread thread{tid, true, false, {}};
  if (ptrace(PTRACE_SEIZE, tid, nullptr, nullptr) == -1) return false;
  if (!StopThread(tid)) {
    (void)ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    return false;
  }
  thread.stopped = true;
  if (!ReadDebugState(tid, &thread.original) ||
      !DebugUnused(thread.original) ||
      !PokeDebug(tid, 0, flags_address) || !PokeDebug(tid, 6, 0) ||
      !PokeDebug(tid, 7, DispatchWrite2Dr7()) ||
      (run && !ContinueThread(tid))) {
    (void)RestoreDebug(tid, thread.original);
    (void)ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    return false;
  }
  thread.stopped = !run;
  *output = thread;
  return true;
}

bool AttachCurrentThreads(pid_t pid, std::uintptr_t flags_address, bool run,
                          std::vector<WatchedThread>* threads) {
  bool ok = true;
  for (const pid_t tid : ListThreads(pid)) {
    if (FindWatched(threads, tid)) continue;
    WatchedThread thread{};
    if (!AttachOneFc1(tid, flags_address, run, &thread)) {
      ok = false;
      continue;
    }
    threads->push_back(thread);
  }
  return ok;
}

bool FreezeAllExcept(pid_t pid, pid_t owner, std::uintptr_t flags_address,
                     std::vector<WatchedThread>* threads) {
  for (auto& thread : *threads) {
    if (!thread.live || thread.tid == owner || thread.stopped) continue;
    if (!StopThread(thread.tid)) return false;
    thread.stopped = true;
  }
  for (int pass = 0; pass < 3; ++pass) {
    const std::size_t before = threads->size();
    if (!AttachCurrentThreads(pid, flags_address, false, threads)) return false;
    for (auto& thread : *threads) {
      if (!thread.live || thread.tid == owner || thread.stopped) continue;
      if (!StopThread(thread.tid)) return false;
      thread.stopped = true;
    }
    if (threads->size() == before) return true;
  }
  return false;
}

bool ReadThreadNameFc1(pid_t pid, pid_t tid, std::string* output) {
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
                         std::uint32_t* detached) {
  bool ok = true;
  *detached = 0;
  for (auto& thread : *threads) {
    if (!thread.live) continue;
    if (!thread.stopped && !StopThread(thread.tid)) {
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

bool EvidenceComplete(const PayloadEvidence& evidence,
                      std::uintptr_t object, std::uintptr_t shadow_vptr,
                      std::uintptr_t original_vptr) {
  const char magic[8] = {'A', '9', 'F', 'C', '0', 'E', '1', 0};
  return std::memcmp(evidence.magic, magic, 8) == 0 &&
         evidence.version == 1 && evidence.size == sizeof(evidence) &&
         evidence.wrapper_entries == 1 && evidence.original_calls == 1 &&
         evidence.clean_returns == 1 && evidence.recursive_entries == 0 &&
         evidence.failures == 0 && evidence.last_object == object &&
         evidence.last_token != 0 && evidence.observed_vptr == shadow_vptr &&
         evidence.restored_vptr == original_vptr && evidence.last_status == 0;
}

bool ConditionalRollback(int mem, std::uintptr_t object,
                         std::uintptr_t shadow_vptr,
                         std::uintptr_t original_vptr, Fc1Report* report) {
  std::uintptr_t current = 0;
  if (!ReadExact(mem, object, &current, sizeof(current))) {
    ++report->read_errors;
    return false;
  }
  if (current == original_vptr) return true;
  if (current != shadow_vptr) {
    ++report->semantic_errors;
    return false;
  }
  ++report->rollback_attempts;
  if (!WriteExactVerified(mem, object, &original_vptr, sizeof(original_vptr))) {
    ++report->rollback_failures;
    return false;
  }
  return true;
}

bool WriteReport(const char* path, const Fc1Report& report) {
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
}

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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX TIMEOUT_MS REPORT ACK\n",
                 argv[0]);
    return 2;
  }
  if (std::strcmp(argv[5], kAcknowledgement) != 0) {
    std::fprintf(stderr, "exact acknowledgement missing; no process opened\n");
    return 2;
  }
  std::uint64_t values[3]{};
  const int bases[3] = {10, 16, 10};
  for (int index = 0; index < 3; ++index) {
    if (!ParseUnsigned(argv[index + 1], bases[index], &values[index])) {
      std::fprintf(stderr, "invalid numeric argument\n");
      return 2;
    }
  }
  if (values[0] == 0 || values[0] > static_cast<std::uint64_t>(INT32_MAX) ||
      values[1] == 0 || values[2] < kMinimumTimeoutMs ||
      values[2] > kMaximumTimeoutMs || access(argv[4], F_OK) == 0) {
    std::fprintf(stderr, "invalid range or report already exists\n");
    return 2;
  }

  const pid_t pid = static_cast<pid_t>(values[0]);
  const auto base = static_cast<std::uintptr_t>(values[1]);
  const auto timeout_ns = values[2] * 1000000ULL;

  Fc1Report report{};
  const char report_magic[8] = {'A', '9', 'F', 'C', '1', 'R', '1', 0};
  std::memcpy(report.magic, report_magic, 8);
  report.version = 1;
  report.size = sizeof(report);
  report.final_phase = static_cast<std::uint32_t>(Phase::kPreflight);
  report.pid = static_cast<std::uint64_t>(pid);
  report.library_base = base;

  if (!VerifyTargetBuild(pid, base)) {
    std::fprintf(stderr, "target or passive payload validation failed\n");
    return 3;
  }

  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return 4;
  a9tas::fc1_payload_elf_v1::Layout payload{};
  if (!a9tas::fc1_payload_elf_v1::Resolve(pid, mem, &payload)) {
    close(mem);
    std::fprintf(stderr, "hash-pinned ARM64 ELF symbol resolution failed\n");
    return 3;
  }
  const std::uintptr_t shadow = payload.shadow;
  const std::uintptr_t control = payload.control;
  const std::uintptr_t evidence_address = payload.evidence;
  const std::uintptr_t wrapper = payload.wrapper;
  report.payload_shadow = shadow;
  report.payload_control = control;
  report.payload_evidence = evidence_address;
  report.wrapper = wrapper;
  if (!ValidatePayloadRanges(pid, shadow, control, evidence_address, wrapper,
                             mem)) {
    close(mem);
    std::fprintf(stderr, "passive payload range/signature validation failed\n");
    return 3;
  }
  report.flags |= kTargetVerified;
  std::uintptr_t context = 0;
  std::uintptr_t object = 0;
  if (!ResolveAndPinIdentity(pid, mem, base, &context, &object)) {
    close(mem);
    std::fprintf(stderr, "untraced read-only identity pinning failed\n");
    return 3;
  }
  close(mem);
  report.physics_context = context;
  report.callback_list = context + kCallbackListOffset;
  report.callback_flags = context + kCallbackFlagsOffset;
  report.car_physics_state = object;
  report.original_vptr = base + kCarPhysicsPrimaryVtable;
  report.flags |= kIdentityPinned;

  mem = open(mem_path, O_RDWR | O_CLOEXEC);
  if (mem < 0) return 4;
  CallbackListHeader pinned_header{};
  if (!ValidatePinnedIdentity(mem, pid, base, context, object, true,
                              &pinned_header) ||
      !PreparePayload(mem, base, object, shadow, control, evidence_address,
                      wrapper, &report.shadow_vptr)) {
    close(mem);
    std::fprintf(stderr, "payload preparation failed before attach\n");
    return 3;
  }
  report.flags |= kPayloadPrepared;

  std::vector<WatchedThread> threads;
  if (!AttachCurrentThreads(pid, report.callback_flags, true, &threads) ||
      threads.empty()) {
    ++report.ptrace_errors;
  }
  report.initial_threads = static_cast<std::uint32_t>(threads.size());
  report.final_phase = static_cast<std::uint32_t>(Phase::kWaitingOpen);

  WatchedThread* owner = nullptr;
  std::uint32_t retired_threads = 0;
  const std::uint64_t wait_deadline = MonotonicNs() + timeout_ns;
  std::uint64_t next_rescan = MonotonicNs() + 250000000ULL;
  while (report.ptrace_errors == 0 && report.semantic_errors == 0 &&
         MonotonicNs() < wait_deadline && owner == nullptr) {
    if (MonotonicNs() >= next_rescan) {
      if (!AttachCurrentThreads(pid, report.callback_flags, true, &threads))
        ++report.ptrace_errors;
      next_rescan = MonotonicNs() + 250000000ULL;
    }
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
    // Android creates and retires short-lived worker threads during normal
    // play.  A clean exit is bookkeeping, not a callback semantic failure.
    // This is the same lifecycle rule used by the proven read-only HWBP
    // observers.  Nonzero exits, signal deaths, unknown stopped TIDs and every
    // other wait state remain fail-closed.
    if (WIFEXITED(status)) {
      if (WEXITSTATUS(status) != 0) {
        report.reserved0 |= kRejectUnexpectedWaitState;
        std::fprintf(stderr,
                     "FC1_WAIT_REJECT reason=0x%x tid=%d status=0x%x\n",
                     report.reserved0, static_cast<int>(tid), status);
        ++report.semantic_errors;
        break;
      }
      // A cleanly exited unknown TID can be left by the enumerate/seize race:
      // it is already dead, owns no restorable debug state and cannot be the
      // stopped callback writer.  Only known live threads enter our accounting.
      if (thread && thread->live) {
        thread->live = false;
        thread->stopped = false;
        ++retired_threads;
      }
      continue;
    }
    if (!thread || !thread->live) {
      report.reserved0 |= kRejectUnexpectedWaitEvent;
      std::fprintf(stderr, "FC1_WAIT_REJECT reason=0x%x tid=%d status=0x%x\n",
                   report.reserved0, static_cast<int>(tid), status);
      ++report.semantic_errors;
      break;
    }
    if (WIFSIGNALED(status)) {
      report.reserved0 |= kRejectThreadSignaled;
      std::fprintf(stderr, "FC1_WAIT_REJECT reason=0x%x tid=%d status=0x%x\n",
                   report.reserved0, static_cast<int>(tid), status);
      ++report.semantic_errors;
      break;
    }
    if (!WIFSTOPPED(status)) {
      report.reserved0 |= kRejectUnexpectedWaitState;
      std::fprintf(stderr, "FC1_WAIT_REJECT reason=0x%x tid=%d status=0x%x\n",
                   report.reserved0, static_cast<int>(tid), status);
      ++report.semantic_errors;
      break;
    }
    thread->stopped = true;
    unsigned long dr6 = 0;
    std::uint16_t flags = 0;
    const bool signal_ok = WSTOPSIG(status) == SIGTRAP;
    const bool dr6_ok = signal_ok && PeekDebug(tid, 6, &dr6);
    const bool dr0_set = dr6_ok && (dr6 & 1UL) != 0;
    const bool flags_ok =
        dr0_set && ReadExact(mem, report.callback_flags, &flags, sizeof(flags));
    if (!signal_ok || !dr6_ok || !dr0_set || !flags_ok) {
      if (!signal_ok) report.reserved0 |= kRejectWrongStopSignal;
      if (signal_ok && !dr6_ok) report.reserved0 |= kRejectDr6Read;
      if (dr6_ok && !dr0_set) report.reserved0 |= kRejectDr0NotSet;
      if (dr0_set && !flags_ok) report.reserved0 |= kRejectFlagsRead;
      ++report.semantic_errors;
      break;
    }
    if ((flags & 0xFFu) == 1) {
      std::string name;
      CallbackListHeader open_header{};
      std::uintptr_t current_vptr = 0;
      std::vector<Mapping> maps;
      const bool name_read = ReadThreadNameFc1(pid, tid, &name);
      const bool name_prefix = name_read && name.rfind("Thread-", 0) == 0;
      const bool maps_read = ReadMaps(pid, &maps);
      const bool list_read = maps_read &&
          ReadCallbackList(mem, maps, report.callback_list, &open_header);
      const bool dispatch_open = list_read && open_header.dispatching == 1;
      const bool unique_object =
          dispatch_open && HasUniqueActiveObject(mem, open_header, object);
      const bool vptr_read =
          ReadExact(mem, object, &current_vptr, sizeof(current_vptr));
      const bool vptr_matches =
          vptr_read && current_vptr == report.original_vptr;
      if (!name_read || !name_prefix || !maps_read || !list_read ||
          !dispatch_open || !unique_object || !vptr_read || !vptr_matches) {
        if (!name_read) report.reserved0 |= kRejectThreadNameRead;
        if (name_read && !name_prefix)
          report.reserved0 |= kRejectThreadNamePrefix;
        if (!maps_read) report.reserved0 |= kRejectMapsRead;
        if (maps_read && !list_read)
          report.reserved0 |= kRejectCallbackListRead;
        if (list_read && !dispatch_open)
          report.reserved0 |= kRejectDispatchNotOpen;
        if (dispatch_open && !unique_object)
          report.reserved0 |= kRejectUniqueObject;
        if (!vptr_read) report.reserved0 |= kRejectObjectVptrRead;
        if (vptr_read && !vptr_matches)
          report.reserved0 |= kRejectObjectVptrMismatch;
        std::fprintf(stderr, "FC1_OPEN_REJECT reason=0x%x tid=%d name=%s\n",
                     report.reserved0, static_cast<int>(tid),
                     name_read ? name.c_str() : "<unreadable>");
        ++report.semantic_errors;
        break;
      }
      owner = thread;
      report.owner_tid = tid;
      report.open_ns = MonotonicNs();
      report.flags |= kDispatchOpenSeen;
    } else {
      if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
        ++report.ptrace_errors;
        break;
      }
      thread->stopped = false;
    }
  }

  if (owner && report.ptrace_errors == 0 && report.semantic_errors == 0) {
    const pid_t transaction_tid = owner->tid;
    if (!FreezeAllExcept(pid, transaction_tid, report.callback_flags,
                         &threads)) {
      ++report.ptrace_errors;
    } else {
      // FreezeAllExcept may append newly discovered threads and reallocate the
      // vector, so never retain the pre-freeze element pointer.
      owner = FindWatched(&threads, transaction_tid);
      report.final_phase = static_cast<std::uint32_t>(Phase::kOthersFrozen);
      std::vector<Mapping> maps;
      CallbackListHeader open_header{};
      std::uintptr_t current_vptr = 0;
      if (owner == nullptr || !owner->stopped ||
          !ReadMaps(pid, &maps) ||
          !ReadCallbackList(mem, maps, report.callback_list, &open_header) ||
          open_header.dispatching != 1 ||
          !HasUniqueActiveObject(mem, open_header, object) ||
          !ReadExact(mem, object, &current_vptr, sizeof(current_vptr)) ||
          current_vptr != report.original_vptr) {
        ++report.semantic_errors;
      } else {
        const std::uintptr_t shadow_vptr = report.shadow_vptr;
        if (InstallShadowVptrWhileFrozen(mem, object, report.original_vptr,
                                         shadow_vptr, &report)) {
          report.flags |= kSingleSwapWritten;
          report.final_phase = static_cast<std::uint32_t>(Phase::kShadowInstalled);
          if (!PokeDebug(owner->tid, 6, 0) || !ContinueThread(owner->tid)) {
            ++report.ptrace_errors;
          } else {
            owner->stopped = false;
            report.final_phase = static_cast<std::uint32_t>(Phase::kWaitingClose);
            const std::uint64_t frame_deadline =
                std::min<std::uint64_t>(wait_deadline,
                    report.open_ns + kDefaultFrameDeadlineNs);
            while (MonotonicNs() < frame_deadline) {
              int status = 0;
              const pid_t tid = waitpid(owner->tid, &status,
                                        __WALL | WNOHANG);
              if (tid == 0) {
                usleep(100);
                continue;
              }
              if (tid < 0) {
                if (errno == EINTR) continue;
                ++report.ptrace_errors;
                break;
              }
              owner->stopped = WIFSTOPPED(status);
              unsigned long dr6 = 0;
              std::uint16_t flags = 0;
              if (!owner->stopped || WSTOPSIG(status) != SIGTRAP ||
                  !PeekDebug(owner->tid, 6, &dr6) || (dr6 & 1UL) == 0 ||
                  !ReadExact(mem, report.callback_flags, &flags,
                             sizeof(flags)) || (flags & 0xFFu) != 0) {
                ++report.semantic_errors;
              } else {
                report.close_ns = MonotonicNs();
                report.flags |= kDispatchCloseSeen;
                report.final_phase = static_cast<std::uint32_t>(Phase::kClosed);
              }
              break;
            }
            if (!owner->stopped && !StopThread(owner->tid))
              ++report.ptrace_errors;
            else
              owner->stopped = true;
          }
        }
      }
    }
  }

  if (ReadExact(mem, evidence_address, &report.evidence,
                sizeof(report.evidence)) &&
      EvidenceComplete(report.evidence, object, report.shadow_vptr,
                       report.original_vptr))
    report.flags |= kWrapperAcknowledged;
  else
    ++report.semantic_errors;

  report.final_phase = static_cast<std::uint32_t>(Phase::kRollback);
  if (ConditionalRollback(mem, object, report.shadow_vptr,
                          report.original_vptr, &report)) {
    std::uintptr_t final_vptr = 0;
    if (ReadExact(mem, object, &final_vptr, sizeof(final_vptr)) &&
        final_vptr == report.original_vptr)
      report.flags |= kOriginalVptrFinal;
    else
      ++report.semantic_errors;
  }

  std::uint32_t detached = 0;
  if (RestoreAndDetachAll(&threads, &detached) &&
      static_cast<std::size_t>(detached + retired_threads) == threads.size())
    report.flags |= kCleanDetach;
  else
    ++report.ptrace_errors;
  report.final_threads = detached + retired_threads;
  report.final_phase = static_cast<std::uint32_t>(Phase::kDetached);
  close(mem);

  if (kill(pid, 0) == 0) report.flags |= kProcessAlive;
  std::uint32_t tracer_pid = UINT32_MAX;
  if (ReadTracerPid(pid, &tracer_pid) && tracer_pid == 0)
    report.flags |= kTracerClear;

  const std::uint32_t required =
      kTargetVerified | kIdentityPinned | kPayloadPrepared |
      kDispatchOpenSeen | kSingleSwapWritten | kWrapperAcknowledged |
      kDispatchCloseSeen | kOriginalVptrFinal | kCleanDetach |
      kProcessAlive | kTracerClear;
  const bool success = report.flags == required &&
                       report.game_write_attempts == 1 &&
                       report.game_write_failures == 0 &&
                       report.rollback_attempts == 0 &&
                       report.rollback_failures == 0 &&
                       report.read_errors == 0 && report.ptrace_errors == 0 &&
                       report.semantic_errors == 0 && report.open_ns != 0 &&
                       report.close_ns >= report.open_ns;
  const bool report_ok = WriteReport(argv[4], report);
  std::printf(
      "FC1_DONE success=%u flags=0x%x owner=%d game_writes=%" PRIu64
      " rollbacks=%" PRIu64 " read_errors=%" PRIu64
      " ptrace_errors=%" PRIu64 " semantic_errors=%" PRIu64
      " clean_detach=%u report=%s\n",
      success, report.flags, report.owner_tid, report.game_write_attempts,
      report.rollback_attempts, report.read_errors, report.ptrace_errors,
      report.semantic_errors, (report.flags & kCleanDetach) != 0, argv[4]);
  return success && report_ok ? 0 : 8;
}

#else

int main() {
  std::puts("FC1_BUILD_ONLY runtime=disabled return=-100 device_access=0 "
            "attach=0 game_writes=0");
  return 100;
}

#endif
