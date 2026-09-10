#include "g4_multi_hook_runtime_v1.h"
#include "barrel_stabilization_replay_core_v1.h"
#include "barrel_prng_v1.h"
#include "g8_runtime_build_profile_v1.h"
#include "realtime_tick_budget_v1.h"
#include "completed_frame_scope_v1.h"

// Candidate only until controller/archive sparse-packet paths are integrated.
// Default builds preserve the exact original FrameEvent wrapper call sequence.
#ifndef A9TAS_EXPERIMENTAL_HIGH_REFRESH
#define A9TAS_EXPERIMENTAL_HIGH_REFRESH 1
#endif

#include <android/log.h>
#include <elf.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bridge = a9tas::g4_g3_adapter_v1;
namespace g3 = a9tas::g3_boundary_adapter_v1;
namespace coordinator = a9tas::g3_tick_coordinator_v1;
namespace g4 = a9tas::g4_input_action_v1;
namespace recording = a9tas::unified_tick_v1;
namespace barrel = a9tas::barrel_stabilization_replay_v1;
namespace barrel_prng = a9tas::barrel_prng_v1;
namespace build_profile = a9tas::g8_runtime_build_profile_v1;
using namespace a9tas::g4_multi_hook_runtime_v1;

namespace {

a9tas::realtime_tick_budget_v1::Budget g_realtime_budget{};

constexpr const char* kTag = "A9TAS_G4";
constexpr const char* kGameBasename = "libAsphalt9.so";
constexpr std::size_t kPatchSize = 16;
// Random helpers replace the whole leaf call rather than tail-calling the
// original.  Use the same literal absolute jump as the ordinary hooks: under
// NativeBridge the ARM game and injected ARM payload can be mapped far more
// than ADRP's +/-4 GiB page range apart.
constexpr std::size_t kRandomPatchSize = kPatchSize;
// The first three instructions identify both random leaf helpers across the
// exact supported builds. The fourth instruction is also a BL, but its
// relative immediate is build-layout dependent. Keep replacing/restoring all
// 16 bytes while validating the fourth word structurally instead of pinning a
// different-build branch displacement.
constexpr std::size_t kRandomIdentitySize = 12;
constexpr int kLogicalCodeProtection = PROT_READ | PROT_EXEC;
constexpr std::uint32_t kArmTag = 0x47344901;
constexpr std::uint32_t kRestoreTag = 0x47344902;
constexpr std::uint32_t kInstallTag = 0x47344903;
constexpr std::uint32_t kRearmTag = 0x47344904;
constexpr std::uint32_t kFailureTag = 0x47344910;

constexpr std::uint8_t kExpectedPrologues[kHookCount][kPatchSize] = {
    {0xff, 0xc3, 0x00, 0xd1, 0xf5, 0x53, 0x01, 0xa9,
     0xf3, 0x7b, 0x02, 0xa9, 0x35, 0x11, 0x91, 0x52},
    {0xec, 0x0f, 0x17, 0xfc, 0xeb, 0x2b, 0x01, 0x6d,
     0xe9, 0x23, 0x02, 0x6d, 0xfc, 0x6f, 0x03, 0xa9},
    {0xff, 0xc3, 0x00, 0xd1, 0xf4, 0x0b, 0x00, 0xf9,
     0xf3, 0x7b, 0x02, 0xa9, 0x28, 0x00, 0x40, 0xf9},
    {0xf5, 0x53, 0xbe, 0xa9, 0xf3, 0x7b, 0x01, 0xa9,
     0x08, 0xe4, 0x46, 0x39, 0xc8, 0x00, 0x00, 0x34},
    {0xff, 0xc3, 0x00, 0xd1, 0xf4, 0x0b, 0x00, 0xf9,
     0xf3, 0x7b, 0x02, 0xa9, 0x08, 0x20, 0x47, 0x39},
    {0xff, 0xc3, 0x02, 0xd1, 0xee, 0x2b, 0x00, 0xfd,
     0xed, 0xb3, 0x05, 0x6d, 0xeb, 0xab, 0x06, 0x6d},
    {0xff, 0x03, 0x05, 0xd1, 0xef, 0x3b, 0x0c, 0x6d,
     0xed, 0x33, 0x0d, 0x6d, 0xeb, 0x2b, 0x0e, 0x6d},
    {0xff, 0x43, 0x01, 0xd1, 0xf7, 0x5b, 0x02, 0xa9,
     0xf5, 0x53, 0x03, 0xa9, 0xf3, 0x7b, 0x04, 0xa9},
};

// Shared race-enter entry.  Both the base and derived lifecycle paths reach
// this exact function; the first four instructions are replayed by the local
// trampoline before it returns to +0x10.
constexpr std::uint8_t kExpectedLifecyclePrologue[kPatchSize] = {
    0xff, 0x83, 0x03, 0xd1, 0xf7, 0x5b, 0x0b, 0xa9,
    0xf5, 0x53, 0x0c, 0xa9, 0xf3, 0x7b, 0x0d, 0xa9,
};

constexpr std::uint8_t
    kExpectedRandomPrologues[kRandomHookCount][kRandomIdentitySize] = {
        {0xfe, 0x0f, 0x1f, 0xf8, 0x08, 0xfc, 0xe7, 0xd2,
         0xc6, 0x19, 0x00, 0x94},
        {0xfe, 0x0f, 0x1f, 0xf8, 0x28, 0x00, 0x40, 0xf9,
         0xb7, 0x19, 0x00, 0x94},
};

constexpr std::uint8_t
    kExpectedSetterBodies[kInstalledSetterCount][28] = {
        {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9,
         0x08, 0x61, 0x0b, 0xd1, 0x08, 0x01, 0x40, 0xf9,
         0x08, 0x00, 0x08, 0x8b, 0x09, 0x99, 0x0c, 0xb9,
         0xc0, 0x03, 0x5f, 0xd6},
        {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9,
         0x08, 0x81, 0x0b, 0xd1, 0x08, 0x01, 0x40, 0xf9,
         0x08, 0x00, 0x08, 0x8b, 0x09, 0x9d, 0x0c, 0xb9,
         0xc0, 0x03, 0x5f, 0xd6},
};

struct MappingMetadata {
  std::uintptr_t start{};
  std::uintptr_t end{};
  int protection{};
  bool found{};
  bool query_ok{};
  bool private_mapping{};
};

struct GameImage {
  std::uintptr_t base{};
  const Elf64_Phdr* programs{};
  std::uint16_t program_count{};
  char path[1024]{};
  bool found{};
};

alignas(64) Control g_control{};
alignas(64) Evidence g_evidence{};
alignas(64) build_profile::Profile g_build_profile{};
alignas(64) bridge::StateV1 g_runtime{};
alignas(64) recording::RecordingFrameV1 g_replay_frames[kMaximumFrames]{};
alignas(64) g4::IntervalSampleV1
    g_replay_intervals[kMaximumIntervalSamples]{};
alignas(64) recording::RecordingFrameV1 g_recorded_frames[kMaximumFrames]{};
alignas(64) g4::IntervalSampleV1
    g_recorded_intervals[kMaximumIntervalSamples]{};
std::uint32_t g_recorded_interval_count{};
alignas(64) g4::PhysicsSnapshotV1 g_record_physics_snapshot{};
alignas(64) barrel::Capture g_barrel_capture{};
std::uint64_t g_barrel_capture_tick{UINT64_MAX};
barrel_prng::State g_barrel_prng{};
std::atomic_flag g_barrel_prng_lock = ATOMIC_FLAG_INIT;

struct RandomHookRuntime {
  std::uintptr_t target{};
  std::uintptr_t wrapper{};
  std::uint8_t original[kRandomPatchSize]{};
  bool original_captured{};
};

RandomHookRuntime g_random_hooks[kRandomHookCount]{};

struct TickSemanticSnapshot {
  bool valid{};
  std::uint64_t tick{};
  std::uint32_t recorded_interval_count{};
  std::uint64_t qualified_events[kHookCount]{};
  std::uint64_t core_faults{};
  std::uint64_t fixed_delta_writes{};
  std::uint64_t control_pair_reads{};
  std::uint64_t control_pair_writes{};
  std::uint64_t interval_calls{};
  std::uint64_t interval_overrides{};
  std::uint64_t interval_records{};
  std::uint64_t natural_nitro_calls{};
  std::uint64_t suppressed_nitro_calls{};
  std::uint64_t injected_nitro_calls{};
  std::uint64_t recorded_frames{};
  std::uint64_t setter_qualified_events[kSetterCount]{};
  std::uint64_t setter_overrides[kSetterCount]{};
  std::uint64_t physics_equal_frames{};
  std::uint64_t physics_corrected_frames{};
  std::uint64_t physics_correction_writes{};
  std::uint64_t physics_skipped_frames{};
  std::uint64_t barrel_rbx_overrides{};
  std::uint64_t barrel_angular_overrides{};
  std::uint64_t barrel_rbx_records{};
  std::uint64_t barrel_angular_records{};
};

TickSemanticSnapshot g_tick_semantic_start{};
std::atomic<std::int32_t> g_install_state{0};
std::atomic_flag g_runtime_lock = ATOMIC_FLAG_INIT;
// The lifecycle entry can be reached while the same thread is still inside
// the permanent logic-dispatch wrapper.  Keep a dependency-free TID/depth
// marker instead of C++ TLS: the NativeBridge carrier has no proven emulated
// TLS resolver, and a different concurrent dispatcher must remain visible via
// active_helpers rather than being accepted as same-thread nesting.
std::atomic<std::uint32_t> g_dispatcher_tid{};
std::atomic<std::uint32_t> g_dispatcher_depth{};
#if A9TAS_EXPERIMENTAL_HIGH_REFRESH
a9tas::completed_frame_scope_v1::Stack<> g_completed_frame_scopes{};
#endif

bool EnterDispatcher(std::uint32_t tid) {
  std::uint32_t owner = g_dispatcher_tid.load(std::memory_order_acquire);
  if (owner == tid) {
    g_dispatcher_depth.fetch_add(1u, std::memory_order_acq_rel);
    return true;
  }
  if (owner != 0 || !g_dispatcher_tid.compare_exchange_strong(
                        owner, tid, std::memory_order_acq_rel))
    return false;
  g_dispatcher_depth.store(1u, std::memory_order_release);
  return true;
}

void LeaveDispatcher(std::uint32_t tid, bool tracked) {
  if (!tracked || g_dispatcher_tid.load(std::memory_order_acquire) != tid)
    return;
  if (g_dispatcher_depth.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
    g_dispatcher_tid.store(0u, std::memory_order_release);
}

std::uint32_t CurrentDispatcherDepth(std::uint32_t tid) {
  if (g_dispatcher_tid.load(std::memory_order_acquire) != tid) return 0;
  return g_dispatcher_depth.load(std::memory_order_acquire);
}

extern "C" {
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_interval_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_final_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_frame_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_nitro_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_submit_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_barrel_roll_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_barrel_yaw_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_dispatch_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g4_lifecycle_tail_v1 = 0;
}

std::size_t AlignNote(std::size_t value) {
  return (value + 3U) & ~std::size_t{3U};
}

bool BuildIdMatches(std::uintptr_t base, const Elf64_Phdr* programs,
                    std::uint16_t count,
                    const std::uint8_t expected[20]) {
  for (std::uint16_t index = 0; index < count; ++index) {
    const Elf64_Phdr& program = programs[index];
    if (program.p_type != PT_NOTE || program.p_memsz < sizeof(Elf64_Nhdr))
      continue;
    const auto* cursor = reinterpret_cast<const std::uint8_t*>(
        base + program.p_vaddr);
    const auto* end = cursor + program.p_memsz;
    while (cursor + sizeof(Elf64_Nhdr) <= end) {
      Elf64_Nhdr note{};
      std::memcpy(&note, cursor, sizeof(note));
      cursor += sizeof(note);
      const std::size_t name_size = AlignNote(note.n_namesz);
      const std::size_t desc_size = AlignNote(note.n_descsz);
      if (name_size > static_cast<std::size_t>(end - cursor) ||
          desc_size > static_cast<std::size_t>(end - cursor) - name_size)
        return false;
      const auto* name = cursor;
      const auto* description = cursor + name_size;
      if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
          std::memcmp(name, "GNU", 4) == 0 && note.n_descsz == 20) {
        return std::memcmp(description, expected, 20) == 0;
      }
      cursor = description + desc_size;
    }
  }
  return false;
}

int FindGameImage(dl_phdr_info* info, size_t, void* opaque) {
  if (info == nullptr || info->dlpi_name == nullptr || opaque == nullptr)
    return 0;
  const char* slash = std::strrchr(info->dlpi_name, '/');
  const char* basename = slash == nullptr ? info->dlpi_name : slash + 1;
  if (std::strcmp(basename, kGameBasename) != 0) return 0;
  if (!BuildIdMatches(info->dlpi_addr, info->dlpi_phdr,
                      static_cast<std::uint16_t>(info->dlpi_phnum),
                      g_build_profile.build_id))
    return 0;
  std::uint64_t image_size = 0;
  for (std::uint16_t index = 0;
       index < static_cast<std::uint16_t>(info->dlpi_phnum); ++index) {
    const Elf64_Phdr& program = info->dlpi_phdr[index];
    if (program.p_type != PT_LOAD ||
        program.p_vaddr > UINT64_MAX - program.p_memsz)
      continue;
    image_size = std::max<std::uint64_t>(
        image_size, program.p_vaddr + program.p_memsz);
  }
  if (image_size != g_build_profile.image_size) return 0;
  auto* image = static_cast<GameImage*>(opaque);
  if (image->found) {
    image->base = 0;
    return 1;
  }
  image->base = info->dlpi_addr;
  image->programs = info->dlpi_phdr;
  image->program_count = static_cast<std::uint16_t>(info->dlpi_phnum);
  std::snprintf(image->path, sizeof(image->path), "%s", info->dlpi_name);
  image->found = true;
  return 0;
}

MappingMetadata QueryMapping(const void* address) {
  MappingMetadata metadata{};
  FILE* maps = std::fopen("/proc/self/maps", "re");
  if (maps == nullptr) return metadata;
  const auto target = reinterpret_cast<std::uintptr_t>(address);
  char line[2048]{};
  while (std::fgets(line, sizeof(line), maps)) {
    unsigned long long start = 0;
    unsigned long long end = 0;
    char perms[5]{};
    if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
      continue;
    if (target < start || target >= end) continue;
    metadata.start = static_cast<std::uintptr_t>(start);
    metadata.end = static_cast<std::uintptr_t>(end);
    if (perms[0] == 'r') metadata.protection |= PROT_READ;
    if (perms[1] == 'w') metadata.protection |= PROT_WRITE;
    if (perms[2] == 'x') metadata.protection |= PROT_EXEC;
    metadata.private_mapping = perms[3] == 'p';
    metadata.found = true;
    break;
  }
  metadata.query_ok = std::ferror(maps) == 0;
  std::fclose(maps);
  return metadata;
}

bool MappingCovers(const MappingMetadata& mapping, const void* address,
                   std::size_t size) {
  if (!mapping.query_ok || !mapping.found || address == nullptr || size == 0)
    return false;
  const auto begin = reinterpret_cast<std::uintptr_t>(address);
  return begin <= UINTPTR_MAX - size && begin >= mapping.start &&
         begin + size <= mapping.end;
}

bool PrivateGuestCode(const MappingMetadata& mapping, const void* address,
                      std::size_t size) {
  return MappingCovers(mapping, address, size) && mapping.private_mapping &&
         (mapping.protection == PROT_READ ||
          mapping.protection == (PROT_READ | PROT_EXEC));
}

bool PrivateRw(const MappingMetadata& mapping, const void* address,
               std::size_t size) {
  return MappingCovers(mapping, address, size) && mapping.private_mapping &&
         mapping.protection == (PROT_READ | PROT_WRITE);
}

bool PrivateReadonly(const MappingMetadata& mapping, const void* address,
                     std::size_t size) {
  return MappingCovers(mapping, address, size) && mapping.private_mapping &&
         mapping.protection == PROT_READ;
}

std::size_t HookPatchSize(std::uint32_t index) {
  return index < kHookCount ? kPatchSize : 0;
}

bool TargetInExecutableLoad(const GameImage& image, std::uintptr_t target,
                            std::size_t size) {
  if (!image.found || image.base == 0 || image.programs == nullptr ||
      size == 0 || target > UINTPTR_MAX - size)
    return false;
  for (std::uint16_t index = 0; index < image.program_count; ++index) {
    const Elf64_Phdr& program = image.programs[index];
    if (program.p_type != PT_LOAD ||
        (program.p_flags & (PF_R | PF_W | PF_X)) != (PF_R | PF_X))
      continue;
    const std::uintptr_t begin = image.base + program.p_vaddr;
    const std::uintptr_t end = begin + program.p_memsz;
    if (target >= begin && target + size <= end) return true;
  }
  return false;
}

void AbsoluteJump(std::uint8_t patch[kPatchSize],
                  std::uintptr_t destination) {
  constexpr std::uint32_t kLoadX17 = 0x58000051;
  constexpr std::uint32_t kBranchX17 = 0xd61f0220;
  std::memcpy(patch, &kLoadX17, 4);
  std::memcpy(patch + 4, &kBranchX17, 4);
  std::memcpy(patch + 8, &destination, 8);
}

bool BuildHookPatch(std::uint32_t index, std::uint8_t patch[kPatchSize]) {
  if (index >= kHookCount) return false;
  std::memset(patch, 0, kPatchSize);
  AbsoluteJump(patch, g_control.wrapper[index]);
  return true;
}

bool BuildRandomHookPatch(std::uint32_t index,
                          std::uint8_t patch[kRandomPatchSize]) {
  if (index >= kRandomHookCount || g_random_hooks[index].target == 0 ||
      g_random_hooks[index].wrapper == 0)
    return false;
  AbsoluteJump(patch, g_random_hooks[index].wrapper);
  return true;
}

bool BuildLifecycleHookPatch(std::uint8_t patch[kPatchSize]) {
  if (g_control.lifecycle_target_entry == 0 ||
      g_control.lifecycle_wrapper == 0)
    return false;
  AbsoluteJump(patch, g_control.lifecycle_wrapper);
  return true;
}

bool RandomOriginalIdentityValid(std::uint32_t index,
                                 const std::uint8_t* bytes) {
  if (index >= kRandomHookCount || bytes == nullptr ||
      std::memcmp(bytes, kExpectedRandomPrologues[index],
                  kRandomIdentitySize) != 0)
    return false;
  std::uint32_t fourth_instruction = 0;
  std::memcpy(&fourth_instruction, bytes + kRandomIdentitySize,
              sizeof(fourth_instruction));
  return (fourth_instruction & 0xfc000000u) == 0x94000000u;
}

void ResetEvidence() {
  std::memset(&g_evidence, 0, sizeof(g_evidence));
  std::memcpy(g_evidence.magic, kEvidenceMagic, sizeof(kEvidenceMagic));
  g_evidence.version = kVersion;
  g_evidence.size = sizeof(Evidence);
  g_evidence.status = kPassive;
  g_evidence.flags = kNoExternalPerFrameStop | kG3CoordinatorReused;
}

void Initialize() {
  std::memset(&g_control, 0, sizeof(g_control));
  std::memcpy(g_control.magic, kControlMagic, sizeof(kControlMagic));
  g_control.version = kVersion;
  g_control.size = sizeof(Control);
  ResetEvidence();
}

void Fault(Error error) {
  __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
  if (__atomic_load_n(&g_evidence.first_error, __ATOMIC_RELAXED) == 0)
    __atomic_store_n(&g_evidence.first_error,
                     static_cast<std::uint32_t>(error), __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.status, kFault, __ATOMIC_RELEASE);
  __android_log_print(
      ANDROID_LOG_ERROR, kTag,
      "fault=%u mode=%u barrier=%u generation=%u tick=%llu receipts=%u "
      "phase=%u lifecycle=%u g3=%d g4=%d diagnostic=0x%llx",
      static_cast<unsigned>(error), g_control.mode,
      g_control.replay_speed_reserved, g_control.generation,
      static_cast<unsigned long long>(g_runtime.tick.coordinator.tick),
      g_runtime.receipt_count,
      static_cast<unsigned>(g_runtime.tick.coordinator.tick_phase),
      static_cast<unsigned>(g_runtime.tick.coordinator.lifecycle),
      g_runtime.last_g3_result, g_runtime.last_g4_result,
      static_cast<unsigned long long>(g_evidence.reserved[0]));
}

bool LockRuntime() {
  while (g_runtime_lock.test_and_set(std::memory_order_acquire)) {}
  __atomic_fetch_add(&g_control.active_helpers, 1u, __ATOMIC_ACQ_REL);
  return true;
}

void UnlockRuntime() {
  __atomic_fetch_sub(&g_control.active_helpers, 1u, __ATOMIC_RELEASE);
  g_runtime_lock.clear(std::memory_order_release);
}

void PublishCompletion(CompletionReason reason) {
  g_evidence.published_ticks = g_runtime.receipt_count;
  g_evidence.last_tick =
      (reason == CompletionReason::kRaceLifecycle ||
       reason == CompletionReason::kManualCheckpoint) &&
              g_runtime.receipt_count != 0
          ? g_runtime.receipt_count - 1u
          : g_runtime.tick.coordinator.tick;
  g_evidence.completion_reason = static_cast<std::uint32_t>(reason);
  // Preserve the exact lifecycle value that caused completion in the high
  // byte of the diagnostic word.  The resident lifecycle hook may immediately
  // observe the following Retry and update last_lifecycle_state, so that live
  // field is not a stable classifier for the attempt that just ended.
  g_evidence.reserved[0] =
      (g_evidence.reserved[0] & 0x00ffffffffffffffULL) |
      (static_cast<std::uint64_t>(g_evidence.last_lifecycle_state & 0xffu)
       << 56u);
  __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.status, kComplete, __ATOMIC_RELEASE);
}

bridge::ConfigV1 AdapterConfig();
bool ActivatePendingAtLifecycle(void* lifecycle_object);
bool RearmArchivedSession(bool replay, bool lifecycle_callback = false);
bool CompleteReplayToAtomicRecording();

TickSemanticSnapshot CaptureTickSemanticStart(std::uint64_t tick) {
  TickSemanticSnapshot snapshot{};
  snapshot.valid = true;
  snapshot.tick = tick;
  snapshot.recorded_interval_count = g_recorded_interval_count;
  std::memcpy(snapshot.qualified_events, g_evidence.qualified_events,
              sizeof(snapshot.qualified_events));
  snapshot.core_faults = g_evidence.core_faults;
  snapshot.fixed_delta_writes = g_evidence.fixed_delta_writes;
  snapshot.control_pair_reads = g_evidence.control_pair_reads;
  snapshot.control_pair_writes = g_evidence.control_pair_writes;
  snapshot.interval_calls = g_evidence.interval_calls;
  snapshot.interval_overrides = g_evidence.interval_overrides;
  snapshot.interval_records = g_evidence.interval_records;
  snapshot.natural_nitro_calls = g_evidence.natural_nitro_calls;
  snapshot.suppressed_nitro_calls = g_evidence.suppressed_nitro_calls;
  snapshot.injected_nitro_calls = g_evidence.injected_nitro_calls;
  snapshot.recorded_frames = g_evidence.recorded_frames;
  std::memcpy(snapshot.setter_qualified_events,
              g_evidence.setter_qualified_events,
              sizeof(snapshot.setter_qualified_events));
  std::memcpy(snapshot.setter_overrides, g_evidence.setter_overrides,
              sizeof(snapshot.setter_overrides));
  snapshot.physics_equal_frames = g_evidence.physics_equal_frames;
  snapshot.physics_corrected_frames = g_evidence.physics_corrected_frames;
  snapshot.physics_correction_writes = g_evidence.physics_correction_writes;
  snapshot.physics_skipped_frames = g_evidence.physics_skipped_frames;
  snapshot.barrel_rbx_overrides = g_evidence.barrel_rbx_overrides;
  snapshot.barrel_angular_overrides = g_evidence.barrel_angular_overrides;
  snapshot.barrel_rbx_records = g_evidence.barrel_rbx_records;
  snapshot.barrel_angular_records = g_evidence.barrel_angular_records;
  return snapshot;
}

bool RestoreDiscardedTickSemantics(std::uint64_t tick) {
  if (!g_tick_semantic_start.valid || g_tick_semantic_start.tick != tick)
    return false;
  const TickSemanticSnapshot& snapshot = g_tick_semantic_start;
  g_recorded_interval_count = snapshot.recorded_interval_count;
  std::memcpy(g_evidence.qualified_events, snapshot.qualified_events,
              sizeof(snapshot.qualified_events));
  g_evidence.core_faults = snapshot.core_faults;
  g_evidence.fixed_delta_writes = snapshot.fixed_delta_writes;
  g_evidence.control_pair_reads = snapshot.control_pair_reads;
  g_evidence.control_pair_writes = snapshot.control_pair_writes;
  g_evidence.interval_calls = snapshot.interval_calls;
  g_evidence.interval_overrides = snapshot.interval_overrides;
  g_evidence.interval_records = snapshot.interval_records;
  g_evidence.natural_nitro_calls = snapshot.natural_nitro_calls;
  g_evidence.suppressed_nitro_calls = snapshot.suppressed_nitro_calls;
  g_evidence.injected_nitro_calls = snapshot.injected_nitro_calls;
  g_evidence.recorded_frames = snapshot.recorded_frames;
  std::memcpy(g_evidence.setter_qualified_events,
              snapshot.setter_qualified_events,
              sizeof(snapshot.setter_qualified_events));
  std::memcpy(g_evidence.setter_overrides, snapshot.setter_overrides,
              sizeof(snapshot.setter_overrides));
  g_evidence.physics_equal_frames = snapshot.physics_equal_frames;
  g_evidence.physics_corrected_frames = snapshot.physics_corrected_frames;
  g_evidence.physics_correction_writes = snapshot.physics_correction_writes;
  g_evidence.physics_skipped_frames = snapshot.physics_skipped_frames;
  g_evidence.barrel_rbx_overrides = snapshot.barrel_rbx_overrides;
  g_evidence.barrel_angular_overrides = snapshot.barrel_angular_overrides;
  g_evidence.barrel_rbx_records = snapshot.barrel_rbx_records;
  g_evidence.barrel_angular_records = snapshot.barrel_angular_records;
  g_record_physics_snapshot = {};
  g_barrel_capture = {};
  g_barrel_capture_tick = UINT64_MAX;
  g_tick_semantic_start = {};
  return true;
}

bool MaybeDiscardOpenLifecycleTick(std::uint32_t lifecycle) {
  if (g_control.completion_policy !=
          static_cast<std::uint32_t>(CompletionPolicy::kRaceLifecycle) ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kRecord) ||
      lifecycle == 3u || !g_runtime.input_action.tick_open)
    return false;
  const std::uint64_t tick = g_runtime.input_action.tick;
  const bridge::Result discarded =
      bridge::DiscardOpenTickForRaceEnd(AdapterConfig(), &g_runtime);
  if (discarded != bridge::Result::kObserved ||
      !RestoreDiscardedTickSemantics(tick)) {
    ++g_evidence.core_faults;
    Fault(kErrorAdapter);
  }
  return true;
}

// Race end is not guaranteed to occur inside the final FrameEvent callback.
// Retry can change the already-bound lifecycle value after that callback and
// before the next qualified Physics-submit.  Observe both existing boundaries
// so an actual-length recording cannot wait forever for a callback that has
// already passed.  No extra game hook is introduced.
bool MaybeCompleteLifecycleRecording(std::uint32_t lifecycle) {
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
      g_control.completion_policy !=
          static_cast<std::uint32_t>(CompletionPolicy::kRaceLifecycle) ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kRecord) ||
      lifecycle == 3u ||
      g_runtime.tick.coordinator.lifecycle !=
          coordinator::Lifecycle::kInRace ||
      g_runtime.tick.coordinator.tick_phase !=
          coordinator::TickPhase::kClosed)
    return false;
  const bridge::Result ended = bridge::ObserveRaceEndAtClosedBoundary(
      AdapterConfig(), &g_runtime, lifecycle);
  g_evidence.last_g3_result = g_runtime.last_g3_result;
  g_evidence.last_g4_result = g_runtime.last_g4_result;
  if (ended == bridge::Result::kRaceEnded &&
      g_runtime.receipt_count != 0) {
    PublishCompletion(CompletionReason::kRaceLifecycle);
  } else {
    ++g_evidence.core_faults;
    Fault(kErrorAdapter);
  }
  return true;
}

std::uint32_t Tid() {
  return static_cast<std::uint32_t>(syscall(__NR_gettid));
}

void Note(std::uint32_t hook, bridge::Result result, std::uint32_t tid) {
  if (result == bridge::Result::kIgnored) {
    ++g_evidence.ignored_events;
  } else if (result == bridge::Result::kObserved ||
             result == bridge::Result::kRepeatedInterval ||
             result == bridge::Result::kComplete) {
    if (g_evidence.first_tid[hook] == 0) g_evidence.first_tid[hook] = tid;
    g_evidence.last_tid[hook] = tid;
    ++g_evidence.qualified_events[hook];
  } else {
    ++g_evidence.core_faults;
    g_evidence.reserved[0] =
        (static_cast<std::uint64_t>(hook) << 32) |
        static_cast<std::uint32_t>(g_runtime.tick.coordinator.tick_phase);
    g_evidence.last_tick = g_runtime.tick.coordinator.tick;
    Fault(kErrorAdapter);
  }
  g_evidence.last_g3_result = g_runtime.last_g3_result;
  g_evidence.last_g4_result = g_runtime.last_g4_result;
  if (result == bridge::Result::kComplete) {
    // frame_limit is only a storage capacity in lifecycle recording mode.
    // Reaching it before a proven race end is truncation, not completion.
    if (g_control.completion_policy ==
        static_cast<std::uint32_t>(CompletionPolicy::kRaceLifecycle))
      Fault(kErrorRecordingCapacity);
    else if (g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay) &&
             __atomic_load_n(&g_control.replay_speed_reserved,
                             __ATOMIC_ACQUIRE) ==
                 kReplayCompletionAtomicRecord) {
      if (!CompleteReplayToAtomicRecording()) Fault(kErrorAdapter);
    } else {
      PublishCompletion(CompletionReason::kFixedFrameLimit);
    }
  }
}

bridge::ConfigV1 AdapterConfig() {
  const bool replay =
      g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay);
  return {
      .tick = {.session_id = g_control.session_id,
               .generation = g_control.generation,
               .frame_limit = g_control.frame_limit,
               .game_base = g_control.game_base,
               .expected_begin_owner = g_control.expected_begin_owner,
               .expected_begin_owner_vptr =
                   g_control.expected_begin_owner_vptr,
               .expected_interval_owner = g_control.expected_interval_owner,
               .expected_interval_owner_vptr =
                   g_control.expected_interval_owner_vptr,
               .expected_player = g_control.expected_player,
               .expected_player_vptr = g_control.expected_player_vptr,
               .physics_context_vtable_rva =
                   g_build_profile.physics_context_vtable_rva,
               .physics_implementation_vtable_rva =
                   g_build_profile.physics_implementation_vtable_rva,
               .frame_event_scheduler_return_rva =
                   g_build_profile.frame_event_scheduler_return_rva,
               .replay_queue = {
                   .mode = replay ? coordinator::ReplayMode::kActiveBlock
                                  : coordinator::ReplayMode::kInactive,
                   .communication_version_matches = true,
                   .block_mode_still_active = replay,
                   .packets = replay ? g_replay_frames : nullptr,
                   .packet_count = replay ? g_control.replay_frame_count : 0,
               }},
      .fixed_delta_us = g_control.fixed_delta_us,
      .interval_replay = {
          .samples = replay ? g_replay_intervals : nullptr,
          .sample_count = replay ? g_control.replay_interval_count : 0,
      },
      .allow_zero_integration_updates = A9TAS_EXPERIMENTAL_HIGH_REFRESH &&
          (g_control.fixed_delta_us == 8333 || g_control.fixed_delta_us == 6944),
  };
}

bool NativePhysicsIdentityAlive() {
  if (g_control.expected_player > UINTPTR_MAX - kBackendInterfaceOffset ||
      g_control.expected_barrel_owner >
          UINTPTR_MAX - kBarrelBackendOffset ||
      g_control.expected_physics_velocity_interface >
          UINTPTR_MAX - kNativeBodySlotOffset ||
      g_control.expected_native_body >
          UINTPTR_MAX - recording::kAngularVelocityOffset -
                            recording::kAngularVelocitySize)
    return false;
  const auto* interface_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_player + kBackendInterfaceOffset);
  const auto* native_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_physics_velocity_interface + kNativeBodySlotOffset);
  const auto* barrel_native_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_barrel_owner + kBarrelBackendOffset);
  const auto* native_vptr = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_native_body);
  return __atomic_load_n(interface_slot, __ATOMIC_RELAXED) ==
             g_control.expected_backend_interface &&
         __atomic_load_n(native_slot, __ATOMIC_RELAXED) ==
             g_control.expected_native_body &&
         __atomic_load_n(barrel_native_slot, __ATOMIC_RELAXED) ==
             g_control.expected_physics_velocity_interface &&
         __atomic_load_n(native_vptr, __ATOMIC_RELAXED) ==
             g_control.game_base +
                 g_build_profile.native_physics_body_vtable_rva;
}

bool NativePhysicsMappingsValid() {
  if (g_control.expected_player > UINTPTR_MAX - kBackendInterfaceOffset ||
      g_control.expected_barrel_owner >
          UINTPTR_MAX - kBarrelBackendOffset ||
      g_control.expected_physics_velocity_interface >
          UINTPTR_MAX - kNativeBodySlotOffset ||
      g_control.expected_native_body >
          UINTPTR_MAX - recording::kAngularVelocityOffset -
                            recording::kAngularVelocitySize)
    return false;
  const auto* interface_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_player + kBackendInterfaceOffset);
  const auto* native_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_physics_velocity_interface + kNativeBodySlotOffset);
  const auto* barrel_native_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_barrel_owner + kBarrelBackendOffset);
  const auto* native = reinterpret_cast<const std::uint8_t*>(
      g_control.expected_native_body);
  return PrivateRw(QueryMapping(interface_slot), interface_slot,
                   sizeof(*interface_slot)) &&
         PrivateRw(QueryMapping(native_slot), native_slot,
                   sizeof(*native_slot)) &&
         PrivateRw(QueryMapping(barrel_native_slot), barrel_native_slot,
                   sizeof(*barrel_native_slot)) &&
         PrivateRw(QueryMapping(native), native,
                   recording::kAngularVelocityOffset +
                       recording::kAngularVelocitySize) &&
         NativePhysicsIdentityAlive();
}

bool ReplayBuffersValid() {
  const bool sparse = AdapterConfig().allow_zero_integration_updates;
  if (g_control.replay_frame_count != g_control.frame_limit ||
      g_control.replay_frame_count == 0 ||
      (g_control.replay_interval_count == 0 && !sparse) ||
      g_control.replay_interval_count > kMaximumIntervalSamples) {
    g_evidence.reserved[0] = 0x2000;
    return false;
  }
  for (std::uint32_t index = 0; index < g_control.replay_frame_count;
       ++index) {
    const auto& frame = g_replay_frames[index];
    if (!g4::PhysicsRecordingFrameValid(frame) || frame.tick != index ||
        frame.monotonic_ns !=
            static_cast<std::uint64_t>(index) * g_control.fixed_delta_us *
                1000ULL) {
      g_evidence.reserved[0] = 0x21000000ULL | index;
      return false;
    }
  }
  std::uint64_t last_tick = UINT64_MAX;
  std::uint32_t next_ordinal = 0;
  for (std::uint32_t index = 0; index < g_control.replay_interval_count;
       ++index) {
    const auto& sample = g_replay_intervals[index];
    if (sample.tick >= g_control.replay_frame_count ||
        !g4::IntervalBitsValid(sample.output_bits)) {
      g_evidence.reserved[0] = 0x22000000ULL | index;
      return false;
    }
    if (sample.tick != last_tick) {
      if ((!sparse && last_tick == UINT64_MAX && sample.tick != 0) ||
          (last_tick != UINT64_MAX && sample.tick <= last_tick) ||
          (!sparse && last_tick != UINT64_MAX && sample.tick != last_tick + 1) ||
          sample.ordinal != 0) {
        g_evidence.reserved[0] = 0x23000000ULL | index;
        return false;
      }
      last_tick = sample.tick;
      next_ordinal = 0;
    }
    if (sample.ordinal != next_ordinal++) {
      g_evidence.reserved[0] = 0x24000000ULL | index;
      return false;
    }
  }
  if (!sparse && last_tick + 1 != g_control.replay_frame_count) {
    g_evidence.reserved[0] = 0x2500;
    return false;
  }
  return true;
}

}  // namespace

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4IntervalOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0x30\n"
      "stp x21, x20, [sp, #0x10]\n"
      "stp x19, x30, [sp, #0x20]\n"
      "mov w21, #0x8889\n"
      "adrp x17, :got:g_g4_interval_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_interval_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4FinalOriginalV1() {
  __asm__ volatile(
      "str d12, [sp, #-0x90]!\n"
      "stp d11, d10, [sp, #0x10]\n"
      "stp d9, d8, [sp, #0x20]\n"
      "stp x28, x27, [sp, #0x30]\n"
      "adrp x17, :got:g_g4_final_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_final_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4FrameOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0x30\n"
      "str x20, [sp, #0x10]\n"
      "stp x19, x30, [sp, #0x20]\n"
      "ldr x8, [x1]\n"
      "adrp x17, :got:g_g4_frame_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_frame_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4NitroOriginalV1(void*) {
  __asm__ volatile(
      "stp x21, x20, [sp, #-0x20]!\n"
      "stp x19, x30, [sp, #0x10]\n"
      "ldrb w8, [x0, #0x1b9]\n"
      "cbz w8, 1f\n"
      "adrp x17, :got:g_g4_nitro_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_nitro_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n"
      "1:\n"
      "ldp x19, x30, [sp, #0x10]\n"
      "ldp x21, x20, [sp], #0x20\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4SubmitOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0x30\n"
      "str x20, [sp, #0x10]\n"
      "stp x19, x30, [sp, #0x20]\n"
      "ldrb w8, [x0, #0x1c8]\n"
      "adrp x17, :got:g_g4_submit_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_submit_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4BarrelRollOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0xb0\n"
      "str d14, [sp, #0x50]\n"
      "stp d13, d12, [sp, #0x58]\n"
      "stp d11, d10, [sp, #0x68]\n"
      "adrp x17, :got:g_g4_barrel_roll_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_barrel_roll_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4BarrelYawOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0x140\n"
      "stp d15, d14, [sp, #0xc0]\n"
      "stp d13, d12, [sp, #0xd0]\n"
      "stp d11, d10, [sp, #0xe0]\n"
      "adrp x17, :got:g_g4_barrel_yaw_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_barrel_yaw_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4DispatcherOriginalV1(void*, std::int64_t*) {
  __asm__ volatile(
      "sub sp, sp, #0x50\n"
      "stp x23, x22, [sp, #0x20]\n"
      "stp x21, x20, [sp, #0x30]\n"
      "stp x19, x30, [sp, #0x40]\n"
      "adrp x17, :got:g_g4_dispatch_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_dispatch_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4LifecycleOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0xe0\n"
      "stp x23, x22, [sp, #0xb0]\n"
      "stp x21, x20, [sp, #0xc0]\n"
      "stp x19, x30, [sp, #0xd0]\n"
      "adrp x17, :got:g_g4_lifecycle_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g4_lifecycle_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((noinline, visibility("hidden"))) std::uint32_t
G4LifecycleBeforeV1(void* object) {
  __atomic_fetch_add(&g_evidence.lifecycle_entries, 1ULL, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_control.active_helpers, 1u, __ATOMIC_ACQ_REL);
  if (object == nullptr || reinterpret_cast<std::uintptr_t>(object) >
                               UINTPTR_MAX - 0x2d8u) {
    ++g_evidence.ignored_events;
    return 0;
  }
  auto* phase = reinterpret_cast<std::uint32_t*>(
      reinterpret_cast<std::uintptr_t>(object) + 0x2d8u);
  if (!PrivateRw(QueryMapping(phase), phase, sizeof(*phase)) ||
      __atomic_load_n(phase, __ATOMIC_ACQUIRE) != 2u) {
    ++g_evidence.ignored_events;
    return 0;
  }
  __atomic_store_n(&g_control.lifecycle_state_address,
                   reinterpret_cast<std::uintptr_t>(phase),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.last_lifecycle_state, 2u, __ATOMIC_RELEASE);
  if (__atomic_load_n(&g_control.pending_state, __ATOMIC_ACQUIRE) == 1u)
    (void)ActivatePendingAtLifecycle(object);
  return 1;
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4LifecycleAfterV1(void* object, std::uint32_t qualified) {
  if (qualified != 0 && object != nullptr &&
      reinterpret_cast<std::uintptr_t>(object) <= UINTPTR_MAX - 0x2d8u) {
    const auto* phase = reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(object) + 0x2d8u);
    if (PrivateRw(QueryMapping(phase), phase, sizeof(*phase)) &&
        __atomic_load_n(phase, __ATOMIC_ACQUIRE) == 3u) {
      __atomic_store_n(&g_evidence.last_lifecycle_state, 3u,
                       __ATOMIC_RELEASE);
      __atomic_fetch_add(&g_evidence.lifecycle_qualified, 1ULL,
                         __ATOMIC_RELAXED);
    } else {
      ++g_evidence.ignored_events;
    }
  }
  __atomic_fetch_sub(&g_control.active_helpers, 1u, __ATOMIC_RELEASE);
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4DispatcherEntryV1(void* owner, std::int64_t* elapsed) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kLogicDispatcherHook], 1ULL,
                     __ATOMIC_RELAXED);
  // Keep restore/rearm from racing the outer dispatcher wrapper while its
  // nested tick hooks temporarily acquire and release the runtime lock.
  __atomic_fetch_add(&g_control.active_helpers, 1u, __ATOMIC_ACQ_REL);
  const std::uint32_t tid = Tid();
  const bool dispatcher_tracked = EnterDispatcher(tid);

  std::uint32_t factor = 1;
  const bool enabled =
      __atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) != 0;
  if (!enabled && owner != nullptr) {
    const std::uintptr_t vptr = __atomic_load_n(
        reinterpret_cast<const std::uintptr_t*>(owner), __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_object[kLogicDispatcherHook],
                     reinterpret_cast<std::uintptr_t>(owner),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_evidence.last_vptr[kLogicDispatcherHook], vptr,
                     __ATOMIC_RELEASE);
  }
  const bool hold_at_replay_completion =
      enabled &&
      g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay) &&
      __atomic_load_n(&g_control.replay_speed_reserved, __ATOMIC_ACQUIRE) ==
          kReplayCompletionBarrierEnabled;
  const bool request_record_checkpoint =
      enabled &&
      g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord) &&
      __atomic_load_n(&g_control.replay_speed_reserved, __ATOMIC_ACQUIRE) ==
          kRecordCheckpointBarrierRequested;
  const std::uint32_t checkpoint_receipts_before =
      request_record_checkpoint ? g_runtime.receipt_count : 0u;
  std::uint32_t accelerated_generation = 0;
  if (enabled &&
      g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay) &&
      g_control.lifecycle_state_address != 0) {
    const auto* lifecycle = reinterpret_cast<const std::uint32_t*>(
        g_control.lifecycle_state_address);
    if (__atomic_load_n(lifecycle, __ATOMIC_RELAXED) == 3u) {
      const std::uint32_t configured = __atomic_load_n(
          &g_control.replay_speed_factor, __ATOMIC_RELAXED);
      if (configured >= kMinimumReplaySpeedFactor &&
          configured <= kMaximumReplaySpeedFactor) {
        factor = configured;
        accelerated_generation = __atomic_load_n(
            &g_control.generation, __ATOMIC_ACQUIRE);
      } else {
        Fault(kErrorControl);
      }
    }
  }

  // Match the upstream real-time accumulator: high-frequency outer callbacks
  // must not each consume a full 16.667ms of simulated time. Low-frequency
  // callbacks may execute several complete ticks; replay scales elapsed budget.
  // Normal-speed quantization is centered by Budget (half-tick phase credit),
  // preserving one natural update under small callback timing jitter.
  const bool paced = enabled && g_control.completed == 0 &&
      (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord) ||
       g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay)) &&
      g_control.lifecycle_state_address != 0 &&
      __atomic_load_n(reinterpret_cast<const std::uint32_t*>(
          g_control.lifecycle_state_address), __ATOMIC_RELAXED) == 3u;
  const std::uint32_t batch_mode = g_control.mode;
  accelerated_generation = g_control.generation;
  std::uint32_t iterations = factor;
  const bool budget_owner = dispatcher_tracked && CurrentDispatcherDepth(tid) == 1u;
  if (budget_owner) {
    LockRuntime();
    if (paced) {
      timespec now{};
      if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        const std::uint64_t now_ns = static_cast<std::uint64_t>(now.tv_sec) *
            1000000000ULL + static_cast<std::uint64_t>(now.tv_nsec);
        iterations = g_realtime_budget.Plan(now_ns, accelerated_generation,
                                            g_control.fixed_delta_us * 1000ULL, factor);
      } else {
        g_realtime_budget.Reset();
        iterations = 1;
      }
    } else {
      g_realtime_budget.Reset();
    }
    UnlockRuntime();
  } else {
    iterations = 1; // nested/concurrent calls do not receive synthetic batches
  }
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const std::uint32_t ticks_before = g_runtime.receipt_count;
    G4DispatcherOriginalV1(owner, elapsed);
    if (enabled) {
      if (g_evidence.first_tid[kLogicDispatcherHook] == 0)
        g_evidence.first_tid[kLogicDispatcherHook] = tid;
      g_evidence.last_tid[kLogicDispatcherHook] = tid;
      g_evidence.last_object[kLogicDispatcherHook] =
          reinterpret_cast<std::uintptr_t>(owner);
      ++g_evidence.qualified_events[kLogicDispatcherHook];
    }

    // A repeated complete-logic call can consume the final replay packet,
    // activate a queued record/replay generation, or leave the race lifecycle.
    // In particular, the first natural dispatcher call of a new race may
    // switch Replay -> Record inside the nested lifecycle hook.  Re-check the
    // authoritative mode and generation before issuing another synthetic Tick;
    // otherwise the old replay's cached 2x/4x/8x factor leaks into the first
    // recording attempt.
    const bool tick_progressed = g_runtime.receipt_count != ticks_before;
    if (paced && !tick_progressed && budget_owner) {
      // Paused UI calls do not consume physics ticks. Keep servicing them once
      // per callback instead of replaying a catch-up batch while paused.
      LockRuntime();
      g_realtime_budget.Reset();
      UnlockRuntime();
    }
    if (iteration + 1u == iterations || !tick_progressed ||
        __atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&g_control.completed, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&g_control.mode, __ATOMIC_ACQUIRE) !=
            batch_mode ||
        __atomic_load_n(&g_control.generation, __ATOMIC_ACQUIRE) !=
            accelerated_generation ||
        (batch_mode == static_cast<std::uint32_t>(RunMode::kReplay) &&
         __atomic_load_n(&g_control.replay_speed_factor, __ATOMIC_ACQUIRE) != factor) ||
        __atomic_load_n(&g_evidence.status, __ATOMIC_ACQUIRE) == kFault ||
        (g_control.lifecycle_state_address != 0 &&
         __atomic_load_n(reinterpret_cast<const std::uint32_t*>(
                             g_control.lifecycle_state_address),
                         __ATOMIC_RELAXED) != 3u))
      break;
  }
  LeaveDispatcher(tid, dispatcher_tracked);
  __atomic_fetch_sub(&g_control.active_helpers, 1u, __ATOMIC_RELEASE);

  // Match AluTasV2 ActiveBlockThread semantics: keep the game Tick itself at
  // the exact terminal replay packet until the host has either armed suffix
  // recording or restored the hooks. active_helpers is released first so the
  // sealed receipt can be validated and the existing replay->record rearm can
  // run. Rearm publishes completed=0 only after recording is authoritative;
  // consequently a delayed ESC can no longer create an unrecorded Tick gap.
  if (hold_at_replay_completion &&
      __atomic_load_n(&g_control.completed, __ATOMIC_ACQUIRE) != 0 &&
      __atomic_load_n(&g_evidence.status, __ATOMIC_ACQUIRE) == kComplete) {
    const timespec one_millisecond{0, 1000 * 1000};
    while (g_install_state.load(std::memory_order_acquire) == 1 &&
           __atomic_load_n(&g_control.completed, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&g_control.replay_speed_reserved,
                           __ATOMIC_ACQUIRE) ==
               kReplayCompletionBarrierEnabled &&
           __atomic_load_n(&g_evidence.status, __ATOMIC_ACQUIRE) == kComplete) {
      nanosleep(&one_millisecond, nullptr);
    }
  }

  // A manual checkpoint must not be created by deleting an already-started
  // Tick after the game has partially advanced it.  A host request is promoted
  // to Held only after the outer dispatcher has returned with a newly published
  // closed Tick.  active_helpers is already zero here, so the stopped host can
  // seal that exact boundary and then release this barrier.
  if (request_record_checkpoint &&
      __atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) != 0 &&
      __atomic_load_n(&g_control.completed, __ATOMIC_ACQUIRE) == 0 &&
      __atomic_load_n(&g_evidence.status, __ATOMIC_ACQUIRE) == kArmed &&
      g_runtime.receipt_count > checkpoint_receipts_before &&
      !g_runtime.input_action.tick_open &&
      g_runtime.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed) {
    std::uint32_t requested = kRecordCheckpointBarrierRequested;
    if (__atomic_compare_exchange_n(
            &g_control.replay_speed_reserved, &requested,
            kRecordCheckpointBarrierHeld, false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
      const timespec one_millisecond{0, 1000 * 1000};
      while (g_install_state.load(std::memory_order_acquire) == 1 &&
             __atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) != 0 &&
             __atomic_load_n(&g_control.completed, __ATOMIC_ACQUIRE) == 0 &&
             __atomic_load_n(&g_control.replay_speed_reserved,
                             __ATOMIC_ACQUIRE) ==
                 kRecordCheckpointBarrierHeld &&
             __atomic_load_n(&g_evidence.status, __ATOMIC_ACQUIRE) == kArmed) {
        nanosleep(&one_millisecond, nullptr);
      }
    }
  }
}

void LockBarrelPrng() {
  while (g_barrel_prng_lock.test_and_set(std::memory_order_acquire)) {}
}

void UnlockBarrelPrng() {
  g_barrel_prng_lock.clear(std::memory_order_release);
}

void ResetBarrelPrng() {
  LockBarrelPrng();
  barrel_prng::Reset(&g_barrel_prng);
  UnlockBarrelPrng();
  ++g_evidence.barrel_random_resets;
}

extern "C" __attribute__((noinline, visibility("hidden"))) bool
G4BarrelRandomBoolEntryV1(void*) {
  __atomic_fetch_add(&g_control.active_helpers, 1u, __ATOMIC_ACQ_REL);
  LockBarrelPrng();
  const bool value = barrel_prng::NextBool(&g_barrel_prng);
  UnlockBarrelPrng();
  __atomic_fetch_add(&g_evidence.barrel_random_bool_calls, 1ULL,
                     __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g_control.active_helpers, 1u, __ATOMIC_RELEASE);
  return value;
}

extern "C" __attribute__((noinline, visibility("hidden"))) float
G4BarrelRandomLerpEntryV1(void*, const float* bounds) {
  __atomic_fetch_add(&g_control.active_helpers, 1u, __ATOMIC_ACQ_REL);
  if (bounds == nullptr) {
    Fault(kErrorBarrelRandom);
    __atomic_fetch_sub(&g_control.active_helpers, 1u, __ATOMIC_RELEASE);
    return 0.0f;
  }
  const float low = bounds[0];
  const float high = bounds[1];
  LockBarrelPrng();
  const float value = barrel_prng::NextFloat01(&g_barrel_prng) *
                      (high - low) + low;
  UnlockBarrelPrng();
  __atomic_fetch_add(&g_evidence.barrel_random_lerp_calls, 1ULL,
                     __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g_control.active_helpers, 1u, __ATOMIC_RELEASE);
  return value;
}

extern "C" __attribute__((noinline, visibility("hidden"))) std::uint32_t
G4SubmitBeforeV1(void* context, std::int64_t* delta_token) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kSubmitHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (context == nullptr) return 0;
  const std::uintptr_t observed_vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(context), __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) {
    __atomic_store_n(&g_evidence.last_object[kSubmitHook],
                     reinterpret_cast<std::uintptr_t>(context),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_evidence.last_vptr[kSubmitHook], observed_vptr,
                     __ATOMIC_RELEASE);
    return 0;
  }
  if (delta_token == nullptr ||
      (reinterpret_cast<std::uintptr_t>(delta_token) & 7u) != 0) {
    Fault(kErrorFixedDelta);
    return 0;
  }
  LockRuntime();
  const std::uintptr_t vptr = observed_vptr;
  const auto* phase = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  const std::uint32_t lifecycle = __atomic_load_n(phase, __ATOMIC_RELAXED);
  const std::uint32_t tid = Tid();
  g_evidence.last_object[kSubmitHook] =
      reinterpret_cast<std::uintptr_t>(context);
  g_evidence.last_vptr[kSubmitHook] = vptr;
  g_evidence.last_lifecycle_state = lifecycle;
  (void)MaybeDiscardOpenLifecycleTick(lifecycle);
  if (MaybeCompleteLifecycleRecording(lifecycle)) {
    UnlockRuntime();
    return 0;
  }
  const std::uint64_t natural_pair =
      g4::CurrentControlPair(g_runtime.input_action);
  const TickSemanticSnapshot semantic_start =
      CaptureTickSemanticStart(g_runtime.tick.coordinator.tick);
  bridge::SubmitBeforeReceiptV1 receipt{};
  const bridge::Result result = bridge::ObservePhysicsSubmitBeforeOriginal(
      AdapterConfig(), &g_runtime, reinterpret_cast<std::uintptr_t>(context),
      vptr, lifecycle, tid, natural_pair, &receipt);
  Note(kSubmitHook, result, tid);
  if (result == bridge::Result::kObserved && receipt.first_call_in_tick &&
      g_evidence.status != kFault) {
    g_tick_semantic_start = semantic_start;
    if (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord)) {
      if (g_record_physics_snapshot.captured) {
        Fault(kErrorPhysicsCapture);
      } else {
        g_record_physics_snapshot = {};
      }
    }
    const RunMode mode = static_cast<RunMode>(g_control.mode);
    if (mode != RunMode::kNeutral && receipt.decision.write_fixed_delta) {
      __atomic_store_n(delta_token, receipt.decision.fixed_delta_us,
                       __ATOMIC_RELAXED);
      if (__atomic_load_n(delta_token, __ATOMIC_RELAXED) !=
          receipt.decision.fixed_delta_us) {
        Fault(kErrorFixedDelta);
      } else {
        ++g_evidence.fixed_delta_writes;
      }
    }
    ++g_evidence.control_pair_reads;
    if (receipt.decision.write_control_pair) Fault(kErrorControlPair);
  }
  UnlockRuntime();
  return result == bridge::Result::kObserved ? 1u : 0u;
}

extern "C" __attribute__((noinline, visibility("hidden"))) std::uint32_t
G4IntervalBeforeV1(void* owner) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kTickHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (owner == nullptr) return 0;
  const std::uintptr_t vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(owner), __ATOMIC_RELAXED);
  // A retained brush-lap session keeps the proven entry hooks installed while
  // the completed control is disabled.  Observe only the current interval
  // owner in that state so Retry can bind the next race without reinstalling
  // code patches or trusting the previous race's object address.
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) {
    __atomic_store_n(&g_evidence.last_object[kTickHook],
                     reinterpret_cast<std::uintptr_t>(owner),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_evidence.last_vptr[kTickHook], vptr,
                     __ATOMIC_RELEASE);
    return 0;
  }
  LockRuntime();
  const auto* phase = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  const std::uint32_t lifecycle = __atomic_load_n(phase, __ATOMIC_RELAXED);
  const std::uint32_t tid = Tid();
  g_evidence.last_object[0] = reinterpret_cast<std::uintptr_t>(owner);
  g_evidence.last_vptr[0] = vptr;
  g_evidence.last_lifecycle_state = lifecycle;
  // The three real input setters own this persistent cache.  This replaces the
  // old C98 memory-pair shortcut and matches AluTasV2's event-driven semantics.
  const std::uint64_t natural_pair =
      g4::CurrentControlPair(g_runtime.input_action);
  bridge::IntervalBeforeReceiptV1 receipt{};
  const bridge::Result result = bridge::ObserveIntervalBeforeOriginal(
      AdapterConfig(), &g_runtime, reinterpret_cast<std::uintptr_t>(owner),
      vptr, lifecycle, tid, natural_pair, &receipt);
  Note(kTickHook, result, tid);
  if (result == bridge::Result::kObserved && receipt.first_call_in_tick &&
      g_evidence.status != kFault) {
    g_barrel_capture = {};
    g_barrel_capture_tick = g_runtime.input_action.tick;
    if (g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay)) {
      std::uint32_t completed = 0;
      const auto service = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(
          g_control.expected_interval_owner + kNitroServiceOffset),
          __ATOMIC_RELAXED);
      if (service > UINTPTR_MAX - 8 ||
          service + 8 != g_control.expected_nitro_state) {
        Fault(kErrorNitroIdentity);
      } else {
        const std::uint32_t planned =
            g_runtime.input_action.replay_packet_present &&
                    (g_runtime.input_action.packet.skip_override_flags &
                     recording::kSkipNitroActivation) == 0
                ? g_runtime.input_action.packet.nitro_activation_count
                : 0;
        for (; completed < planned; ++completed)
          G4NitroOriginalV1(
              reinterpret_cast<void*>(g_control.expected_nitro_state));
      }
      const bridge::Result accounted =
          bridge::AccountInjectedNitroCalls(&g_runtime, completed);
      if (accounted != bridge::Result::kObserved) {
        Note(kTickHook, accounted, tid);
      } else {
        g_evidence.injected_nitro_calls += completed;
      }
    }
  }
  UnlockRuntime();
  return bridge::IntervalAfterQualified(result) ? 1u : 0u;
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4IntervalAfterV1(std::uint32_t* output,
                  std::uint32_t entry_qualified) {
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0)
    return;
  // The exact function is shared by more than one implementation object.
  // Pair this after-original decision only with the immediately preceding
  // qualified before-original result from this wrapper invocation.
  if (entry_qualified == 0) return;
  if (output == nullptr) {
    Fault(kErrorIntervalStream);
    return;
  }
  LockRuntime();
  const std::uint32_t original_bits =
      __atomic_load_n(output, __ATOMIC_RELAXED);
  g4::IntervalDecisionV1 decision{};
  const bridge::Result result = bridge::ObserveIntervalAfterOriginal(
      AdapterConfig(), &g_runtime, original_bits, &decision);
  Note(kTickHook, result, Tid());
  std::uint32_t final_bits = original_bits;
  if (result == bridge::Result::kObserved && g_evidence.status != kFault) {
    ++g_evidence.interval_calls;
    if (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord)) {
      if (g_recorded_interval_count >= kMaximumIntervalSamples) {
        Fault(kErrorRecordingCapacity);
      } else {
        g_recorded_intervals[g_recorded_interval_count++] = {
            .tick = decision.tick,
            .ordinal = decision.ordinal,
            .output_bits = decision.final_output_bits,
        };
        ++g_evidence.interval_records;
      }
    } else if (g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay) &&
               g_recorded_interval_count < kMaximumIntervalSamples) {
      // AluTasV2 records continuously while replay overrides are active. Keep
      // the real, after-original interval observations in a private staging
      // stream so a later replay-to-record handoff can retain the exact prefix
      // without manufacturing interval samples or restarting its clock. This
      // staging data is not exposed as replay evidence and never changes the
      // game getter result.
      g_recorded_intervals[g_recorded_interval_count++] = {
          .tick = decision.tick,
          .ordinal = decision.ordinal,
          .output_bits = original_bits,
      };
    }
    if (g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay) &&
        decision.override_after_original) {
      final_bits = decision.final_output_bits;
      __atomic_store_n(output, final_bits, __ATOMIC_RELAXED);
      ++g_evidence.interval_overrides;
    }
  }
  UnlockRuntime();
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4FinalAfterV1(void* player) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kFinalWriterHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (player == nullptr) return;
  const std::uintptr_t observed_vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(player), __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) {
    __atomic_store_n(&g_evidence.last_object[kFinalWriterHook],
                     reinterpret_cast<std::uintptr_t>(player),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_evidence.last_vptr[kFinalWriterHook], observed_vptr,
                     __ATOMIC_RELEASE);
    return;
  }
  LockRuntime();
  const std::uintptr_t vptr = observed_vptr;
  const std::uint32_t tid = Tid();
  g_evidence.last_object[1] = reinterpret_cast<std::uintptr_t>(player);
  g_evidence.last_vptr[1] = vptr;
  const bridge::Result result = bridge::ObserveFinalWriterReturn(
      AdapterConfig(), &g_runtime, reinterpret_cast<std::uintptr_t>(player),
      vptr, tid
#if A9TAS_EXPERIMENTAL_HIGH_REFRESH
      , g_completed_frame_scopes.Current(tid)
#endif
      );
  Note(kFinalWriterHook, result, tid);
  if (result == bridge::Result::kObserved && g_evidence.status != kFault &&
      (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord) ||
       g_control.mode == static_cast<std::uint32_t>(RunMode::kReplay))) {
    if (!NativePhysicsIdentityAlive()) {
      Fault(kErrorPhysicsIdentity);
    } else {
      std::uint8_t transform[recording::kTransformSize]{};
      std::uint8_t linear[recording::kLinearVelocitySize]{};
      const auto* native = reinterpret_cast<const std::uint8_t*>(
          g_control.expected_native_body);
      std::memcpy(transform, native + kNativePoseOffset, sizeof(transform));
      std::memcpy(linear, native + kNativeLinearOffset, sizeof(linear));
      if (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord)) {
        if (g4::CapturePhysicsSnapshot(
                g_runtime.input_action.tick, transform, linear,
                &g_record_physics_snapshot) != g4::Result::kReady)
          Fault(kErrorPhysicsCapture);
      } else {
        g4::PhysicsCorrectionDecisionV1 correction{};
        const g4::Result corrected = g4::DecidePhysicsCorrection(
            g_runtime.input_action, transform, linear, &correction);
        if (corrected != g4::Result::kReady) {
          Fault(kErrorPhysicsCorrection);
        } else if (g_runtime.input_action.tick >= g_control.frame_limit) {
          Fault(kErrorPhysicsCorrection);
        } else {
          // g_recorded_frames is unused by record export in replay mode. Keep
          // one source-shaped, source-bound scratch entry per tick so the host
          // can export the natural state seen after the original Final Writer
          // and before this function changes any vehicle bytes.
          auto diagnostic = g_runtime.input_action.packet;
          std::memcpy(diagnostic.transform_bits, transform,
                      sizeof(diagnostic.transform_bits));
          std::memcpy(diagnostic.linear_velocity_bits, linear,
                      sizeof(diagnostic.linear_velocity_bits));
          diagnostic.reserved = static_cast<std::uint32_t>(correction.kind);
          g_recorded_frames[g_runtime.input_action.tick] = diagnostic;
        }
        if (corrected == g4::Result::kReady && g_evidence.status != kFault &&
            correction.kind ==
                   g4::PhysicsCorrectionKindV1::kSkipped) {
          ++g_evidence.physics_skipped_frames;
        } else if (corrected == g4::Result::kReady &&
                   g_evidence.status != kFault && correction.kind ==
                   g4::PhysicsCorrectionKindV1::kNaturalEqual) {
          ++g_evidence.physics_equal_frames;
        } else if (corrected == g4::Result::kReady &&
                   g_evidence.status != kFault && correction.kind ==
                   g4::PhysicsCorrectionKindV1::kCorrectBoth) {
          auto* writable = reinterpret_cast<std::uint8_t*>(
              g_control.expected_native_body);
          const auto& packet = g_runtime.input_action.packet;
          std::memcpy(writable + kNativePoseOffset, packet.transform_bits,
                      recording::kTransformSize);
          std::memcpy(writable + kNativeLinearOffset,
                      packet.linear_velocity_bits,
                      recording::kLinearVelocitySize);
          __atomic_thread_fence(__ATOMIC_SEQ_CST);
          const bool immediate =
              std::memcmp(writable + kNativePoseOffset,
                          packet.transform_bits,
                          recording::kTransformSize) == 0 &&
              std::memcmp(writable + kNativeLinearOffset,
                          packet.linear_velocity_bits,
                          recording::kLinearVelocitySize) == 0;
          if (!immediate) {
            std::memcpy(writable + kNativePoseOffset, transform,
                        sizeof(transform));
            std::memcpy(writable + kNativeLinearOffset, linear,
                        sizeof(linear));
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            const bool rolled_back =
                std::memcmp(writable + kNativePoseOffset, transform,
                            sizeof(transform)) == 0 &&
                std::memcmp(writable + kNativeLinearOffset, linear,
                            sizeof(linear)) == 0;
            Fault(kErrorPhysicsCorrection);
            if (!rolled_back) __builtin_trap();
          } else {
            ++g_evidence.physics_corrected_frames;
            g_evidence.physics_correction_writes += 2;
          }
        } else if (corrected == g4::Result::kReady &&
                   g_evidence.status != kFault) {
          Fault(kErrorPhysicsCorrection);
        }
      }
    }
  }
  UnlockRuntime();
}

#if A9TAS_EXPERIMENTAL_HIGH_REFRESH
extern "C" __attribute__((noinline, visibility("hidden"))) bool
G4FrameScopeEnterV1(void* context, std::uintptr_t caller_return) {
  LockRuntime();
  const std::uint32_t tid = Tid();
  bridge::CompletedFrameScopeV1 witness{};
  const auto config = AdapterConfig();
  if (g_control.enabled && !g_control.completed &&
      config.allow_zero_integration_updates &&
      reinterpret_cast<std::uintptr_t>(context) == config.tick.expected_begin_owner &&
      caller_return == config.tick.game_base + config.tick.frame_event_scheduler_return_rva &&
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      g_runtime.tick.coordinator.tick_phase != coordinator::TickPhase::kClosed) {
    witness = {caller_return, config.tick.generation, tid,
               g_runtime.tick.coordinator.tick};
  }
  const bool tracked = g_completed_frame_scopes.Push(tid, witness);
  UnlockRuntime();
  return tracked;
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4FrameScopeLeaveV1(bool tracked) {
  if (!tracked) return;
  LockRuntime();
  g_completed_frame_scopes.Pop(Tid());
  UnlockRuntime();
}
#endif

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4FrameAfterV1(std::uintptr_t caller_return) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kFrameEventHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) return;
  LockRuntime();
  const std::uint32_t tid = Tid();
  g_evidence.last_caller_return = caller_return;
  const auto* phase = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  const std::uint32_t lifecycle =
      __atomic_load_n(phase, __ATOMIC_RELAXED);
  g_evidence.last_lifecycle_state = lifecycle;
  (void)MaybeDiscardOpenLifecycleTick(lifecycle);
  if (MaybeCompleteLifecycleRecording(lifecycle)) {
    UnlockRuntime();
    return;
  }
  const std::uint32_t receipt_count_before = g_runtime.receipt_count;
  const bool recording_before_tick_end =
      g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord);
  const bridge::Result result = bridge::ObserveTickEndReturn(
      AdapterConfig(), &g_runtime, caller_return, tid);
  Note(kFrameEventHook, result, tid);
  if (recording_before_tick_end &&
      g_evidence.status != kFault &&
      g_runtime.receipt_count == receipt_count_before + 1) {
    if (receipt_count_before >= kMaximumFrames) {
      Fault(kErrorRecordingCapacity);
    } else {
      const auto& receipt = g_runtime.receipts[receipt_count_before];
      const std::uint64_t monotonic_ns =
          receipt.tick * static_cast<std::uint64_t>(g_control.fixed_delta_us) *
          1000ULL;
      if (g4::BuildPhysicsRecordingFrame(
              receipt, g_record_physics_snapshot, monotonic_ns,
              &g_recorded_frames[receipt_count_before],
              AdapterConfig().allow_zero_integration_updates) != g4::Result::kReady) {
        Fault(kErrorAdapter);
      } else {
        if (g_barrel_capture_tick == receipt.tick) {
          std::memcpy(
              g_recorded_frames[receipt_count_before].barrel_angular_velocity,
              g_barrel_capture.angular_bits,
              sizeof(g_barrel_capture.angular_bits));
          std::memcpy(g_recorded_frames[receipt_count_before].barrel_rbx,
                      g_barrel_capture.rbx_bits,
                      sizeof(g_barrel_capture.rbx_bits));
        }
        if (!g4::PhysicsRecordingFrameValid(
                g_recorded_frames[receipt_count_before])) {
          Fault(kErrorBarrelTail);
        } else {
          ++g_evidence.recorded_frames;
          g_record_physics_snapshot = {};
        }
      }
    }
  }
  if (g_runtime.tick.coordinator.tick_phase ==
      coordinator::TickPhase::kClosed)
    g_tick_semantic_start = {};
  UnlockRuntime();
}

extern "C" __attribute__((noinline, visibility("hidden"))) std::uint32_t
G4NitroBeforeV1(void* nitro_state) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kNitroHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
      nitro_state == nullptr)
    return 1;
  LockRuntime();
  const std::uint32_t tid = Tid();
  const std::uintptr_t vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(nitro_state),
      __ATOMIC_RELAXED);
  g_evidence.last_object[kNitroHook] =
      reinterpret_cast<std::uintptr_t>(nitro_state);
  g_evidence.last_vptr[kNitroHook] = vptr;
  if (reinterpret_cast<std::uintptr_t>(nitro_state) !=
      g_control.expected_nitro_state) {
    ++g_evidence.ignored_events;
    UnlockRuntime();
    return 1;
  }
  // The hook is process-wide while the session is armed, but AluTasV2 only
  // suppresses a natural activation when an exact replay packet is active.
  // Calls outside the currently open tick must remain transparent.
  if (!g_runtime.input_action.tick_open) {
    ++g_evidence.ignored_events;
    UnlockRuntime();
    return 1;
  }
  bool call_original = true;
  const bridge::Result result =
      bridge::ObserveNaturalNitroCall(&g_runtime, &call_original);
  Note(kNitroHook, result, tid);
  if (result == bridge::Result::kObserved) {
    if (call_original)
      ++g_evidence.natural_nitro_calls;
    else
      ++g_evidence.suppressed_nitro_calls;
  }
  UnlockRuntime();
  return call_original ? 1u : 0u;
}

barrel::ActiveFrame CurrentBarrelFrame() {
  barrel::ActiveFrame active{};
  if (g_control.mode != static_cast<std::uint32_t>(RunMode::kReplay) ||
      !g_runtime.input_action.tick_open ||
      !g_runtime.input_action.replay_packet_present)
    return active;
  active.has_value = true;
  active.permit.generation = g_control.generation;
  active.permit.frame_index =
      static_cast<std::uint32_t>(g_runtime.input_action.tick);
  active.skip_override_flags =
      g_runtime.input_action.packet.skip_override_flags;
  std::memcpy(active.angular_bits,
              g_runtime.input_action.packet.barrel_angular_velocity,
              sizeof(active.angular_bits));
  std::memcpy(active.rbx_bits, g_runtime.input_action.packet.barrel_rbx,
              sizeof(active.rbx_bits));
  return active;
}

void NoteBarrelTail(std::uint32_t hook, barrel::Result result,
                    std::uint32_t tid) {
  if (result == barrel::Result::kInvalidArgument ||
      result == barrel::Result::kStalePermit) {
    ++g_evidence.core_faults;
    Fault(kErrorBarrelTail);
    return;
  }
  if (g_evidence.first_tid[hook] == 0) g_evidence.first_tid[hook] = tid;
  g_evidence.last_tid[hook] = tid;
  ++g_evidence.qualified_events[hook];
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4BarrelRollAfterV1(void* owner) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kBarrelRollHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
      owner == nullptr)
    return;
  LockRuntime();
  const std::uint32_t tid = Tid();
  const std::uintptr_t object = reinterpret_cast<std::uintptr_t>(owner);
  const std::uintptr_t vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(owner), __ATOMIC_RELAXED);
  g_evidence.last_object[kBarrelRollHook] = object;
  g_evidence.last_vptr[kBarrelRollHook] = vptr;
  if (!g_runtime.input_action.tick_open ||
      object != g_control.expected_barrel_owner) {
    ++g_evidence.ignored_events;
    UnlockRuntime();
    return;
  }
  if (vptr != g_control.expected_barrel_owner_vptr ||
      object > UINTPTR_MAX - kBarrelRbxOffset) {
    Fault(kErrorBarrelIdentity);
    UnlockRuntime();
    return;
  }
  auto* live = reinterpret_cast<std::uint32_t*>(object + kBarrelRbxOffset);
  const barrel::ActiveFrame active = CurrentBarrelFrame();
  const barrel::PostOriginalContext context = {
      .expected_permit = {.generation = g_control.generation,
                          .frame_index = static_cast<std::uint32_t>(
                              g_runtime.input_action.tick),
                          .reserved = 0},
      .original_returned = true,
      .padding = {},
  };
  const barrel::Result result = barrel::OnBarrelRollPostOriginal(
      active, context, live, &g_barrel_capture);
  NoteBarrelTail(kBarrelRollHook, result, tid);
  if (result == barrel::Result::kOverridden)
    ++g_evidence.barrel_rbx_overrides;
  if (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord))
    ++g_evidence.barrel_rbx_records;
  UnlockRuntime();
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4BarrelYawAfterV1(void* owner) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kBarrelYawHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
      owner == nullptr)
    return;
  LockRuntime();
  const std::uint32_t tid = Tid();
  const std::uintptr_t object = reinterpret_cast<std::uintptr_t>(owner);
  const std::uintptr_t vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(owner), __ATOMIC_RELAXED);
  g_evidence.last_object[kBarrelYawHook] = object;
  g_evidence.last_vptr[kBarrelYawHook] = vptr;
  if (!g_runtime.input_action.tick_open ||
      object != g_control.expected_barrel_owner) {
    ++g_evidence.ignored_events;
    UnlockRuntime();
    return;
  }
  if (vptr != g_control.expected_barrel_owner_vptr ||
      object > UINTPTR_MAX - kBarrelBackendOffset) {
    Fault(kErrorBarrelIdentity);
    UnlockRuntime();
    return;
  }
  const std::uintptr_t backend = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(
          object + kBarrelBackendOffset),
      __ATOMIC_RELAXED);
  if (backend != g_control.expected_physics_velocity_interface ||
      backend > UINTPTR_MAX - kBarrelBackendNativeBodyOffset) {
    Fault(kErrorBarrelIdentity);
    UnlockRuntime();
    return;
  }
  const std::uintptr_t native = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(
          backend + kBarrelBackendNativeBodyOffset),
      __ATOMIC_RELAXED);
  if (native != g_control.expected_native_body ||
      native > UINTPTR_MAX - recording::kAngularVelocityOffset) {
    Fault(kErrorBarrelIdentity);
    UnlockRuntime();
    return;
  }
  auto* live = reinterpret_cast<std::uint32_t*>(
      native + recording::kAngularVelocityOffset);
  const barrel::ActiveFrame active = CurrentBarrelFrame();
  const barrel::PostOriginalContext context = {
      .expected_permit = {.generation = g_control.generation,
                          .frame_index = static_cast<std::uint32_t>(
                              g_runtime.input_action.tick),
                          .reserved = 0},
      .original_returned = true,
      .padding = {},
  };
  const barrel::Result result = barrel::OnBarrelYawPostOriginal(
      active, context, live, &g_barrel_capture);
  NoteBarrelTail(kBarrelYawHook, result, tid);
  if (result == barrel::Result::kOverridden)
    ++g_evidence.barrel_angular_overrides;
  if (g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord))
    ++g_evidence.barrel_angular_records;
  UnlockRuntime();
}

#define G4_SAVE_BEFORE_HELPER                         \
  "sub sp, sp, #0x140\n"                             \
  "stp x0, x1, [sp, #0x00]\n"                       \
  "stp x2, x3, [sp, #0x10]\n"                       \
  "stp x4, x5, [sp, #0x20]\n"                       \
  "stp x6, x7, [sp, #0x30]\n"                       \
  "stp x8, x9, [sp, #0x40]\n"                       \
  "stp x10, x11, [sp, #0x50]\n"                     \
  "stp x12, x13, [sp, #0x60]\n"                     \
  "stp x14, x15, [sp, #0x70]\n"                     \
  "stp x16, x17, [sp, #0x80]\n"                     \
  "str x18, [sp, #0x90]\n"                          \
  "str x30, [sp, #0x98]\n"                          \
  "stp q0, q1, [sp, #0xA0]\n"                       \
  "stp q2, q3, [sp, #0xC0]\n"                       \
  "stp q4, q5, [sp, #0xE0]\n"                       \
  "stp q6, q7, [sp, #0x100]\n"                      \
  "mrs x9, nzcv\n"                                      \
  "mrs x10, fpcr\n"                                     \
  "stp x9, x10, [sp, #0x120]\n"                     \
  "mrs x9, fpsr\n"                                      \
  "str x9, [sp, #0x130]\n"

#define G4_RESTORE_BEFORE_HELPER                      \
  "ldr x9, [sp, #0x130]\n"                          \
  "msr fpsr, x9\n"                                      \
  "ldp x9, x10, [sp, #0x120]\n"                     \
  "msr nzcv, x9\n"                                      \
  "msr fpcr, x10\n"                                     \
  "ldp q6, q7, [sp, #0x100]\n"                      \
  "ldp q4, q5, [sp, #0xE0]\n"                       \
  "ldp q2, q3, [sp, #0xC0]\n"                       \
  "ldp q0, q1, [sp, #0xA0]\n"                       \
  "ldr x30, [sp, #0x98]\n"                          \
  "ldr x18, [sp, #0x90]\n"                          \
  "ldp x16, x17, [sp, #0x80]\n"                     \
  "ldp x14, x15, [sp, #0x70]\n"                     \
  "ldp x12, x13, [sp, #0x60]\n"                     \
  "ldp x10, x11, [sp, #0x50]\n"                     \
  "ldp x8, x9, [sp, #0x40]\n"                       \
  "ldp x6, x7, [sp, #0x30]\n"                       \
  "ldp x4, x5, [sp, #0x20]\n"                       \
  "ldp x2, x3, [sp, #0x10]\n"                       \
  "ldp x0, x1, [sp, #0x00]\n"                       \
  "add sp, sp, #0x140\n"

// The after-original wrappers use the G2/G3-validated full volatile-register,
// SIMD, NZCV, FPCR and FPSR preservation sequence.
#define G4_AFTER_ORIGINAL_SAVE                         \
  "stp x0, x1, [sp, #0x20]\n"                       \
  "stp x2, x3, [sp, #0x30]\n"                       \
  "stp x4, x5, [sp, #0x40]\n"                       \
  "stp x6, x7, [sp, #0x50]\n"                       \
  "stp x8, x9, [sp, #0x60]\n"                       \
  "stp x10, x11, [sp, #0x70]\n"                     \
  "stp x12, x13, [sp, #0x80]\n"                     \
  "stp x14, x15, [sp, #0x90]\n"                     \
  "stp x16, x17, [sp, #0xA0]\n"                     \
  "str x18, [sp, #0xB0]\n"                          \
  "stp q0, q1, [sp, #0xC0]\n"                       \
  "stp q2, q3, [sp, #0xE0]\n"                       \
  "stp q4, q5, [sp, #0x100]\n"                      \
  "stp q6, q7, [sp, #0x120]\n"                      \
  "mrs x9, nzcv\n"                                      \
  "mrs x10, fpcr\n"                                     \
  "stp x9, x10, [sp, #0x140]\n"                     \
  "mrs x9, fpsr\n"                                      \
  "str x9, [sp, #0x150]\n"

#define G4_AFTER_ORIGINAL_RESTORE                      \
  "ldr x9, [sp, #0x150]\n"                          \
  "msr fpsr, x9\n"                                      \
  "ldp x9, x10, [sp, #0x140]\n"                     \
  "msr nzcv, x9\n"                                      \
  "msr fpcr, x10\n"                                     \
  "ldp q6, q7, [sp, #0x120]\n"                      \
  "ldp q4, q5, [sp, #0x100]\n"                      \
  "ldp q2, q3, [sp, #0xE0]\n"                       \
  "ldp q0, q1, [sp, #0xC0]\n"                       \
  "ldr x18, [sp, #0xB0]\n"                          \
  "ldp x16, x17, [sp, #0xA0]\n"                     \
  "ldp x14, x15, [sp, #0x90]\n"                     \
  "ldp x12, x13, [sp, #0x80]\n"                     \
  "ldp x10, x11, [sp, #0x70]\n"                     \
  "ldp x8, x9, [sp, #0x60]\n"                       \
  "ldp x6, x7, [sp, #0x50]\n"                       \
  "ldp x4, x5, [sp, #0x40]\n"                       \
  "ldp x2, x3, [sp, #0x30]\n"                       \
  "ldp x0, x1, [sp, #0x20]\n"

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4LifecycleEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x0\n"
      G4_SAVE_BEFORE_HELPER
      "ldr x0, [sp, #0x00]\n"
      "bl G4LifecycleBeforeV1\n"
      "mov w20, w0\n"
      G4_RESTORE_BEFORE_HELPER
      "mov x0, x19\n"
      "bl G4LifecycleOriginalV1\n"
      G4_AFTER_ORIGINAL_SAVE
      "mov x0, x19\n"
      "mov w1, w20\n"
      "bl G4LifecycleAfterV1\n"
      G4_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4IntervalEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x0\n"
      // Reuse the live-proven G2/GetPhysicsInterval ABI: X8 is the four-byte
      // output pointer. The original returns a pointer in X0; it does not
      // return the interval value in S0.
      "str x8, [sp, #0x18]\n"
      G4_SAVE_BEFORE_HELPER
      "ldr x0, [sp, #0x00]\n"
      "bl G4IntervalBeforeV1\n"
      "mov w20, w0\n"
      G4_RESTORE_BEFORE_HELPER
      "mov x0, x19\n"
      "bl G4IntervalOriginalV1\n"
      G4_AFTER_ORIGINAL_SAVE
      "ldr x0, [sp, #0x18]\n"
      "mov w1, w20\n"
      "bl G4IntervalAfterV1\n"
      G4_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4SubmitEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x20\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x0\n"
      "mov x20, x1\n"
      G4_SAVE_BEFORE_HELPER
      "ldp x0, x1, [sp, #0x00]\n"
      "bl G4SubmitBeforeV1\n"
      G4_RESTORE_BEFORE_HELPER
      "mov x0, x19\n"
      "mov x1, x20\n"
      "bl G4SubmitOriginalV1\n"
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x20\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4FinalEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x0\n"
      "bl G4FinalOriginalV1\n"
      G4_AFTER_ORIGINAL_SAVE
      "mov x0, x19\n"
      "bl G4FinalAfterV1\n"
      G4_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4FrameEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x30\n"
#if A9TAS_EXPERIMENTAL_HIGH_REFRESH
      G4_SAVE_BEFORE_HELPER
      "mov x1, x19\n"
      "bl G4FrameScopeEnterV1\n"
      "mov w20, w0\n"
      G4_RESTORE_BEFORE_HELPER
#endif
      "bl G4FrameOriginalV1\n"
      G4_AFTER_ORIGINAL_SAVE
      "mov x0, x19\n"
      "bl G4FrameAfterV1\n"
#if A9TAS_EXPERIMENTAL_HIGH_REFRESH
      "mov w0, w20\n"
      "bl G4FrameScopeLeaveV1\n"
#endif
      G4_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4NitroEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x20\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x0\n"
      G4_SAVE_BEFORE_HELPER
      "ldr x0, [sp, #0x00]\n"
      "bl G4NitroBeforeV1\n"
      "mov w20, w0\n"
      G4_RESTORE_BEFORE_HELPER
      "cbz w20, 1f\n"
      "mov x0, x19\n"
      "bl G4NitroOriginalV1\n"
      "1:\n"
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x20\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4BarrelRollEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "bl G4BarrelRollOriginalV1\n"
      G4_AFTER_ORIGINAL_SAVE
      "ldr x0, [sp, #0x00]\n"
      "bl G4BarrelRollAfterV1\n"
      G4_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G4BarrelYawEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "bl G4BarrelYawOriginalV1\n"
      G4_AFTER_ORIGINAL_SAVE
      "ldr x0, [sp, #0x00]\n"
      "bl G4BarrelYawAfterV1\n"
      G4_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

using G4SetterFunctionV1 = void (*)(void*, float*);

bool G4SetterOwnerQualifiedV1(void* owner) {
  if (owner == nullptr ||
      reinterpret_cast<std::uintptr_t>(owner) !=
          g_control.expected_setter_object)
    return false;
  return __atomic_load_n(
             reinterpret_cast<const std::uintptr_t*>(owner),
             __ATOMIC_RELAXED) == g_control.expected_setter_vptr;
}

bool G4SetterValueReadableV1(const void* value) {
  // The untouched game setter dereferences this same float pointer
  // immediately.  Re-scanning /proc/self/maps on every high-frequency input
  // event adds latency without proving anything the natural call does not
  // already require.
  return value != nullptr &&
         (reinterpret_cast<std::uintptr_t>(value) & 3u) == 0;
}

void G4NoteSetterV1(std::uint32_t index, void* owner,
                    std::uint32_t final_bits, bool overridden) {
  ++g_evidence.setter_qualified_events[index];
  if (overridden) ++g_evidence.setter_overrides[index];
  g_evidence.setter_last_object[index] =
      reinterpret_cast<std::uintptr_t>(owner);
  g_evidence.setter_last_bits[index] = final_bits;
  g_evidence.setter_cache_bits[kBrakeSetter] =
      g_runtime.input_action.setter_cache.brake_bits;
  g_evidence.setter_cache_bits[kSteeringSetter] =
      g_runtime.input_action.setter_cache.steering_bits;
  g_evidence.setter_cache_bits[kAcceleratorSetter] =
      g_runtime.input_action.setter_cache.accelerator_bits;
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4BrakeSetterEntryV1(void* owner, float* value) {
  const auto original = reinterpret_cast<G4SetterFunctionV1>(
      g_control.setter_original[kBrakeSetter]);
  // A completed lifecycle recording is an immutable receipt.  Retry can make
  // the next race call this permanent pass-through hook before its pending
  // generation is activated.  Those calls must not overwrite the previous
  // generation's setter identity or counters.
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) {
    original(owner, value);
    return;
  }
  __atomic_fetch_add(&g_evidence.setter_entries[kBrakeSetter], 1ULL,
                     __ATOMIC_RELAXED);
  if (G4SetterOwnerQualifiedV1(owner)) {
    if (!G4SetterValueReadableV1(value)) {
      Fault(kErrorSetterValue);
    } else {
      const std::uint32_t natural_bits = __atomic_load_n(
          reinterpret_cast<const std::uint32_t*>(value), __ATOMIC_RELAXED);
      LockRuntime();
      g4::SetterDecisionV1 decision{};
      const bridge::Result result = bridge::ObserveBrakeBeforeOriginal(
          &g_runtime, natural_bits, &decision);
      if (result != bridge::Result::kObserved) {
        ++g_evidence.core_faults;
        Fault(kErrorAdapter);
      } else {
        if (decision.replay_override)
          __atomic_store_n(reinterpret_cast<std::uint32_t*>(value),
                           decision.final_bits, __ATOMIC_RELAXED);
        G4NoteSetterV1(kBrakeSetter, owner, decision.final_bits,
                      decision.replay_override);
      }
      UnlockRuntime();
    }
  } else {
    ++g_evidence.ignored_events;
  }
  original(owner, value);
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4SteeringSetterEntryV1(void* owner, float* value) {
  const auto original = reinterpret_cast<G4SetterFunctionV1>(
      g_control.setter_original[kSteeringSetter]);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) {
    original(owner, value);
    return;
  }
  __atomic_fetch_add(&g_evidence.setter_entries[kSteeringSetter], 1ULL,
                     __ATOMIC_RELAXED);
  if (G4SetterOwnerQualifiedV1(owner)) {
    if (!G4SetterValueReadableV1(value)) {
      Fault(kErrorSetterValue);
    } else {
      const std::uint32_t natural_bits = __atomic_load_n(
          reinterpret_cast<const std::uint32_t*>(value), __ATOMIC_RELAXED);
      LockRuntime();
      g4::SetterDecisionV1 decision{};
      const bridge::Result result = bridge::ObserveSteeringBeforeOriginal(
          &g_runtime, natural_bits, &decision);
      if (result != bridge::Result::kObserved) {
        ++g_evidence.core_faults;
        Fault(kErrorAdapter);
      } else {
        if (decision.replay_override)
          __atomic_store_n(reinterpret_cast<std::uint32_t*>(value),
                           decision.final_bits, __ATOMIC_RELAXED);
        G4NoteSetterV1(kSteeringSetter, owner, decision.final_bits,
                      decision.replay_override);
      }
      UnlockRuntime();
    }
  } else {
    ++g_evidence.ignored_events;
  }
  original(owner, value);
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G4AcceleratorSetterEntryV1(void* owner, float* value) {
  const auto original = reinterpret_cast<G4SetterFunctionV1>(
      g_control.setter_original[kAcceleratorSetter]);
  // AluTasV2's accelerator detour is intentionally after-original.
  original(owner, value);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) return;
  __atomic_fetch_add(&g_evidence.setter_entries[kAcceleratorSetter], 1ULL,
                     __ATOMIC_RELAXED);
  if (!G4SetterOwnerQualifiedV1(owner)) {
    ++g_evidence.ignored_events;
    return;
  }
  auto* member = reinterpret_cast<std::uint32_t*>(
      reinterpret_cast<std::uintptr_t>(owner) + kControlPairOffset + 8);
  if (!G4SetterValueReadableV1(member)) {
    Fault(kErrorSetterValue);
    return;
  }
  const std::uint32_t natural_bits =
      __atomic_load_n(member, __ATOMIC_RELAXED);
  LockRuntime();
  std::uint32_t final_bits = natural_bits;
  const bridge::Result result = bridge::ObserveAcceleratorAfterOriginal(
      &g_runtime, natural_bits, &final_bits);
  if (result != bridge::Result::kObserved) {
    ++g_evidence.core_faults;
    Fault(kErrorAdapter);
  } else {
    const bool overridden = final_bits != natural_bits;
    if (overridden)
      __atomic_store_n(member, final_bits, __ATOMIC_RELAXED);
    G4NoteSetterV1(kAcceleratorSetter, owner, final_bits, overridden);
  }
  UnlockRuntime();
}

namespace {

const std::uintptr_t kWrappers[kHookCount] = {
    reinterpret_cast<std::uintptr_t>(&G4IntervalEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4FinalEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4FrameEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4NitroEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4SubmitEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4BarrelRollEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4BarrelYawEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4DispatcherEntryV1),
};

const std::uintptr_t kRandomWrappers[kRandomHookCount] = {
    reinterpret_cast<std::uintptr_t>(&G4BarrelRandomBoolEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4BarrelRandomLerpEntryV1),
};

const std::uintptr_t kSetterWrappers[kInstalledSetterCount] = {
    reinterpret_cast<std::uintptr_t>(&G4BrakeSetterEntryV1),
    reinterpret_cast<std::uintptr_t>(&G4SteeringSetterEntryV1),
};

const std::uintptr_t kLifecycleWrapper =
    reinterpret_cast<std::uintptr_t>(&G4LifecycleEntryV1);

bool NonzeroSha256(const std::uint8_t hash[32]) {
  std::uint8_t combined = 0;
  for (std::uint32_t index = 0; index < 32; ++index) combined |= hash[index];
  return combined != 0;
}

bool PassiveControlFresh() {
  if (std::memcmp(g_control.magic, kControlMagic, sizeof(kControlMagic)) != 0 ||
      g_control.version != kVersion || g_control.size != sizeof(Control) ||
      g_control.enabled != 0 || g_control.frame_limit != 0 ||
      g_control.generation != 0 || g_control.completed != 0 ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kNeutral) ||
      g_control.active_helpers != 0 || g_control.fixed_delta_us != 0 ||
      g_control.replay_frame_count != 0 ||
      g_control.replay_interval_count != 0 ||
      g_control.completion_policy !=
          static_cast<std::uint32_t>(CompletionPolicy::kFixedFrameLimit) ||
      g_control.session_id != 0 || g_control.expected_begin_owner != 0 ||
      g_control.expected_begin_owner_vptr != 0 ||
      g_control.expected_interval_owner != 0 ||
      g_control.expected_interval_owner_vptr != 0 ||
      g_control.expected_player != 0 || g_control.expected_player_vptr != 0 ||
      g_control.lifecycle_state_address != 0 ||
      g_control.expected_main_object != 0 ||
      g_control.expected_main_object_vptr != 0 ||
      g_control.expected_nitro_state != 0 ||
      g_control.expected_setter_object != 0 ||
      g_control.expected_setter_vptr != 0 ||
      g_control.expected_backend_interface != 0 ||
      g_control.expected_physics_velocity_interface != 0 ||
      g_control.expected_native_body != 0 ||
      g_control.expected_barrel_owner != 0 ||
      g_control.expected_barrel_owner_vptr != 0 ||
      g_control.game_base != 0 || g_control.replay_speed_factor != 0 ||
      g_control.replay_speed_reserved != 0 ||
      g_control.lifecycle_target_entry != 0 ||
      g_control.lifecycle_target_tail != 0 ||
      g_control.lifecycle_wrapper != 0 || g_control.pending_mode != 0 ||
      g_control.pending_state != 0 || g_control.pending_generation != 0 ||
      g_control.pending_reserved != 0)
    return false;
  for (std::uint32_t index = 0; index < kHookCount; ++index)
    if (g_control.target_entry[index] != 0 ||
        g_control.target_tail[index] != 0 || g_control.wrapper[index] != 0)
      return false;
  for (std::uint32_t index = 0; index < kSetterCount; ++index)
    if (g_control.setter_slot[index] != 0 ||
        g_control.setter_original[index] != 0 ||
        g_control.setter_wrapper[index] != 0)
      return false;
  if (g_control.archived_generation != 0 ||
      g_control.archived_frame_count != 0 ||
      g_control.archived_interval_count != 0)
    return false;
  for (std::uint8_t value : g_control.recording_sha256)
    if (value != 0) return false;
  return true;
}

bool ArmControlValid(bool archived_rearm = false,
                     bool active_replay_handoff = false,
                     std::uint32_t expected_active_helpers = 0) {
  g_evidence.reserved[0] = 0x1000;
  const std::uint32_t observed_active_helpers = __atomic_load_n(
      &g_control.active_helpers, __ATOMIC_ACQUIRE);
  if (observed_active_helpers != expected_active_helpers) {
    g_evidence.reserved[0] = 0x100000000ULL |
        (static_cast<std::uint64_t>(expected_active_helpers) << 16) |
        observed_active_helpers;
    return false;
  }
  if (std::memcmp(g_control.magic, kControlMagic, sizeof(kControlMagic)) != 0 ||
      g_control.version != kVersion || g_control.size != sizeof(Control) ||
      g_control.enabled != (active_replay_handoff ? 1u : 0u) ||
      g_control.completed !=
          (active_replay_handoff ? 0u : archived_rearm ? 1u : 0u) ||
      (g_control.mode != static_cast<std::uint32_t>(RunMode::kNeutral) &&
       g_control.mode != static_cast<std::uint32_t>(RunMode::kRecord) &&
       g_control.mode != static_cast<std::uint32_t>(RunMode::kReplay)) ||
       g_control.generation == 0 || g_control.session_id == 0 ||
      g_control.expected_begin_owner == 0 ||
      g_control.expected_begin_owner_vptr == 0 ||
      g_control.expected_begin_owner_vptr !=
          g_control.game_base +
              g_build_profile.physics_context_vtable_rva ||
      g_control.expected_interval_owner == 0 ||
      g_control.expected_interval_owner_vptr == 0 ||
      g_control.expected_player == 0 || g_control.expected_player_vptr == 0 ||
      g_control.lifecycle_state_address == 0 ||
      g_control.expected_main_object == 0 ||
      g_control.expected_main_object_vptr !=
          g_control.game_base + g_build_profile.main_time_source_vtable_rva ||
      g_control.expected_nitro_state == 0 ||
      g_control.expected_setter_object == 0 ||
      g_control.expected_setter_vptr !=
          g_control.game_base + g_build_profile.adjusted_setter_vtable_rva ||
      g_control.expected_backend_interface == 0 ||
      g_control.expected_physics_velocity_interface == 0 ||
      g_control.expected_native_body == 0 ||
      g_control.expected_barrel_owner == 0 ||
      (g_control.expected_barrel_owner_vptr !=
           g_control.game_base + g_build_profile.vehicle_source_vtable_rva &&
       g_control.expected_barrel_owner_vptr !=
           g_control.game_base +
               g_build_profile.car_physics_body_source_vtable_rva) ||
      g_control.expected_interval_owner >
          UINTPTR_MAX - kAdjustedSetterObjectOffset ||
      g_control.expected_setter_object !=
          g_control.expected_interval_owner + kAdjustedSetterObjectOffset ||
      g_control.game_base == 0 ||
      g_control.fixed_delta_us < recording::kMinimumFixedIntervalUs ||
      g_control.fixed_delta_us > recording::kMaximumFixedIntervalUs)
    return false;
  const RunMode mode = static_cast<RunMode>(g_control.mode);
  const auto completion =
      static_cast<CompletionPolicy>(g_control.completion_policy);
  g_evidence.reserved[0] = 0x1100;
  if (!ArmFrameLimitValid(g_control.frame_limit, mode, completion,
                          archived_rearm))
    return false;
  if (mode == RunMode::kReplay) {
    g_evidence.reserved[0] = 0x1200;
    if (g_control.replay_frame_count != g_control.frame_limit ||
        (g_control.replay_interval_count == 0 &&
         !AdapterConfig().allow_zero_integration_updates) ||
        g_control.replay_interval_count > kMaximumIntervalSamples ||
        !NonzeroSha256(g_control.recording_sha256) || !ReplayBuffersValid() ||
        g_control.replay_speed_factor < kMinimumReplaySpeedFactor ||
        g_control.replay_speed_factor > kMaximumReplaySpeedFactor ||
        g_control.replay_speed_reserved > kReplayCompletionAtomicRecord)
      return false;
    if (g_control.replay_speed_reserved == kReplayCompletionAtomicRecord &&
        g_control.generation == UINT32_MAX)
      return false;
  } else if (g_control.replay_frame_count != 0 ||
             g_control.replay_interval_count != 0 ||
             NonzeroSha256(g_control.recording_sha256) ||
             g_control.replay_speed_factor != 1 ||
             g_control.replay_speed_reserved != 0) {
    return false;
  }
  g_evidence.reserved[0] = 0x1300;
  for (std::uint32_t index = 0; index < kHookCount; ++index)
    if (g_control.target_entry[index] !=
            g_control.game_base + g_build_profile.hook_rvas[index] ||
        g_control.target_tail[index] !=
            g_control.target_entry[index] + HookPatchSize(index) ||
        g_control.wrapper[index] != kWrappers[index]) {
      g_evidence.reserved[0] = 0x13000000ULL | index;
      return false;
    }
  if (g_control.lifecycle_target_entry !=
          g_control.game_base + g_build_profile.lifecycle_shared_enter_rva ||
      g_control.lifecycle_target_tail !=
          g_control.lifecycle_target_entry + kPatchSize ||
      g_control.lifecycle_wrapper != kLifecycleWrapper)
    return false;
  g_evidence.reserved[0] = 0x1400;
  for (std::uint32_t index = 0; index < kInstalledSetterCount; ++index)
    if (g_control.setter_slot[index] !=
            g_control.game_base + g_build_profile.adjusted_setter_vtable_rva +
                kSetterSlotOffsets[index] ||
        g_control.setter_original[index] !=
            g_control.game_base +
                g_build_profile.setter_original_rvas[index] ||
        g_control.setter_wrapper[index] != kSetterWrappers[index]) {
      g_evidence.reserved[0] = 0x14000000ULL | index;
      return false;
    }
  for (std::uint32_t index = kInstalledSetterCount; index < kSetterCount;
       ++index)
    if (g_control.setter_slot[index] != 0 ||
        g_control.setter_original[index] != 0 ||
        g_control.setter_wrapper[index] != 0) {
      g_evidence.reserved[0] = 0x15000000ULL | index;
      return false;
    }
  g_evidence.reserved[0] = 0x1600;
  if (archived_rearm) {
    if (g_control.archived_generation == 0 ||
        g_control.archived_generation >= UINT32_MAX ||
        g_control.generation != g_control.archived_generation + 1u ||
        g_control.archived_frame_count == 0 ||
        g_control.archived_frame_count > kMaximumFrames ||
        (g_control.archived_interval_count == 0 &&
         !AdapterConfig().allow_zero_integration_updates) ||
        g_control.archived_interval_count > kMaximumIntervalSamples)
      return false;
    if (mode == RunMode::kReplay) {
      if (completion != CompletionPolicy::kFixedFrameLimit)
        return false;
    } else if (mode == RunMode::kRecord) {
      if (completion != CompletionPolicy::kRaceLifecycle ||
          g_control.archived_frame_count > g_control.frame_limit ||
          g_control.archived_interval_count > kMaximumIntervalSamples)
        return false;
    } else {
      return false;
    }
  } else if (g_control.archived_generation != 0 ||
             g_control.archived_frame_count != 0 ||
             g_control.archived_interval_count != 0) {
    return false;
  }
  g_evidence.reserved[0] = 0x1fff;
  return true;
}

bool ObjectIdentity(std::uint64_t object, std::uint64_t expected_vptr) {
  const auto* pointer = reinterpret_cast<const std::uintptr_t*>(object);
  return PrivateRw(QueryMapping(pointer), pointer, sizeof(*pointer)) &&
         __atomic_load_n(pointer, __ATOMIC_RELAXED) == expected_vptr;
}

bool ReadPrivateValue(std::uintptr_t address, void* output,
                      std::size_t size) {
  if (address == 0 || output == nullptr || size == 0) return false;
  const auto* source = reinterpret_cast<const void*>(address);
  const MappingMetadata mapping = QueryMapping(source);
  if (!MappingCovers(mapping, source, size) || !mapping.private_mapping ||
      (mapping.protection & PROT_READ) == 0)
    return false;
  std::memcpy(output, source, size);
  return true;
}

bool ApplyPointerAdjustment(std::uintptr_t base, std::int64_t adjustment,
                            std::uintptr_t* output) {
  if (output == nullptr) return false;
  if (adjustment >= 0) {
    const std::uint64_t positive = static_cast<std::uint64_t>(adjustment);
    if (positive > UINTPTR_MAX - base) return false;
    *output = base + static_cast<std::uintptr_t>(positive);
    return *output != 0;
  }
  const std::uint64_t magnitude =
      static_cast<std::uint64_t>(-(adjustment + 1)) + 1u;
  if (magnitude > base) return false;
  *output = base - static_cast<std::uintptr_t>(magnitude);
  return *output != 0;
}

bool BindObservedRaceObjects(void* lifecycle_object) {
  if (lifecycle_object == nullptr ||
      reinterpret_cast<std::uintptr_t>(lifecycle_object) >
          UINTPTR_MAX - 0x2d8u)
    return false;
  const std::uintptr_t begin = __atomic_load_n(
      &g_evidence.last_object[kSubmitHook], __ATOMIC_ACQUIRE);
  const std::uintptr_t begin_vptr = __atomic_load_n(
      &g_evidence.last_vptr[kSubmitHook], __ATOMIC_ACQUIRE);
  const std::uintptr_t interval = __atomic_load_n(
      &g_evidence.last_object[kTickHook], __ATOMIC_ACQUIRE);
  const std::uintptr_t interval_vptr = __atomic_load_n(
      &g_evidence.last_vptr[kTickHook], __ATOMIC_ACQUIRE);
  const std::uintptr_t player = __atomic_load_n(
      &g_evidence.last_object[kFinalWriterHook], __ATOMIC_ACQUIRE);
  const std::uintptr_t player_vptr = __atomic_load_n(
      &g_evidence.last_vptr[kFinalWriterHook], __ATOMIC_ACQUIRE);
  if (begin_vptr !=
          g_control.game_base + g_build_profile.physics_context_vtable_rva ||
      interval_vptr != g_control.game_base +
                           g_build_profile.physics_implementation_vtable_rva ||
      !ObjectIdentity(begin, begin_vptr) ||
      !ObjectIdentity(interval, interval_vptr) ||
      !ObjectIdentity(player, player_vptr) ||
      !ObjectIdentity(g_control.expected_main_object,
                      g_control.expected_main_object_vptr) ||
      interval > UINTPTR_MAX - kAdjustedSetterObjectOffset)
    return false;

  const std::uintptr_t setter = interval + kAdjustedSetterObjectOffset;
  const std::uintptr_t setter_vptr =
      g_control.game_base + g_build_profile.adjusted_setter_vtable_rva;
  if (!ObjectIdentity(setter, setter_vptr) ||
      interval > UINTPTR_MAX - kNitroServiceOffset)
    return false;
  std::uintptr_t nitro_service = 0;
  if (!ReadPrivateValue(interval + kNitroServiceOffset, &nitro_service,
                        sizeof(nitro_service)) ||
      !ObjectIdentity(nitro_service,
                      g_control.game_base +
                          g_build_profile.nitro_service_vtable_rva) ||
      nitro_service > UINTPTR_MAX - 8u)
    return false;

  std::uintptr_t interface = 0, interface_vptr = 0;
  std::int64_t interface_adjustment = 0;
  if (player > UINTPTR_MAX - kBackendInterfaceOffset ||
      !ReadPrivateValue(player + kBackendInterfaceOffset, &interface,
                        sizeof(interface)) ||
      !ReadPrivateValue(interface, &interface_vptr, sizeof(interface_vptr)) ||
      interface_vptr < 0x230u ||
      !ReadPrivateValue(interface_vptr - 0x230u, &interface_adjustment,
                        sizeof(interface_adjustment)))
    return false;
  std::uintptr_t backend_base = 0;
  if (!ApplyPointerAdjustment(interface, interface_adjustment,
                              &backend_base) ||
      backend_base > UINTPTR_MAX - 0xa0u)
    return false;
  std::uintptr_t delegate_interface = 0, delegate_vptr = 0;
  std::int64_t delegate_adjustment = 0;
  if (!ReadPrivateValue(backend_base + 0xa0u, &delegate_interface,
                        sizeof(delegate_interface)) ||
      !ReadPrivateValue(delegate_interface, &delegate_vptr,
                        sizeof(delegate_vptr)) ||
      delegate_vptr < 0x230u ||
      !ReadPrivateValue(delegate_vptr - 0x230u, &delegate_adjustment,
                        sizeof(delegate_adjustment)))
    return false;
  std::uintptr_t delegate = 0;
  if (!ApplyPointerAdjustment(delegate_interface, delegate_adjustment,
                              &delegate) ||
      !ReadPrivateValue(delegate, &delegate_vptr, sizeof(delegate_vptr)) ||
      delegate_vptr < 0x68u)
    return false;
  std::int64_t linear_adjustment = 0, angular_adjustment = 0;
  if (!ReadPrivateValue(delegate_vptr - 0x60u, &linear_adjustment,
                        sizeof(linear_adjustment)) ||
      !ReadPrivateValue(delegate_vptr - 0x68u, &angular_adjustment,
                        sizeof(angular_adjustment)))
    return false;
  std::uintptr_t linear_source = 0, angular_source = 0;
  if (!ApplyPointerAdjustment(delegate, linear_adjustment, &linear_source) ||
      !ApplyPointerAdjustment(delegate, angular_adjustment, &angular_source) ||
      linear_source == 0 || linear_source != angular_source)
    return false;
  std::uintptr_t source_vptr = 0;
  if (!ReadPrivateValue(linear_source, &source_vptr, sizeof(source_vptr)) ||
      (source_vptr != g_control.game_base +
                          g_build_profile.vehicle_source_vtable_rva &&
       source_vptr != g_control.game_base +
                          g_build_profile.car_physics_body_source_vtable_rva) ||
      linear_source > UINTPTR_MAX - kBarrelBackendOffset)
    return false;
  std::uintptr_t velocity_interface = 0, native_body = 0, native_vptr = 0;
  if (!ReadPrivateValue(linear_source + kBarrelBackendOffset,
                        &velocity_interface, sizeof(velocity_interface)) ||
      velocity_interface > UINTPTR_MAX - kNativeBodySlotOffset ||
      !ReadPrivateValue(velocity_interface + kNativeBodySlotOffset,
                        &native_body, sizeof(native_body)) ||
      !ReadPrivateValue(native_body, &native_vptr, sizeof(native_vptr)) ||
      native_vptr != g_control.game_base +
                         g_build_profile.native_physics_body_vtable_rva)
    return false;

  g_control.expected_begin_owner = begin;
  g_control.expected_begin_owner_vptr = begin_vptr;
  g_control.expected_interval_owner = interval;
  g_control.expected_interval_owner_vptr = interval_vptr;
  g_control.expected_player = player;
  g_control.expected_player_vptr = player_vptr;
  g_control.lifecycle_state_address =
      reinterpret_cast<std::uintptr_t>(lifecycle_object) + 0x2d8u;
  g_control.expected_nitro_state = nitro_service + 8u;
  g_control.expected_setter_object = setter;
  g_control.expected_setter_vptr = setter_vptr;
  g_control.expected_backend_interface = interface;
  g_control.expected_physics_velocity_interface = velocity_interface;
  g_control.expected_native_body = native_body;
  g_control.expected_barrel_owner = linear_source;
  g_control.expected_barrel_owner_vptr = source_vptr;
  return true;
}

bool ActivatePendingAtLifecycle(void* lifecycle_object) {
  const std::uint32_t pending = __atomic_load_n(
      &g_control.pending_mode, __ATOMIC_ACQUIRE);
  if (pending != static_cast<std::uint32_t>(RunMode::kRecord) &&
      pending != static_cast<std::uint32_t>(RunMode::kReplay))
    return false;
  const bool replay = pending == static_cast<std::uint32_t>(RunMode::kReplay);
  const bool active_retry = __atomic_load_n(
      &g_control.pending_reserved, __ATOMIC_ACQUIRE) == 1u;
  // A direct-Retry request is deliberately queued while the current attempt
  // is still recording.  The lifecycle hook can observe an earlier 2 -> 3
  // transition before the old attempt reaches its authoritative closed
  // boundary.  That is a normal "not yet" event, not a rejected activation:
  // keep pending_state=1 so the next lifecycle entry can activate the already
  // archived attempt.  Marking it rejected here permanently loses the next
  // race even though the old attempt subsequently completes without error.
  const bool source_attempt_still_active = active_retry && !replay &&
      g_control.enabled == 1 && g_control.completed == 0 &&
      g_control.mode == static_cast<std::uint32_t>(RunMode::kRecord) &&
      g_control.completion_policy == static_cast<std::uint32_t>(
          CompletionPolicy::kRaceLifecycle) &&
      g_evidence.status == kArmed && g_evidence.first_error == 0 &&
      g_control.generation != 0 && g_control.generation != UINT32_MAX &&
      g_control.pending_generation == g_control.generation + 1u &&
      __atomic_load_n(&g_control.pending_state, __ATOMIC_ACQUIRE) == 1u;
  if (source_attempt_still_active) return false;

  if (!BindObservedRaceObjects(lifecycle_object)) {
    __atomic_store_n(&g_control.pending_state, 3u, __ATOMIC_RELEASE);
    return false;
  }
  if (active_retry) {
    if (replay || g_control.enabled != 0 || g_control.completed != 1 ||
        g_evidence.status != kComplete || g_evidence.first_error != 0 ||
        g_control.generation == 0 || g_control.generation == UINT32_MAX ||
        g_control.pending_generation != g_control.generation + 1u ||
        g_runtime.receipt_count == 0 ||
        (g_evidence.interval_records == 0 &&
         !AdapterConfig().allow_zero_integration_updates)) {
      __atomic_store_n(&g_control.pending_state, 3u, __ATOMIC_RELEASE);
      return false;
    }
    const std::uint32_t prior_generation = g_control.generation;
    g_control.archived_generation = prior_generation;
    g_control.archived_frame_count = g_runtime.receipt_count;
    g_control.archived_interval_count = static_cast<std::uint32_t>(
        g_evidence.interval_records);
    g_control.generation = g_control.pending_generation;
    g_control.session_id ^= 0x9e3779b97f4a7c15ULL ^
        static_cast<std::uint64_t>(g_control.generation);
    if (g_control.session_id == 0) g_control.session_id = 1;
    g_control.mode = static_cast<std::uint32_t>(RunMode::kRecord);
    g_control.completion_policy = static_cast<std::uint32_t>(
        CompletionPolicy::kRaceLifecycle);
    g_control.replay_frame_count = 0;
    g_control.replay_interval_count = 0;
    g_control.replay_speed_factor = 1;
    g_control.replay_speed_reserved = 0;
    std::memset(g_control.recording_sha256, 0,
                sizeof(g_control.recording_sha256));
  } else if (g_control.pending_reserved != 0) {
    __atomic_store_n(&g_control.pending_state, 3u, __ATOMIC_RELEASE);
    return false;
  }
  if (!RearmArchivedSession(replay, true)) {
    __atomic_store_n(&g_control.pending_state, 3u, __ATOMIC_RELEASE);
    return false;
  }
  if (active_retry && g_control.generation != UINT32_MAX) {
    // Direct-Retry recording is a resident loop, not a one-shot arm.  Publish
    // the following generation before returning to the new race so repeated
    // failed laps never need Java to win a countdown race.
    __atomic_store_n(&g_control.pending_generation,
                     g_control.generation + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.pending_mode,
                     static_cast<std::uint32_t>(RunMode::kRecord),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.pending_reserved, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.pending_state, 1u, __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_control.pending_mode, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.pending_generation, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.pending_reserved, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.pending_state, 0u, __ATOMIC_RELEASE);
  }
  __atomic_fetch_add(&g_evidence.pending_activations, 1ULL,
                     __ATOMIC_RELAXED);
  return true;
}

bool SetProtection(std::uintptr_t target, int protection) {
  const long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) return false;
  const std::size_t page_size = static_cast<std::size_t>(page_size_value);
  const std::uintptr_t page = target & ~(page_size - 1);
  return mprotect(reinterpret_cast<void*>(page), page_size, protection) == 0;
}

bool PublishHook(std::uint32_t index) {
  if (index >= kHookCount) return false;
  auto* target = reinterpret_cast<std::uint8_t*>(g_control.target_entry[index]);
  const std::size_t size = HookPatchSize(index);
  std::uint8_t patch[kPatchSize]{};
  if (!BuildHookPatch(index, patch)) return false;
  if (!SetProtection(reinterpret_cast<std::uintptr_t>(target),
                     PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(target), target, size))
    return false;
  std::memcpy(target, patch, size);
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + size));
  const bool bytes = std::memcmp(target, patch, size) == 0;
  const bool logical = SetProtection(reinterpret_cast<std::uintptr_t>(target),
                                     kLogicalCodeProtection) &&
                       PrivateGuestCode(QueryMapping(target), target,
                                         size);
  return bytes && logical;
}

bool RestoreHook(std::uint32_t index) {
  if (index >= kHookCount) return false;
  auto* target = reinterpret_cast<std::uint8_t*>(g_control.target_entry[index]);
  const std::size_t size = HookPatchSize(index);
  std::uint8_t patch[kPatchSize]{};
  if (!BuildHookPatch(index, patch)) return false;
  if (std::memcmp(target, kExpectedPrologues[index], size) == 0)
    return true;
  if (std::memcmp(target, patch, size) != 0) return false;
  if (!SetProtection(reinterpret_cast<std::uintptr_t>(target),
                     PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(target), target, size))
    return false;
  std::memcpy(target, kExpectedPrologues[index], size);
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + size));
  const bool bytes =
      std::memcmp(target, kExpectedPrologues[index], size) == 0;
  const bool logical = SetProtection(reinterpret_cast<std::uintptr_t>(target),
                                     kLogicalCodeProtection) &&
                       PrivateGuestCode(QueryMapping(target), target,
                                         size);
  return bytes && logical;
}

bool PublishLifecycleHook() {
  auto* target = reinterpret_cast<std::uint8_t*>(
      g_control.lifecycle_target_entry);
  std::uint8_t patch[kPatchSize]{};
  if (!BuildLifecycleHookPatch(patch) ||
      !SetProtection(g_control.lifecycle_target_entry,
                     PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(target), target, kPatchSize))
    return false;
  std::memcpy(target, patch, kPatchSize);
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + kPatchSize));
  const bool bytes = std::memcmp(target, patch, kPatchSize) == 0;
  const bool logical = SetProtection(g_control.lifecycle_target_entry,
                                     kLogicalCodeProtection) &&
                       PrivateGuestCode(QueryMapping(target), target,
                                        kPatchSize);
  return bytes && logical;
}

bool RestoreLifecycleHook() {
  auto* target = reinterpret_cast<std::uint8_t*>(
      g_control.lifecycle_target_entry);
  std::uint8_t patch[kPatchSize]{};
  if (!BuildLifecycleHookPatch(patch)) return false;
  if (std::memcmp(target, kExpectedLifecyclePrologue, kPatchSize) == 0)
    return true;
  if (std::memcmp(target, patch, kPatchSize) != 0 ||
      !SetProtection(g_control.lifecycle_target_entry,
                     PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(target), target, kPatchSize))
    return false;
  std::memcpy(target, kExpectedLifecyclePrologue, kPatchSize);
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + kPatchSize));
  const bool bytes =
      std::memcmp(target, kExpectedLifecyclePrologue, kPatchSize) == 0;
  const bool logical = SetProtection(g_control.lifecycle_target_entry,
                                     kLogicalCodeProtection) &&
                       PrivateGuestCode(QueryMapping(target), target,
                                        kPatchSize);
  return bytes && logical;
}

bool LifecycleHookPublishedValid() {
  const auto* target = reinterpret_cast<const std::uint8_t*>(
      g_control.lifecycle_target_entry);
  std::uint8_t patch[kPatchSize]{};
  return BuildLifecycleHookPatch(patch) &&
         PrivateGuestCode(QueryMapping(target), target, kPatchSize) &&
         std::memcmp(target, patch, kPatchSize) == 0;
}

std::uintptr_t RandomHookRva(std::uint32_t index) {
  if (index == kBarrelRandomBoolHook)
    return g_build_profile.barrel_random_bool_rva;
  if (index == kBarrelRandomLerpHook)
    return g_build_profile.barrel_random_lerp_rva;
  return 0;
}

bool PublishRandomHook(std::uint32_t index) {
  if (index >= kRandomHookCount) return false;
  auto* target = reinterpret_cast<std::uint8_t*>(g_random_hooks[index].target);
  std::uint8_t patch[kRandomPatchSize]{};
  if (!BuildRandomHookPatch(index, patch) ||
      !SetProtection(g_random_hooks[index].target, PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(target), target, kRandomPatchSize))
    return false;
  std::memcpy(target, patch, kRandomPatchSize);
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + kRandomPatchSize));
  const bool bytes = std::memcmp(target, patch, kRandomPatchSize) == 0;
  const bool logical = SetProtection(g_random_hooks[index].target,
                                     kLogicalCodeProtection) &&
                       PrivateGuestCode(QueryMapping(target), target,
                                        kRandomPatchSize);
  return bytes && logical;
}

bool RestoreRandomHook(std::uint32_t index) {
  if (index >= kRandomHookCount) return false;
  auto* target = reinterpret_cast<std::uint8_t*>(g_random_hooks[index].target);
  std::uint8_t patch[kRandomPatchSize]{};
  if (!g_random_hooks[index].original_captured ||
      !RandomOriginalIdentityValid(index, g_random_hooks[index].original) ||
      !BuildRandomHookPatch(index, patch))
    return false;
  if (std::memcmp(target, g_random_hooks[index].original,
                  kRandomPatchSize) == 0)
    return true;
  if (std::memcmp(target, patch, kRandomPatchSize) != 0) return false;
  if (!SetProtection(g_random_hooks[index].target, PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(target), target, kRandomPatchSize))
    return false;
  std::memcpy(target, g_random_hooks[index].original, kRandomPatchSize);
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + kRandomPatchSize));
  const bool bytes = std::memcmp(target, g_random_hooks[index].original,
                                 kRandomPatchSize) == 0;
  const bool logical = SetProtection(g_random_hooks[index].target,
                                     kLogicalCodeProtection) &&
                       PrivateGuestCode(QueryMapping(target), target,
                                        kRandomPatchSize);
  return bytes && logical;
}

bool RandomHooksPublishedValid() {
  for (std::uint32_t index = 0; index < kRandomHookCount; ++index) {
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_random_hooks[index].target);
    std::uint8_t patch[kRandomPatchSize]{};
    if (!BuildRandomHookPatch(index, patch) ||
        !PrivateGuestCode(QueryMapping(target), target, kRandomPatchSize) ||
        std::memcmp(target, patch, kRandomPatchSize) != 0)
      return false;
  }
  return true;
}

bool RandomHooksRestoredValid() {
  for (std::uint32_t index = 0; index < kRandomHookCount; ++index) {
    const std::uintptr_t expected = g_control.game_base + RandomHookRva(index);
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_random_hooks[index].target);
    if (g_random_hooks[index].target != expected ||
        g_random_hooks[index].wrapper != kRandomWrappers[index] ||
        !g_random_hooks[index].original_captured ||
        !RandomOriginalIdentityValid(index, g_random_hooks[index].original) ||
        !PrivateGuestCode(QueryMapping(target), target, kRandomPatchSize) ||
        std::memcmp(target, g_random_hooks[index].original,
                    kRandomPatchSize) != 0)
      return false;
  }
  return true;
}

bool PublishSetterSlot(std::uint32_t index) {
  if (index >= kInstalledSetterCount) return false;
  auto* slot = reinterpret_cast<std::uintptr_t*>(
      g_control.setter_slot[index]);
  if (!PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot)) ||
      __atomic_load_n(slot, __ATOMIC_ACQUIRE) !=
          g_control.setter_original[index])
    return false;
  if (!SetProtection(reinterpret_cast<std::uintptr_t>(slot),
                     PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(slot), slot, sizeof(*slot)))
    return false;
  __atomic_store_n(slot, g_control.setter_wrapper[index], __ATOMIC_RELEASE);
  const bool written = __atomic_load_n(slot, __ATOMIC_ACQUIRE) ==
                       g_control.setter_wrapper[index];
  const bool readonly =
      SetProtection(reinterpret_cast<std::uintptr_t>(slot), PROT_READ) &&
      PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot));
  return written && readonly;
}

bool RestoreSetterSlot(std::uint32_t index) {
  if (index >= kInstalledSetterCount) return false;
  auto* slot = reinterpret_cast<std::uintptr_t*>(
      g_control.setter_slot[index]);
  const std::uintptr_t current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (current == g_control.setter_original[index])
    return PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot));
  if (current != g_control.setter_wrapper[index] ||
      !PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot)))
    return false;
  if (!SetProtection(reinterpret_cast<std::uintptr_t>(slot),
                     PROT_READ | PROT_WRITE) ||
      !PrivateRw(QueryMapping(slot), slot, sizeof(*slot)))
    return false;
  __atomic_store_n(slot, g_control.setter_original[index], __ATOMIC_RELEASE);
  const bool restored = __atomic_load_n(slot, __ATOMIC_ACQUIRE) ==
                        g_control.setter_original[index];
  const bool readonly =
      SetProtection(reinterpret_cast<std::uintptr_t>(slot), PROT_READ) &&
      PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot));
  return restored && readonly;
}

bool PublishedTargetsValid() {
  for (std::uint32_t index = 0; index < kHookCount; ++index) {
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_control.target_entry[index]);
    std::uint8_t patch[kPatchSize]{};
    if (!BuildHookPatch(index, patch) ||
        !PrivateGuestCode(QueryMapping(target), target, HookPatchSize(index)) ||
        std::memcmp(target, patch, HookPatchSize(index)) != 0)
      return false;
  }
  if (!LifecycleHookPublishedValid() || !RandomHooksPublishedValid())
    return false;
  for (std::uint32_t index = 0; index < kInstalledSetterCount; ++index) {
    const auto* slot = reinterpret_cast<const std::uintptr_t*>(
        g_control.setter_slot[index]);
    const auto* original = reinterpret_cast<const std::uint8_t*>(
        g_control.setter_original[index]);
    if (!PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot)) ||
        __atomic_load_n(slot, __ATOMIC_ACQUIRE) !=
            g_control.setter_wrapper[index] ||
        !PrivateGuestCode(QueryMapping(original), original,
                          sizeof(kExpectedSetterBodies[index])) ||
        std::memcmp(original, kExpectedSetterBodies[index],
                    sizeof(kExpectedSetterBodies[index])) != 0)
      return false;
  }
  return true;
}

bool RestoredTargetsValid() {
  if (!build_profile::Valid(g_build_profile) || g_control.game_base == 0)
    return false;
  for (std::uint32_t index = 0; index < kHookCount; ++index) {
    const std::uintptr_t expected_target =
        g_control.game_base + g_build_profile.hook_rvas[index];
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_control.target_entry[index]);
    if (g_control.target_entry[index] != expected_target ||
        g_control.target_tail[index] !=
            expected_target + HookPatchSize(index) ||
        g_control.wrapper[index] != kWrappers[index] ||
        !PrivateGuestCode(QueryMapping(target), target,
                          HookPatchSize(index)) ||
        std::memcmp(target, kExpectedPrologues[index],
                    HookPatchSize(index)) != 0)
      return false;
  }
  const std::uintptr_t lifecycle_target =
      g_control.game_base + g_build_profile.lifecycle_shared_enter_rva;
  const auto* lifecycle = reinterpret_cast<const std::uint8_t*>(
      g_control.lifecycle_target_entry);
  if (g_control.lifecycle_target_entry != lifecycle_target ||
      g_control.lifecycle_target_tail != lifecycle_target + kPatchSize ||
      g_control.lifecycle_wrapper != kLifecycleWrapper ||
      !PrivateGuestCode(QueryMapping(lifecycle), lifecycle, kPatchSize) ||
      std::memcmp(lifecycle, kExpectedLifecyclePrologue, kPatchSize) != 0)
    return false;
  if (!RandomHooksRestoredValid()) return false;
  for (std::uint32_t index = 0; index < kInstalledSetterCount; ++index) {
    const std::uintptr_t expected_slot =
        g_control.game_base + g_build_profile.adjusted_setter_vtable_rva +
        kSetterSlotOffsets[index];
    const std::uintptr_t expected_original =
        g_control.game_base + g_build_profile.setter_original_rvas[index];
    const auto* slot = reinterpret_cast<const std::uintptr_t*>(
        g_control.setter_slot[index]);
    if (g_control.setter_slot[index] != expected_slot ||
        g_control.setter_original[index] != expected_original ||
        g_control.setter_wrapper[index] != kSetterWrappers[index] ||
        !PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot)) ||
        __atomic_load_n(slot, __ATOMIC_ACQUIRE) != expected_original)
      return false;
  }
  for (std::uint32_t index = kInstalledSetterCount; index < kSetterCount;
       ++index)
    if (g_control.setter_slot[index] != 0 ||
        g_control.setter_original[index] != 0 ||
        g_control.setter_wrapper[index] != 0)
      return false;
  return true;
}

bool RestoredSessionReusable() {
  constexpr std::uint64_t kRequiredRestoreFlags =
      kAllRestored | kAllSetterSlotsRestored;
  return g_control.enabled == 0 && g_control.completed == 1 &&
         g_control.active_helpers == 0 &&
         g_evidence.status == kRestored && g_evidence.first_error == 0 &&
         (g_evidence.flags & kRequiredRestoreFlags) == kRequiredRestoreFlags &&
         g_evidence.install_calls == 1 && g_evidence.restore_calls == 1 &&
         g_evidence.core_faults == 0 &&
         !g_runtime.input_action.tick_open &&
         g_runtime.tick.coordinator.tick_phase ==
             coordinator::TickPhase::kClosed &&
         g_runtime.tick.coordinator.failures == 0 &&
         RestoredTargetsValid();
}

void ResetRestoredSessionMetadata() {
  // The large frame/interval arrays may retain old bytes, but their freshly
  // reset counts make them unreachable.  The next record/replay overwrites
  // every published element.  Avoid a multi-megabyte reset on the stopped
  // game thread merely to start another race.
  Initialize();
  std::memset(&g_runtime, 0, sizeof(g_runtime));
  g_recorded_interval_count = 0;
  g_record_physics_snapshot = {};
  g_barrel_capture = {};
  g_barrel_capture_tick = UINT64_MAX;
  g_tick_semantic_start = {};
  g_g4_interval_tail_v1 = 0;
  g_g4_final_tail_v1 = 0;
  g_g4_frame_tail_v1 = 0;
  g_g4_nitro_tail_v1 = 0;
  g_g4_submit_tail_v1 = 0;
  g_g4_barrel_roll_tail_v1 = 0;
  g_g4_barrel_yaw_tail_v1 = 0;
  g_g4_dispatch_tail_v1 = 0;
  g_g4_lifecycle_tail_v1 = 0;
  std::memset(g_random_hooks, 0, sizeof(g_random_hooks));
  g_barrel_prng = {};
  g_barrel_prng_lock.clear(std::memory_order_release);
  g_runtime_lock.clear(std::memory_order_release);
}

bool InstallPassive(const GameImage& image) {
  std::int32_t expected = 0;
  if (!g_install_state.compare_exchange_strong(expected, -2)) {
    expected = 2;
    if (!g_install_state.compare_exchange_strong(expected, -2) ||
        !RestoredSessionReusable()) {
      Fault(kErrorAlreadyInstalled);
      g_install_state.store(-1, std::memory_order_release);
      return false;
    }
    ResetRestoredSessionMetadata();
  }
  ResetEvidence();
  ++g_evidence.install_calls;
  if (!PassiveControlFresh() || !build_profile::Valid(g_build_profile)) {
    Fault(kErrorControl);
    g_install_state.store(-1);
    return false;
  }
  if (!image.found || image.base == 0) {
    Fault(kErrorGameIdentity);
    g_install_state.store(-1);
    return false;
  }
  g_evidence.flags |= kGameIdentity;
  g_control.game_base = image.base;
  for (std::uint32_t index = 0; index < kHookCount; ++index) {
    g_control.target_entry[index] =
        image.base + g_build_profile.hook_rvas[index];
    g_control.target_tail[index] =
        g_control.target_entry[index] + HookPatchSize(index);
    g_control.wrapper[index] = kWrappers[index];
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_control.target_entry[index]);
    const std::size_t size = HookPatchSize(index);
    if (!TargetInExecutableLoad(image, g_control.target_entry[index], size) ||
        !PrivateGuestCode(QueryMapping(target), target, size)) {
      Fault(kErrorTargetRange);
      g_install_state.store(-1);
      return false;
    }
    if (std::memcmp(target, kExpectedPrologues[index], size) != 0) {
      Fault(kErrorPrologue);
      g_install_state.store(-1);
      return false;
    }
    // Encode every patch before publishing the first one, so a bad wrapper
    // identity leaves all game entries untouched.
    std::uint8_t patch[kPatchSize]{};
    if (!BuildHookPatch(index, patch)) {
      Fault(kErrorTargetRange);
      g_install_state.store(-1);
      return false;
    }
  }
  g_control.lifecycle_target_entry =
      image.base + g_build_profile.lifecycle_shared_enter_rva;
  g_control.lifecycle_target_tail =
      g_control.lifecycle_target_entry + kPatchSize;
  g_control.lifecycle_wrapper = kLifecycleWrapper;
  const auto* lifecycle_target = reinterpret_cast<const std::uint8_t*>(
      g_control.lifecycle_target_entry);
  std::uint8_t lifecycle_patch[kPatchSize]{};
  if (!TargetInExecutableLoad(image, g_control.lifecycle_target_entry,
                              kPatchSize) ||
      !PrivateGuestCode(QueryMapping(lifecycle_target), lifecycle_target,
                        kPatchSize) ||
      std::memcmp(lifecycle_target, kExpectedLifecyclePrologue,
                  kPatchSize) != 0 ||
      !BuildLifecycleHookPatch(lifecycle_patch)) {
    Fault(kErrorPrologue);
    g_install_state.store(-1);
    return false;
  }
  for (std::uint32_t index = 0; index < kRandomHookCount; ++index) {
    g_random_hooks[index].target = image.base + RandomHookRva(index);
    g_random_hooks[index].wrapper = kRandomWrappers[index];
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_random_hooks[index].target);
    std::uint8_t patch[kRandomPatchSize]{};
    if (!TargetInExecutableLoad(image, g_random_hooks[index].target,
                                kRandomPatchSize) ||
        !PrivateGuestCode(QueryMapping(target), target, kRandomPatchSize) ||
        !RandomOriginalIdentityValid(index, target) ||
        !BuildRandomHookPatch(index, patch)) {
      Fault(kErrorBarrelRandom);
      g_install_state.store(-1);
      return false;
    }
    std::memcpy(g_random_hooks[index].original, target, kRandomPatchSize);
    g_random_hooks[index].original_captured = true;
  }
  for (std::uint32_t index = 0; index < kInstalledSetterCount; ++index) {
    g_control.setter_slot[index] =
        image.base + g_build_profile.adjusted_setter_vtable_rva +
        kSetterSlotOffsets[index];
    g_control.setter_original[index] =
        image.base + g_build_profile.setter_original_rvas[index];
    g_control.setter_wrapper[index] = kSetterWrappers[index];
    const auto* slot = reinterpret_cast<const std::uintptr_t*>(
        g_control.setter_slot[index]);
    const auto* original = reinterpret_cast<const std::uint8_t*>(
        g_control.setter_original[index]);
    if (!PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot)) ||
        __atomic_load_n(slot, __ATOMIC_ACQUIRE) !=
            g_control.setter_original[index] ||
        !TargetInExecutableLoad(image, g_control.setter_original[index],
                                sizeof(kExpectedSetterBodies[index])) ||
        !PrivateGuestCode(QueryMapping(original), original,
                          sizeof(kExpectedSetterBodies[index])) ||
        std::memcmp(original, kExpectedSetterBodies[index],
                    sizeof(kExpectedSetterBodies[index])) != 0) {
      Fault(kErrorSetterIdentity);
      g_install_state.store(-1);
      return false;
    }
  }
  g_evidence.flags |= kAllPrologues;
  g_g4_interval_tail_v1 = g_control.target_tail[kTickHook];
  g_g4_final_tail_v1 = g_control.target_tail[kFinalWriterHook];
  g_g4_frame_tail_v1 = g_control.target_tail[kFrameEventHook];
  g_g4_nitro_tail_v1 = g_control.target_tail[kNitroHook];
  g_g4_submit_tail_v1 = g_control.target_tail[kSubmitHook];
  g_g4_barrel_roll_tail_v1 = g_control.target_tail[kBarrelRollHook];
  g_g4_barrel_yaw_tail_v1 = g_control.target_tail[kBarrelYawHook];
  g_g4_dispatch_tail_v1 = g_control.target_tail[kLogicDispatcherHook];
  g_g4_lifecycle_tail_v1 = g_control.lifecycle_target_tail;
  // Passive installation validates all ordinary and deterministic-random
  // targets but changes no game code. Hooks are published together at arm.
  g_evidence.flags |= kPayloadPreloaded;
  g_install_state.store(1, std::memory_order_release);
  __atomic_store_n(&g_evidence.status, kPassiveInstalled, __ATOMIC_RELEASE);
  return true;
}

void ResetForArm() {
  std::uint64_t raw_entries[kHookCount]{};
  std::uint64_t raw_setter_entries[kSetterCount]{};
  std::memcpy(raw_entries, g_evidence.wrapper_entries, sizeof(raw_entries));
  std::memcpy(raw_setter_entries, g_evidence.setter_entries,
              sizeof(raw_setter_entries));
  const std::uint64_t install_calls = g_evidence.install_calls;
  const std::uint64_t flags = g_evidence.flags;
  ResetEvidence();
  std::memcpy(g_evidence.wrapper_entries, raw_entries, sizeof(raw_entries));
  std::memcpy(g_evidence.setter_entries, raw_setter_entries,
              sizeof(raw_setter_entries));
  g_evidence.install_calls = install_calls;
  g_evidence.flags = flags;
}

void ResetForArchivedRearm() {
  const std::uint64_t install_calls = g_evidence.install_calls;
  const std::uint64_t flags = g_evidence.flags;
  const std::uint64_t lifecycle_entries = g_evidence.lifecycle_entries;
  const std::uint64_t lifecycle_qualified = g_evidence.lifecycle_qualified;
  const std::uint64_t pending_activations = g_evidence.pending_activations;
  ResetEvidence();
  g_evidence.install_calls = install_calls;
  g_evidence.flags = flags;
  g_evidence.lifecycle_entries = lifecycle_entries;
  g_evidence.lifecycle_qualified = lifecycle_qualified;
  g_evidence.pending_activations = pending_activations;
}

bool SealPausedRecord() {
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kRecord) ||
      g_control.completion_policy !=
          static_cast<std::uint32_t>(CompletionPolicy::kRaceLifecycle) ||
      g_control.enabled != 1 || g_control.completed != 0 ||
      g_control.active_helpers != 0 || g_runtime.tick.complete != 0 ||
      g_runtime.tick.coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      g_runtime.tick.coordinator.failures != 0 ||
      g_runtime.receipt_count == 0 ||
      g_runtime.receipt_count != g_runtime.tick.coordinator.tick ||
      g_evidence.first_error != 0 || g_evidence.status != kArmed) {
    Fault(kErrorControl);
    return false;
  }
  auto* lifecycle = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  if (!PrivateRw(QueryMapping(lifecycle), lifecycle, sizeof(*lifecycle))) {
    Fault(kErrorLifecycleIdentity);
    return false;
  }
  const std::uint32_t observed_lifecycle =
      __atomic_load_n(lifecycle, __ATOMIC_RELAXED);
  g_evidence.last_lifecycle_state = observed_lifecycle;

  // Pause observation is host-side and can arrive between Submit/Interval and
  // FinalWriter/FrameEnd.  That open tail has never produced a recording
  // packet, so it must be removed atomically while the controller has all game
  // threads stopped.  This is also the Retry race resolver: after discarding
  // the partial tail, lifecycle 3 seals a checkpoint while any non-3 state
  // closes the attempt as a race-lifecycle completion.
  if (g_runtime.input_action.tick_open) {
    const std::uint64_t open_tick = g_runtime.input_action.tick;
    const bridge::Result discarded =
        bridge::DiscardOpenTickForRaceEnd(AdapterConfig(), &g_runtime);
    g_evidence.last_g3_result = g_runtime.last_g3_result;
    g_evidence.last_g4_result = g_runtime.last_g4_result;
    if (discarded != bridge::Result::kObserved ||
        !RestoreDiscardedTickSemantics(open_tick)) {
      ++g_evidence.core_faults;
      Fault(kErrorAdapter);
      return false;
    }
  }

  if (g_runtime.input_action.tick_open ||
      g_runtime.tick.coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      g_runtime.receipt_count != g_runtime.tick.coordinator.tick ||
      g_runtime.receipt_count != g_runtime.tick.coordinator.ticks_published ||
      g_runtime.receipt_count != g_runtime.tick.coordinator.ticks_begun ||
      g_runtime.receipt_count != g_evidence.recorded_frames ||
      (g_evidence.interval_records == 0 &&
       !AdapterConfig().allow_zero_integration_updates)) {
    Fault(kErrorControl);
    return false;
  }

  if (observed_lifecycle == 3u) {
    PublishCompletion(CompletionReason::kManualCheckpoint);
    return true;
  }
  const bridge::Result ended = bridge::ObserveRaceEndAtClosedBoundary(
      AdapterConfig(), &g_runtime, observed_lifecycle);
  g_evidence.last_g3_result = g_runtime.last_g3_result;
  g_evidence.last_g4_result = g_runtime.last_g4_result;
  if (ended != bridge::Result::kRaceEnded) {
    ++g_evidence.core_faults;
    Fault(kErrorAdapter);
    return false;
  }
  PublishCompletion(CompletionReason::kRaceLifecycle);
  return true;
}

bool PausedReplayReceiptValid() {
  return g_install_state.load(std::memory_order_acquire) == 1 &&
      g_control.archived_generation != 0 &&
      g_control.archived_frame_count != 0 &&
      g_runtime.input_action.tick_open == false &&
      g_runtime.tick.complete == 1 &&
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      g_runtime.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      g_runtime.tick.coordinator.generation == g_control.archived_generation &&
      g_runtime.tick.coordinator.tick == g_control.archived_frame_count &&
      g_runtime.receipt_count == g_control.archived_frame_count &&
      (g_evidence.status == kComplete || g_evidence.status == kQueued) &&
      g_evidence.first_error == 0 &&
      g_evidence.completion_reason ==
          static_cast<std::uint32_t>(CompletionReason::kFixedFrameLimit) &&
      g_evidence.published_ticks == g_control.archived_frame_count &&
      g_evidence.recorded_frames == 0 && g_evidence.interval_records == 0;
}

bool ActiveReplayReceiptValidForBranch() {
  return g_install_state.load(std::memory_order_acquire) == 1 &&
      g_control.archived_generation != 0 &&
      g_control.archived_frame_count != 0 &&
      g_control.archived_frame_count < g_control.frame_limit &&
      !g_runtime.input_action.tick_open && g_runtime.tick.complete == 0 &&
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      g_runtime.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      g_runtime.tick.coordinator.generation == g_control.archived_generation &&
      g_runtime.tick.coordinator.tick == g_control.archived_frame_count &&
      g_runtime.receipt_count == g_control.archived_frame_count &&
      g_evidence.status == kArmed && g_evidence.first_error == 0 &&
      g_evidence.completion_reason ==
          static_cast<std::uint32_t>(CompletionReason::kNone) &&
      g_evidence.published_ticks == g_control.archived_frame_count &&
      g_evidence.recorded_frames == 0 && g_evidence.interval_records == 0;
}

bool StagedReplayIntervalsValid(std::uint32_t prefix_count) {
  return g_recorded_interval_count <= kMaximumIntervalSamples &&
      g4::IntervalSequenceValid(
          {g_recorded_intervals, g_recorded_interval_count}, prefix_count,
          AdapterConfig().allow_zero_integration_updates);
}

bool StagedReplayPrefixValid(std::uint32_t prefix_count) {
  if (prefix_count == 0 || prefix_count > kMaximumFrames ||
      !StagedReplayIntervalsValid(prefix_count))
    return false;
  for (std::uint32_t index = 0; index < prefix_count; ++index) {
    const auto& source = g_replay_frames[index];
    if (!g4::PhysicsRecordingFrameValid(source) || source.tick != index ||
        source.monotonic_ns !=
            static_cast<std::uint64_t>(index) * g_control.fixed_delta_us *
                1000ULL)
      return false;
  }
  return true;
}

void SeedValidatedContinuousRecordingPrefix(std::uint32_t prefix_count) {
  std::uint64_t natural_nitro_calls = 0;
  for (std::uint32_t index = 0; index < prefix_count; ++index)
    natural_nitro_calls += g_replay_frames[index].nitro_activation_count;
  std::memcpy(g_recorded_frames, g_replay_frames,
              sizeof(g_recorded_frames[0]) * prefix_count);

  // Convert the already-observed replay prefix into the same accounting that
  // ordinary recording would have produced. The physical frames and interval
  // samples above remain the authoritative source; these counters only prove
  // that the continuous output owns every closed prefix tick.
  g_evidence.published_ticks = prefix_count;
  g_evidence.last_tick = prefix_count - 1u;
  g_evidence.completion_reason =
      static_cast<std::uint32_t>(CompletionReason::kNone);
  g_evidence.interval_records = g_recorded_interval_count;
  g_evidence.natural_nitro_calls = natural_nitro_calls;
  g_evidence.suppressed_nitro_calls = 0;
  g_evidence.injected_nitro_calls = 0;
  g_evidence.recorded_frames = prefix_count;
  std::memset(g_evidence.setter_overrides, 0,
              sizeof(g_evidence.setter_overrides));
  g_evidence.physics_equal_frames = 0;
  g_evidence.physics_corrected_frames = 0;
  g_evidence.physics_correction_writes = 0;
  g_evidence.physics_skipped_frames = 0;
  g_evidence.barrel_rbx_overrides = 0;
  g_evidence.barrel_angular_overrides = 0;
  g_evidence.barrel_rbx_records =
      g_evidence.qualified_events[kBarrelRollHook];
  g_evidence.barrel_angular_records =
      g_evidence.qualified_events[kBarrelYawHook];
}

bool SeedContinuousRecordingPrefix(std::uint32_t prefix_count) {
  if (!StagedReplayPrefixValid(prefix_count)) return false;
  SeedValidatedContinuousRecordingPrefix(prefix_count);
  return true;
}

// The previous host-side handoff waited until the outer dispatcher returned.
// FrameAfter had already sealed replay Tick T by then, so game code between the
// two boundaries could advance the vehicle to T+2 before recording T+1.  Make
// the AluTasV2-style replay/record ownership change at the authoritative closed
// FrameAfter boundary itself.  A delayed ESC may now add honestly recorded
// suffix ticks, but it cannot omit a simulation transition from the stream.
bool CompleteReplayToAtomicRecording() {
  const std::uint32_t prefix_count = g_control.replay_frame_count;
  const std::uint32_t prior_generation = g_control.generation;
  const std::uint32_t next_generation = prior_generation + 1u;
  const std::uint32_t replay_interval_count =
      g_control.replay_interval_count;
  g_evidence.reserved[0] = 0xA7000001ULL;
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      g_control.enabled != 1 || g_control.completed != 0 ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kReplay) ||
       g_control.completion_policy !=
           static_cast<std::uint32_t>(CompletionPolicy::kFixedFrameLimit) ||
       g_control.replay_speed_reserved != kReplayCompletionAtomicRecord ||
       prior_generation == 0 || prior_generation == UINT32_MAX ||
       next_generation == 0 || g_control.pending_mode != 0u ||
       g_control.pending_state != 0u || g_control.pending_generation != 0u ||
       g_control.pending_reserved != 0u || prefix_count == 0 ||
      prefix_count >= kMaximumFrames ||
      g_runtime.input_action.tick_open || g_runtime.tick.complete != 1 ||
      g_runtime.tick.coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      g_runtime.tick.coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      g_runtime.tick.coordinator.generation != prior_generation ||
      g_runtime.tick.coordinator.tick != prefix_count ||
      g_runtime.receipt_count != prefix_count ||
      !StagedReplayPrefixValid(prefix_count))
    return false;

  const std::uint64_t next_session_id =
      (g_control.session_id ^ 0x9e3779b97f4a7c15ULL ^
       static_cast<std::uint64_t>(next_generation)) != 0
          ? g_control.session_id ^ 0x9e3779b97f4a7c15ULL ^
                static_cast<std::uint64_t>(next_generation)
          : 1ULL;
  bridge::ConfigV1 next_config = AdapterConfig();
  next_config.tick.session_id = next_session_id;
  next_config.tick.generation = next_generation;
  next_config.tick.frame_limit = kMaximumFrames;
  next_config.tick.replay_queue = {
      .mode = coordinator::ReplayMode::kInactive,
      .communication_version_matches = true,
      .block_mode_still_active = false,
      .packets = nullptr,
      .packet_count = 0,
  };
  next_config.interval_replay = {};
  g_evidence.reserved[0] = 0xA7000002ULL;
  const bridge::Result transitioned =
      bridge::ContinueReplayAsRecordAtClosedBoundary(
          next_config, g_replay_frames, prefix_count, &g_runtime);
  if (transitioned != bridge::Result::kObserved) {
    g_evidence.reserved[0] =
        0xA7001000ULL | static_cast<std::uint32_t>(transitioned);
    return false;
  }

  // Nothing below this point can reject the transition.  Publish the copied
  // prefix and the new mode only after the coordinator has accepted the exact
  // closed boundary, so observers can never see a half-converted session.
  SeedValidatedContinuousRecordingPrefix(prefix_count);
  g_control.archived_generation = prior_generation;
  g_control.archived_frame_count = prefix_count;
  g_control.archived_interval_count = replay_interval_count;
  g_control.generation = next_generation;
  g_control.session_id = next_session_id;
  g_control.frame_limit = kMaximumFrames;
  g_control.mode = static_cast<std::uint32_t>(RunMode::kRecord);
  g_control.completion_policy =
      static_cast<std::uint32_t>(CompletionPolicy::kRaceLifecycle);
  g_control.replay_frame_count = 0;
  g_control.replay_interval_count = 0;
  g_control.replay_speed_factor = 1;
  g_control.replay_speed_reserved = kReplayCompletionBarrierDisabled;
  std::memset(g_control.recording_sha256, 0,
              sizeof(g_control.recording_sha256));
  g_control.pending_mode = 0;
  g_control.pending_generation = 0;
  g_control.pending_reserved = 0;
  __atomic_store_n(&g_control.pending_state, 0u, __ATOMIC_RELEASE);

  g_tick_semantic_start = {};
  g_record_physics_snapshot = {};
  g_barrel_capture = {};
  g_barrel_capture_tick = UINT64_MAX;
  g_evidence.flags |= kReplayRecordHandoffComplete;
  g_evidence.completion_reason =
      static_cast<std::uint32_t>(CompletionReason::kNone);
  __atomic_store_n(&g_control.completed, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.status, kArmed, __ATOMIC_RELEASE);
  __atomic_store_n(&g_control.enabled, 1u, __ATOMIC_RELEASE);
  g_evidence.reserved[0] = 0xA70000FFULL;
  return true;
}

bool RearmPausedReplayRecord() {
  const bool completed_replay = PausedReplayReceiptValid();
  const bool active_replay = !completed_replay &&
      ActiveReplayReceiptValidForBranch();
  if ((!completed_replay && !active_replay) ||
      !ArmControlValid(true, active_replay) ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kRecord) ||
      g_control.completion_policy !=
          static_cast<std::uint32_t>(CompletionPolicy::kRaceLifecycle) ||
      !PublishedTargetsValid()) {
    Fault(kErrorControl);
    return false;
  }
  auto* lifecycle = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  if (!PrivateRw(QueryMapping(lifecycle), lifecycle, sizeof(*lifecycle)) ||
      __atomic_load_n(lifecycle, __ATOMIC_RELAXED) != 3u) {
    Fault(kErrorLifecycleIdentity);
    return false;
  }
  if (!ObjectIdentity(g_control.expected_begin_owner,
                      g_control.expected_begin_owner_vptr) ||
      !ObjectIdentity(g_control.expected_interval_owner,
                      g_control.expected_interval_owner_vptr) ||
      !ObjectIdentity(g_control.expected_barrel_owner,
                      g_control.expected_barrel_owner_vptr) ||
      !ObjectIdentity(g_control.expected_setter_object,
                      g_control.expected_setter_vptr) ||
      !ObjectIdentity(g_control.expected_player,
                      g_control.expected_player_vptr) ||
      !ObjectIdentity(g_control.expected_main_object,
                      g_control.expected_main_object_vptr) ||
      !NativePhysicsMappingsValid()) {
    Fault(kErrorObjectIdentity);
    return false;
  }

  const std::uint32_t prefix_count =
      static_cast<std::uint32_t>(g_control.archived_frame_count);
  if (!SeedContinuousRecordingPrefix(prefix_count)) {
    Fault(kErrorIntervalStream);
    return false;
  }
  g_tick_semantic_start = {};
  g_record_physics_snapshot = {};
  g_barrel_capture = {};
  g_barrel_capture_tick = UINT64_MAX;
  g_evidence.flags |= kLifecycleIdentity | kObjectIdentity;
  const bridge::Result rearmed =
      bridge::ContinueReplayAsRecordAtClosedBoundary(
          AdapterConfig(), g_replay_frames, prefix_count, &g_runtime);
  if (rearmed != bridge::Result::kObserved) {
    Fault(kErrorAdapter);
    return false;
  }
  __atomic_store_n(&g_control.completed, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.status, kArmed, __ATOMIC_RELEASE);
  __atomic_store_n(&g_control.enabled, 1u, __ATOMIC_RELEASE);
  return true;
}

bool ArchivedCompletedReceiptValid() {
  const bool queued_for_this_generation =
      g_evidence.status == kQueued && g_control.pending_state == 1u &&
      g_control.pending_mode == g_control.mode &&
      g_control.pending_generation == g_control.generation &&
      g_control.pending_reserved == 0u;
  const bool race_end =
      g_evidence.completion_reason == static_cast<std::uint32_t>(
          CompletionReason::kRaceLifecycle) &&
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInactive &&
      g_runtime.tick.complete == 0 &&
      g_evidence.recorded_frames == g_control.archived_frame_count &&
      g_evidence.interval_records == g_control.archived_interval_count;
  const bool manual_checkpoint =
      g_evidence.completion_reason == static_cast<std::uint32_t>(
          CompletionReason::kManualCheckpoint) &&
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      g_runtime.tick.complete == 0 &&
      g_evidence.recorded_frames == g_control.archived_frame_count &&
      g_evidence.interval_records == g_control.archived_interval_count;
  const bool completed_replay =
      g_evidence.completion_reason == static_cast<std::uint32_t>(
          CompletionReason::kFixedFrameLimit) &&
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      g_runtime.tick.complete == 1 &&
      g_evidence.recorded_frames == 0 && g_evidence.interval_records == 0;
  return g_control.archived_generation != 0 &&
      g_control.archived_frame_count != 0 &&
      (g_control.archived_interval_count != 0 ||
       AdapterConfig().allow_zero_integration_updates) &&
      !g_runtime.input_action.tick_open &&
      g_runtime.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      g_runtime.tick.coordinator.generation == g_control.archived_generation &&
      g_runtime.receipt_count == g_control.archived_frame_count &&
      (g_evidence.status == kComplete || queued_for_this_generation) &&
      g_evidence.first_error == 0 &&
      (race_end || manual_checkpoint || completed_replay) &&
      g_evidence.published_ticks == g_control.archived_frame_count;
}

bool RearmArchivedSession(bool replay, bool lifecycle_callback) {
  g_evidence.reserved[0] = 0x300;
  const std::uint32_t expected_active_helpers = lifecycle_callback
      ? 1u + CurrentDispatcherDepth(Tid())
      : 0u;
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      !ArmControlValid(true, false, expected_active_helpers) ||
      g_control.mode != static_cast<std::uint32_t>(
          replay ? RunMode::kReplay : RunMode::kRecord) ||
      !ArchivedCompletedReceiptValid() ||
      !PublishedTargetsValid()) {
    Fault(kErrorControl);
    return false;
  }
  GameImage image{};
  dl_iterate_phdr(FindGameImage, &image);
  g_evidence.reserved[0] = 0x301;
  if (!image.found || image.base != g_control.game_base) {
    Fault(kErrorGameIdentity);
    return false;
  }
  auto* lifecycle = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  if (!PrivateRw(QueryMapping(lifecycle), lifecycle, sizeof(*lifecycle)) ||
      __atomic_load_n(lifecycle, __ATOMIC_RELAXED) != 2u) {
    Fault(kErrorLifecycleIdentity);
    return false;
  }
  if (!ObjectIdentity(g_control.expected_begin_owner,
                      g_control.expected_begin_owner_vptr) ||
      !ObjectIdentity(g_control.expected_interval_owner,
                      g_control.expected_interval_owner_vptr) ||
      !ObjectIdentity(g_control.expected_barrel_owner,
                      g_control.expected_barrel_owner_vptr) ||
      !ObjectIdentity(g_control.expected_setter_object,
                      g_control.expected_setter_vptr) ||
      !ObjectIdentity(g_control.expected_player,
                      g_control.expected_player_vptr) ||
      !ObjectIdentity(g_control.expected_main_object,
                      g_control.expected_main_object_vptr) ||
      !NativePhysicsMappingsValid()) {
    Fault(kErrorObjectIdentity);
    return false;
  }
  auto* pair = reinterpret_cast<std::uint64_t*>(
      g_control.expected_interval_owner + kControlPairOffset);
  auto* service_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_interval_owner + kNitroServiceOffset);
  if (!PrivateRw(QueryMapping(pair), pair, sizeof(*pair)) ||
      !PrivateRw(QueryMapping(service_slot), service_slot,
                 sizeof(*service_slot))) {
    Fault(kErrorMainObjectIdentity);
    return false;
  }
  const std::uintptr_t service =
      __atomic_load_n(service_slot, __ATOMIC_RELAXED);
  if (!ObjectIdentity(service, g_control.game_base +
                                   g_build_profile.nitro_service_vtable_rva) ||
      service > UINTPTR_MAX - 8 ||
      service + 8 != g_control.expected_nitro_state) {
    Fault(kErrorNitroIdentity);
    return false;
  }
  const auto* nitro_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.game_base + g_build_profile.nitro_service_vtable_rva +
      kNitroDispatchSlot);
  if (!PrivateGuestCode(QueryMapping(nitro_slot), nitro_slot,
                        sizeof(*nitro_slot)) ||
      __atomic_load_n(nitro_slot, __ATOMIC_RELAXED) !=
          g_control.game_base + g_build_profile.nitro_dispatch_rva) {
    Fault(kErrorNitroIdentity);
    return false;
  }

  // A fixed-length replay and a manual record checkpoint both leave the old
  // coordinator in-race.  Retry closes that attempt before arming the next
  // generation.  A naturally finished recording is already inactive.
  const bool prior_session_still_in_race =
      g_runtime.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace;
  ResetForArchivedRearm();
  ResetBarrelPrng();
  g_recorded_interval_count = 0;
  g_tick_semantic_start = {};
  g_record_physics_snapshot = {};
  g_barrel_capture = {};
  g_barrel_capture_tick = UINT64_MAX;
  std::memset(g_recorded_frames, 0, sizeof(g_recorded_frames));
  std::memset(g_recorded_intervals, 0, sizeof(g_recorded_intervals));
  g_evidence.flags |= kLifecycleIdentity | kObjectIdentity;
  const bridge::Result rearmed = prior_session_still_in_race
      ? bridge::RearmAfterSealedCheckpointAtRetry(AdapterConfig(), &g_runtime)
      : bridge::RearmAfterArchivedRace(AdapterConfig(), &g_runtime);
  if (rearmed !=
      bridge::Result::kObserved) {
    Fault(kErrorAdapter);
    return false;
  }
  __atomic_store_n(&g_control.completed, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.status, kArmed, __ATOMIC_RELEASE);
  __atomic_store_n(&g_control.enabled, 1u, __ATOMIC_RELEASE);
  return true;
}

bool RearmArchivedReplay() {
  return RearmArchivedSession(true, false);
}

bool RearmArchivedRecord() {
  return RearmArchivedSession(false, false);
}

bool QueueArchivedSession(bool replay) {
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      g_control.enabled != 0 || g_control.completed != 1 ||
      g_control.active_helpers != 0 ||
      g_control.mode != static_cast<std::uint32_t>(
          replay ? RunMode::kReplay : RunMode::kRecord) ||
      !ArmControlValid(true) || !ArchivedCompletedReceiptValid() ||
      !PublishedTargetsValid()) {
    return false;
  }
  __atomic_store_n(&g_control.pending_generation, g_control.generation,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_mode,
                   static_cast<std::uint32_t>(
                       replay ? RunMode::kReplay : RunMode::kRecord),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_reserved, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_state, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.status, kQueued, __ATOMIC_RELEASE);
  return true;
}

bool QueueActiveRecordRetry() {
  const bool active_rearmed_generation =
      g_control.archived_generation != 0;
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      g_control.enabled != 1 || g_control.completed != 0 ||
      g_control.active_helpers != 0 ||
      g_control.mode != static_cast<std::uint32_t>(RunMode::kRecord) ||
      g_control.completion_policy != static_cast<std::uint32_t>(
          CompletionPolicy::kRaceLifecycle) ||
      g_control.generation == 0 || g_control.generation == UINT32_MAX ||
      g_control.pending_mode != 0 || g_control.pending_state != 0 ||
      g_control.pending_generation != 0 || g_control.pending_reserved != 0 ||
      g_evidence.status != kArmed || g_evidence.first_error != 0 ||
      // This command runs while the current recording is active.  The old
      // fresh-generation branch called ArmControlValid() with its default
      // active=false contract, contradicting the enabled==1 precondition
      // above and rejecting every first recording at progress 0x1000.
      !(active_rearmed_generation ? ArmControlValid(true, true)
                                  : ArmControlValid(false, true)) ||
      !PublishedTargetsValid())
    return false;
  __atomic_store_n(&g_control.pending_generation,
                   g_control.generation + 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_mode,
                   static_cast<std::uint32_t>(RunMode::kRecord),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_reserved, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_state, 1u, __ATOMIC_RELEASE);
  return true;
}

bool CancelPending() {
  const bool active_record_queue =
      g_control.enabled == 1 && g_control.completed == 0 &&
      g_evidence.status == kArmed &&
      g_control.pending_mode ==
          static_cast<std::uint32_t>(RunMode::kRecord) &&
      g_control.pending_state == 1u &&
      g_control.pending_generation == g_control.generation + 1u &&
      g_control.pending_reserved == 1u;
  const bool archived_queue =
      g_control.enabled == 0 && g_control.completed == 1 &&
      g_evidence.status == kQueued && g_control.pending_state == 1u &&
      ((g_control.pending_generation == g_control.generation &&
        g_control.pending_reserved == 0u) ||
       (g_control.pending_mode ==
            static_cast<std::uint32_t>(RunMode::kRecord) &&
             g_control.pending_generation == g_control.generation + 1u &&
             g_control.pending_reserved == 1u));
  // A direct-Retry record is queued while the current attempt is active.  If
  // the user seals that attempt before pressing Retry, completion deliberately
  // preserves the pending next generation.  It must remain cancellable without
  // pretending that the sealed predecessor was a QueueArchivedSession.
  const bool completed_record_queue =
      g_control.enabled == 0 && g_control.completed == 1 &&
      g_evidence.status == kComplete && g_control.pending_state == 1u &&
      g_control.pending_mode == static_cast<std::uint32_t>(RunMode::kRecord) &&
      g_control.pending_generation == g_control.generation + 1u &&
      g_control.pending_reserved == 1u;
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      g_control.active_helpers != 0 || g_evidence.first_error != 0 ||
      (!active_record_queue && !archived_queue && !completed_record_queue)) {
    return false;
  }
  __atomic_store_n(&g_control.pending_mode, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_generation, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_reserved, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_control.pending_state, 0u, __ATOMIC_RELEASE);
  if (archived_queue)
    __atomic_store_n(&g_evidence.status, kComplete, __ATOMIC_RELEASE);
  return true;
}

bool ArmSession() {
  // This field is zero in every successful receipt.  During the bounded
  // NativeBridge arm call it records the last completed stage so the host can
  // distinguish a guest-call timeout from a semantic rejection before it
  // terminates an uncertain process.
  g_evidence.reserved[0] = 1;
  if (g_install_state.load(std::memory_order_acquire) != 1) {
    g_evidence.reserved[0] = 0x01000000ULL |
        static_cast<std::uint32_t>(g_install_state.load(
            std::memory_order_relaxed));
    Fault(kErrorControl);
    return false;
  }
  if (!ArmControlValid()) {
    Fault(kErrorControl);
    return false;
  }
  GameImage image{};
  dl_iterate_phdr(FindGameImage, &image);
  g_evidence.reserved[0] = 2;
  if (!image.found || image.base != g_control.game_base) {
    Fault(kErrorGameIdentity);
    return false;
  }
  auto* lifecycle = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  if (!PrivateRw(QueryMapping(lifecycle), lifecycle, sizeof(*lifecycle)) ||
      __atomic_load_n(lifecycle, __ATOMIC_RELAXED) != 2u) {
    Fault(kErrorLifecycleIdentity);
    return false;
  }
  if (!ObjectIdentity(g_control.expected_begin_owner,
                      g_control.expected_begin_owner_vptr) ||
      !ObjectIdentity(g_control.expected_interval_owner,
                      g_control.expected_interval_owner_vptr) ||
      !ObjectIdentity(g_control.expected_barrel_owner,
                      g_control.expected_barrel_owner_vptr) ||
      !ObjectIdentity(g_control.expected_setter_object,
                      g_control.expected_setter_vptr) ||
      !ObjectIdentity(g_control.expected_player,
                      g_control.expected_player_vptr) ||
      !ObjectIdentity(g_control.expected_main_object,
                      g_control.expected_main_object_vptr)) {
    Fault(kErrorObjectIdentity);
    return false;
  }
  if (!NativePhysicsMappingsValid()) {
    Fault(kErrorPhysicsIdentity);
    return false;
  }
  g_evidence.reserved[0] = 3;
  auto* pair = reinterpret_cast<std::uint64_t*>(
      g_control.expected_interval_owner + kControlPairOffset);
  auto* service_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.expected_interval_owner + kNitroServiceOffset);
  if (!PrivateRw(QueryMapping(pair), pair, sizeof(*pair)) ||
      !PrivateRw(QueryMapping(service_slot), service_slot,
                 sizeof(*service_slot))) {
    Fault(kErrorMainObjectIdentity);
    return false;
  }
  const std::uintptr_t service =
      __atomic_load_n(service_slot, __ATOMIC_RELAXED);
  if (!ObjectIdentity(service, g_control.game_base +
                                   g_build_profile.nitro_service_vtable_rva)) {
    Fault(kErrorNitroIdentity);
    return false;
  }
  if (service > UINTPTR_MAX - 8 ||
      service + 8 != g_control.expected_nitro_state) {
    Fault(kErrorNitroIdentity);
    return false;
  }
  const auto* nitro_slot = reinterpret_cast<const std::uintptr_t*>(
      g_control.game_base + g_build_profile.nitro_service_vtable_rva +
      kNitroDispatchSlot);
  if (!PrivateGuestCode(QueryMapping(nitro_slot), nitro_slot,
                        sizeof(*nitro_slot)) ||
      __atomic_load_n(nitro_slot, __ATOMIC_RELAXED) !=
          g_control.game_base + g_build_profile.nitro_dispatch_rva) {
    Fault(kErrorNitroIdentity);
    return false;
  }
  for (std::uint32_t index = 0; index < kHookCount; ++index) {
    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_control.target_entry[index]);
    std::uint8_t expected[kPatchSize]{};
    std::memcpy(expected, kExpectedPrologues[index], kPatchSize);
    if (!PrivateGuestCode(QueryMapping(target), target, HookPatchSize(index)) ||
        std::memcmp(target, expected, HookPatchSize(index)) != 0) {
      Fault(kErrorPatchReadback);
      return false;
    }
  }
  const auto* lifecycle_entry = reinterpret_cast<const std::uint8_t*>(
      g_control.lifecycle_target_entry);
  if (!PrivateGuestCode(QueryMapping(lifecycle_entry), lifecycle_entry,
                        kPatchSize) ||
      std::memcmp(lifecycle_entry, kExpectedLifecyclePrologue,
                  kPatchSize) != 0) {
    Fault(kErrorPatchReadback);
    return false;
  }
  for (std::uint32_t index = 0; index < kInstalledSetterCount; ++index) {
    const auto* slot = reinterpret_cast<const std::uintptr_t*>(
        g_control.setter_slot[index]);
    const auto* original = reinterpret_cast<const std::uint8_t*>(
        g_control.setter_original[index]);
    if (!PrivateReadonly(QueryMapping(slot), slot, sizeof(*slot)) ||
        __atomic_load_n(slot, __ATOMIC_ACQUIRE) !=
            g_control.setter_original[index] ||
        !PrivateGuestCode(QueryMapping(original), original,
                          sizeof(kExpectedSetterBodies[index])) ||
        std::memcmp(original, kExpectedSetterBodies[index],
                    sizeof(kExpectedSetterBodies[index])) != 0) {
      Fault(kErrorSetterIdentity);
      return false;
    }
  }
  g_evidence.reserved[0] = 4;
  ResetForArm();
  g_evidence.reserved[0] = 5;
  g_recorded_interval_count = 0;
  g_tick_semantic_start = {};
  g_record_physics_snapshot = {};
  g_barrel_capture = {};
  g_barrel_capture_tick = UINT64_MAX;
  std::memset(g_recorded_frames, 0, sizeof(g_recorded_frames));
  std::memset(g_recorded_intervals, 0, sizeof(g_recorded_intervals));
  g_evidence.reserved[0] = 6;
  g_evidence.flags |= kLifecycleIdentity | kObjectIdentity;
  if (bridge::Initialize(AdapterConfig(), &g_runtime) !=
      bridge::Result::kObserved) {
    Fault(kErrorAdapter);
    return false;
  }
  ResetBarrelPrng();
  g_evidence.reserved[0] = 7;
  std::uint32_t published = 0;
  for (; published < kHookCount; ++published) {
    g_evidence.reserved[0] = 0x100u + published;
    if (!PublishHook(published)) break;
  }
  if (published != kHookCount) {
    while (published > 0) (void)RestoreHook(--published);
    Fault(kErrorPatchReadback);
    return false;
  }
  g_evidence.reserved[0] = 0x170u;
  if (!PublishLifecycleHook()) {
    while (published > 0) (void)RestoreHook(--published);
    Fault(kErrorPatchReadback);
    return false;
  }
  std::uint32_t random_published = 0;
  for (; random_published < kRandomHookCount; ++random_published) {
    g_evidence.reserved[0] = 0x180u + random_published;
    if (!PublishRandomHook(random_published)) break;
  }
  if (random_published != kRandomHookCount) {
    while (random_published > 0)
      (void)RestoreRandomHook(--random_published);
    while (published > 0) (void)RestoreHook(--published);
    (void)RestoreLifecycleHook();
    Fault(kErrorBarrelRandom);
    return false;
  }
  std::uint32_t setters_published = 0;
  for (; setters_published < kInstalledSetterCount; ++setters_published) {
    g_evidence.reserved[0] = 0x200u + setters_published;
    if (!PublishSetterSlot(setters_published)) break;
  }
  if (setters_published != kInstalledSetterCount) {
    while (setters_published > 0)
      (void)RestoreSetterSlot(--setters_published);
    while (random_published > 0)
      (void)RestoreRandomHook(--random_published);
    while (published > 0) (void)RestoreHook(--published);
    (void)RestoreLifecycleHook();
    Fault(kErrorSetterPublish);
    return false;
  }
  g_evidence.flags |= kAllPatchesPublished | kAllPatchesReadBack |
                      kAllLogicalRx | kControlPairBound | kFixedDeltaBound |
                       kNitroServiceBound | kExactIntervalStream |
                       kSetterVtableBound | kAllSetterSlotsPublished |
                       kPhysicsBackendBound | kBarrelTailBound |
                       kDeterministicBarrelPrngBound;
  g_evidence.reserved[0] = 0;
  __atomic_store_n(&g_evidence.status, kArmed, __ATOMIC_RELEASE);
  __atomic_store_n(&g_control.enabled, 1u, __ATOMIC_RELEASE);
  return true;
}

bool Restore() {
  const std::int32_t prior = g_install_state.exchange(-3);
  if (prior != 1) {
    Fault(kErrorNotInstalled);
    return false;
  }
  __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
  ++g_evidence.restore_calls;
  if (__atomic_load_n(&g_control.active_helpers, __ATOMIC_ACQUIRE) != 0) {
    Fault(kErrorRecursive);
    g_install_state.store(-1);
    return false;
  }
  // Restore can be requested after a host-side wait/transport failure while
  // the payload has begun, but not yet closed, one authoritative tick.  All
  // game threads are stopped by the controller and active_helpers is zero at
  // this point.  Discard that unpublished tail before removing hooks so the
  // restored carrier is immediately reusable in this same game process.
  if (g_runtime.input_action.tick_open) {
    const std::uint64_t open_tick = g_runtime.input_action.tick;
    const bridge::Result discarded =
        bridge::DiscardOpenTickForRaceEnd(AdapterConfig(), &g_runtime);
    g_evidence.last_g3_result = g_runtime.last_g3_result;
    g_evidence.last_g4_result = g_runtime.last_g4_result;
    if (discarded != bridge::Result::kObserved ||
        !RestoreDiscardedTickSemantics(open_tick)) {
      ++g_evidence.core_faults;
      Fault(kErrorAdapter);
      g_install_state.store(-1);
      return false;
    }
  }
  if (g_runtime.input_action.tick_open ||
      g_runtime.tick.coordinator.tick_phase != coordinator::TickPhase::kClosed) {
    ++g_evidence.core_faults;
    Fault(kErrorAdapter);
    g_install_state.store(-1);
    return false;
  }
  bool restored = true;
  for (std::uint32_t index = kInstalledSetterCount; index > 0; --index)
    restored = RestoreSetterSlot(index - 1) && restored;
  for (std::uint32_t index = kRandomHookCount; index > 0; --index)
    restored = RestoreRandomHook(index - 1) && restored;
  restored = RestoreLifecycleHook() && restored;
  for (std::uint32_t index = kHookCount; index > 0; --index)
    restored = RestoreHook(index - 1) && restored;
  if (!restored) {
    Fault(kErrorRestoreReadback);
    g_install_state.store(-1);
    return false;
  }
  g_evidence.flags |= kAllRestored | kAllSetterSlotsRestored;
  // Eight ordinary code hooks, two deterministic barrel-random hooks and both
  // proven adjusted setter slots are session-scoped. Accelerator stays
  // intentionally unhooked.
  g_g4_interval_tail_v1 = 0;
  g_g4_final_tail_v1 = 0;
  g_g4_frame_tail_v1 = 0;
  g_g4_nitro_tail_v1 = 0;
  g_g4_submit_tail_v1 = 0;
  g_g4_barrel_roll_tail_v1 = 0;
  g_g4_barrel_yaw_tail_v1 = 0;
  g_g4_dispatch_tail_v1 = 0;
  g_g4_lifecycle_tail_v1 = 0;
  // Restore is also the clean terminal transition for a user-cancelled,
  // still-active record/replay.  Such a run has completed==0 on entry.  The
  // reinstall path intentionally accepts a fully restored payload in the same
  // process, but its RestoredSessionReusable contract requires completed==1.
  // Publish that terminal bit only after every hook/slot has been restored and
  // read back, so a successful Restore can always be reused without restarting
  // the game while a partial restore can never masquerade as reusable.
  __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
  g_install_state.store(2, std::memory_order_release);
  __atomic_store_n(&g_evidence.status, kRestored, __ATOMIC_RELEASE);
  return true;
}

std::uint64_t Return(std::uint32_t tag) {
  return (static_cast<std::uint64_t>(Tid()) << 32) | tag;
}

}  // namespace

extern "C" __attribute__((constructor)) void G4InitializeV1() {
  Initialize();
}

extern "C" __attribute__((visibility("default"))) jlong
a9tas_g4_command_v1(JNIEnv*, jobject, jlong command) {
  bool passed = false;
  std::uint32_t tag = kFailureTag;
  if (static_cast<std::uint64_t>(command) ==
      static_cast<std::uint64_t>(Command::kInstallPassive)) {
    GameImage image{};
    dl_iterate_phdr(FindGameImage, &image);
    passed = InstallPassive(image);
    if (passed) tag = kInstallTag;
  } else if (static_cast<std::uint64_t>(command) ==
      static_cast<std::uint64_t>(Command::kArmSession)) {
    passed = ArmSession();
    if (passed) tag = kArmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(Command::kRestore)) {
    passed = Restore();
    if (passed) tag = kRestoreTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(
                  Command::kRearmArchivedReplay)) {
    passed = RearmArchivedReplay();
    if (passed) tag = kRearmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(
                 Command::kRearmArchivedRecord)) {
    passed = RearmArchivedRecord();
    if (passed) tag = kRearmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(Command::kSealPausedRecord)) {
    passed = SealPausedRecord();
    if (passed) tag = kArmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(
                 Command::kRearmPausedReplayRecord)) {
    passed = RearmPausedReplayRecord();
    if (passed) tag = kRearmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(Command::kQueueArchivedReplay)) {
    passed = QueueArchivedSession(true);
    if (passed) tag = kRearmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(Command::kQueueArchivedRecord)) {
    passed = QueueArchivedSession(false);
    if (passed) tag = kRearmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(Command::kQueueActiveRecordRetry)) {
    passed = QueueActiveRecordRetry();
    if (passed) tag = kRearmTag;
  } else if (static_cast<std::uint64_t>(command) ==
             static_cast<std::uint64_t>(Command::kCancelPending)) {
    passed = CancelPending();
    if (passed) tag = kRearmTag;
  } else {
    Fault(kErrorCommand);
  }
  __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                      "command=%lld passed=%d status=%d ticks=%u error=%u",
                      static_cast<long long>(command), passed ? 1 : 0,
                      g_evidence.status, g_runtime.receipt_count,
                      g_evidence.first_error);
  return static_cast<jlong>(Return(tag));
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_g4_protocol_v1() {
  return kVersion;
}

extern "C" __attribute__((visibility("default"))) Control*
a9tas_g4_control_v1() {
  return &g_control;
}

extern "C" __attribute__((visibility("default"))) Evidence*
a9tas_g4_evidence_v1() {
  return &g_evidence;
}

extern "C" __attribute__((visibility("default"))) bridge::StateV1*
a9tas_g4_runtime_v1() {
  return &g_runtime;
}

extern "C" __attribute__((visibility("default")))
build_profile::Profile* a9tas_g4_build_profile_v1() {
  return &g_build_profile;
}

extern "C" __attribute__((visibility("default")))
recording::RecordingFrameV1* a9tas_g4_replay_frames_v1() {
  return g_replay_frames;
}

extern "C" __attribute__((visibility("default")))
g4::IntervalSampleV1* a9tas_g4_replay_intervals_v1() {
  return g_replay_intervals;
}

extern "C" __attribute__((visibility("default")))
recording::RecordingFrameV1* a9tas_g4_recorded_frames_v1() {
  return g_recorded_frames;
}

extern "C" __attribute__((visibility("default")))
g4::IntervalSampleV1* a9tas_g4_recorded_intervals_v1() {
  return g_recorded_intervals;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_control_data_v1 = reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_evidence_data_v1 = reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_runtime_data_v1 = reinterpret_cast<std::uintptr_t>(&g_runtime);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_build_profile_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_build_profile);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_replay_frames_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_replay_frames);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_replay_intervals_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_replay_intervals);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_recorded_frames_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_recorded_frames);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g4_recorded_intervals_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_recorded_intervals);
