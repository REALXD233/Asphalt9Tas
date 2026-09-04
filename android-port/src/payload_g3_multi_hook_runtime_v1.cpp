#include "g3_multi_hook_runtime_v1.h"

#include <android/log.h>
#include <elf.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace adapter = a9tas::g3_boundary_adapter_v1;
using namespace a9tas::g3_multi_hook_runtime_v1;

namespace {

constexpr const char* kTag = "A9TAS_G3";
constexpr const char* kGameBasename = "libAsphalt9.so";
constexpr const char* kGameBuildId =
    "e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b";
constexpr std::size_t kPatchSize = 16;
constexpr int kLogicalCodeProtection = PROT_READ | PROT_EXEC;
constexpr std::uint32_t kArmTag = 0x47334901;
constexpr std::uint32_t kRestoreTag = 0x47334902;
constexpr std::uint32_t kInstallTag = 0x47334903;
constexpr std::uint32_t kFailureTag = 0x47334910;

constexpr std::uintptr_t kTargetRvas[kHookCount] = {
    adapter::kPhysicsIntervalRva,
    adapter::kFinalWriterRva,
    adapter::kFrameEventRva,
};

constexpr std::uint8_t kExpectedPrologues[kHookCount][kPatchSize] = {
    {0xff, 0xc3, 0x00, 0xd1, 0xf5, 0x53, 0x01, 0xa9,
     0xf3, 0x7b, 0x02, 0xa9, 0x35, 0x11, 0x91, 0x52},
    {0xec, 0x0f, 0x17, 0xfc, 0xeb, 0x2b, 0x01, 0x6d,
     0xe9, 0x23, 0x02, 0x6d, 0xfc, 0x6f, 0x03, 0xa9},
    {0xff, 0xc3, 0x00, 0xd1, 0xf4, 0x0b, 0x00, 0xf9,
     0xf3, 0x7b, 0x02, 0xa9, 0x28, 0x00, 0x40, 0xf9},
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
alignas(64) adapter::State g_runtime{};
std::atomic<std::int32_t> g_install_state{0};
std::atomic_flag g_runtime_lock = ATOMIC_FLAG_INIT;

extern "C" {
__attribute__((visibility("hidden"))) std::uintptr_t g_g3_interval_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g3_final_tail_v1 = 0;
__attribute__((visibility("hidden"))) std::uintptr_t g_g3_frame_tail_v1 = 0;
}

std::size_t AlignNote(std::size_t value) {
  return (value + 3U) & ~std::size_t{3U};
}

bool BuildIdMatches(std::uintptr_t base, const Elf64_Phdr* programs,
                    std::uint16_t count) {
  constexpr char kHex[] = "0123456789abcdef";
  char observed[41]{};
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
        for (std::size_t byte = 0; byte < note.n_descsz; ++byte) {
          observed[byte * 2] = kHex[description[byte] >> 4];
          observed[byte * 2 + 1] = kHex[description[byte] & 0xf];
        }
        return std::strcmp(observed, kGameBuildId) == 0;
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
                      static_cast<std::uint16_t>(info->dlpi_phnum)))
    return 0;
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

void ResetEvidence() {
  std::memset(&g_evidence, 0, sizeof(g_evidence));
  std::memcpy(g_evidence.magic, kEvidenceMagic, sizeof(kEvidenceMagic));
  g_evidence.version = kVersion;
  g_evidence.size = sizeof(Evidence);
  g_evidence.status = kPassive;
  g_evidence.flags = kNoGameplayWrites | kNoExternalPerFrameStop;
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

std::uint32_t Tid() {
  return static_cast<std::uint32_t>(syscall(__NR_gettid));
}

void Note(std::uint32_t hook, adapter::Result result, std::uint32_t tid) {
  if (g_evidence.first_tid[hook] == 0) g_evidence.first_tid[hook] = tid;
  g_evidence.last_tid[hook] = tid;
  if (result == adapter::Result::kIgnoredUnqualified ||
      result == adapter::Result::kWaitingForRace ||
      result == adapter::Result::kWaitingForTick) {
    ++g_evidence.ignored_events;
  } else if (result == adapter::Result::kObserved ||
             result == adapter::Result::kComplete) {
    ++g_evidence.qualified_events[hook];
  } else {
    ++g_evidence.core_faults;
    g_evidence.reserved0 = hook;
    g_evidence.reserved1[0] =
        static_cast<std::uint32_t>(g_runtime.coordinator.tick_phase);
    g_evidence.reserved1[1] = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(g_runtime.coordinator.last_result));
    g_evidence.reserved1[2] = g_runtime.coordinator.tick;
    Fault(kErrorAdapter);
  }
  if (result == adapter::Result::kComplete) {
    g_evidence.published_ticks = g_runtime.receipt_count;
    g_evidence.last_tick = g_runtime.coordinator.tick;
    __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_evidence.status, kComplete, __ATOMIC_RELEASE);
  }
}

adapter::Config AdapterConfig() {
  return {.session_id = g_control.session_id,
          .generation = g_control.generation,
          .frame_limit = g_control.frame_limit,
          .game_base = g_control.game_base,
          .expected_begin_owner = g_control.expected_begin_owner,
          .expected_begin_owner_vptr = g_control.expected_begin_owner_vptr,
          .expected_interval_owner = g_control.expected_interval_owner,
          .expected_interval_owner_vptr =
              g_control.expected_interval_owner_vptr,
          .expected_player = g_control.expected_player,
          .expected_player_vptr = g_control.expected_player_vptr};
}

}  // namespace

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G3IntervalOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0x30\n"
      "stp x21, x20, [sp, #0x10]\n"
      "stp x19, x30, [sp, #0x20]\n"
      "mov w21, #0x8889\n"
      "adrp x17, :got:g_g3_interval_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g3_interval_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G3FinalOriginalV1() {
  __asm__ volatile(
      "str d12, [sp, #-0x90]!\n"
      "stp d11, d10, [sp, #0x10]\n"
      "stp d9, d8, [sp, #0x20]\n"
      "stp x28, x27, [sp, #0x30]\n"
      "adrp x17, :got:g_g3_final_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g3_final_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G3FrameOriginalV1() {
  __asm__ volatile(
      "sub sp, sp, #0x30\n"
      "str x20, [sp, #0x10]\n"
      "stp x19, x30, [sp, #0x20]\n"
      "ldr x8, [x1]\n"
      "adrp x17, :got:g_g3_frame_tail_v1\n"
      "ldr x17, [x17, #:got_lo12:g_g3_frame_tail_v1]\n"
      "ldr x17, [x17]\n"
      "br x17\n");
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G3IntervalBeforeV1(void* owner) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kTickHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
      owner == nullptr)
    return;
  LockRuntime();
  const std::uintptr_t vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(owner), __ATOMIC_RELAXED);
  const auto* phase = reinterpret_cast<const std::uint32_t*>(
      g_control.lifecycle_state_address);
  const std::uint32_t lifecycle = __atomic_load_n(phase, __ATOMIC_RELAXED);
  const std::uint32_t tid = Tid();
  g_evidence.last_object[0] = reinterpret_cast<std::uintptr_t>(owner);
  g_evidence.last_vptr[0] = vptr;
  g_evidence.last_lifecycle_state = lifecycle;
  const adapter::Result result = adapter::ObserveTickBeginAndPrePhysics(
      AdapterConfig(), &g_runtime, reinterpret_cast<std::uintptr_t>(owner),
      vptr, lifecycle, tid);
  Note(kTickHook, result, tid);
  UnlockRuntime();
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G3FinalAfterV1(void* player) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kFinalWriterHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
      player == nullptr)
    return;
  LockRuntime();
  const std::uintptr_t vptr = __atomic_load_n(
      reinterpret_cast<const std::uintptr_t*>(player), __ATOMIC_RELAXED);
  const std::uint32_t tid = Tid();
  g_evidence.last_object[1] = reinterpret_cast<std::uintptr_t>(player);
  g_evidence.last_vptr[1] = vptr;
  const adapter::Result result = adapter::ObserveFinalWriterReturn(
      AdapterConfig(), &g_runtime, reinterpret_cast<std::uintptr_t>(player),
      vptr, tid);
  Note(kFinalWriterHook, result, tid);
  UnlockRuntime();
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G3FrameAfterV1(std::uintptr_t caller_return) {
  __atomic_fetch_add(&g_evidence.wrapper_entries[kFrameEventHook], 1ULL,
                     __ATOMIC_RELAXED);
  if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0) return;
  LockRuntime();
  const std::uint32_t tid = Tid();
  g_evidence.last_caller_return = caller_return;
  const adapter::Result result = adapter::ObserveFrameEventReturn(
      AdapterConfig(), &g_runtime, caller_return, tid);
  Note(kFrameEventHook, result, tid);
  UnlockRuntime();
}

#define G3_SAVE_BEFORE_HELPER                         \
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

#define G3_RESTORE_BEFORE_HELPER                      \
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

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G3IntervalEntryV1() {
  __asm__ volatile(G3_SAVE_BEFORE_HELPER
                   "ldr x0, [sp, #0x00]\n"
                   "bl G3IntervalBeforeV1\n"
                   G3_RESTORE_BEFORE_HELPER
                   "b G3IntervalOriginalV1\n");
}

// The two after-original wrappers use the G2-validated full volatile-register,
// SIMD, NZCV, FPCR and FPSR preservation sequence.
#define G3_AFTER_ORIGINAL_SAVE                         \
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

#define G3_AFTER_ORIGINAL_RESTORE                      \
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
G3FinalEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x0\n"
      "bl G3FinalOriginalV1\n"
      G3_AFTER_ORIGINAL_SAVE
      "mov x0, x19\n"
      "bl G3FinalAfterV1\n"
      G3_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G3FrameEntryV1() {
  __asm__ volatile(
      "sub sp, sp, #0x160\n"
      "stp x19, x20, [sp, #0x00]\n"
      "str x30, [sp, #0x10]\n"
      "mov x19, x30\n"
      "bl G3FrameOriginalV1\n"
      G3_AFTER_ORIGINAL_SAVE
      "mov x0, x19\n"
      "bl G3FrameAfterV1\n"
      G3_AFTER_ORIGINAL_RESTORE
      "ldr x30, [sp, #0x10]\n"
      "ldp x19, x20, [sp, #0x00]\n"
      "add sp, sp, #0x160\n"
      "ret\n");
}

namespace {

const std::uintptr_t kWrappers[kHookCount] = {
    reinterpret_cast<std::uintptr_t>(&G3IntervalEntryV1),
    reinterpret_cast<std::uintptr_t>(&G3FinalEntryV1),
    reinterpret_cast<std::uintptr_t>(&G3FrameEntryV1),
};

bool PassiveControlFresh() {
  if (std::memcmp(g_control.magic, kControlMagic, sizeof(kControlMagic)) != 0 ||
      g_control.version != kVersion || g_control.size != sizeof(Control) ||
      g_control.enabled != 0 || g_control.frame_limit != 0 ||
      g_control.generation != 0 || g_control.completed != 0 ||
      g_control.active_helpers != 0 || g_control.reserved0 != 0 ||
      g_control.session_id != 0 || g_control.expected_begin_owner != 0 ||
      g_control.expected_begin_owner_vptr != 0 ||
      g_control.expected_interval_owner != 0 ||
      g_control.expected_interval_owner_vptr != 0 ||
      g_control.expected_player != 0 || g_control.expected_player_vptr != 0 ||
      g_control.lifecycle_state_address != 0 || g_control.game_base != 0 ||
      g_control.begin_continuation != 0 ||
      g_control.begin_branch_destination != 0)
    return false;
  for (std::uint32_t index = 0; index < kHookCount; ++index)
    if (g_control.target_entry[index] != 0 ||
        g_control.target_tail[index] != 0 || g_control.wrapper[index] != 0)
      return false;
  for (std::uint64_t value : g_control.reserved)
    if (value != 0) return false;
  return true;
}

bool ArmControlValid() {
  if (std::memcmp(g_control.magic, kControlMagic, sizeof(kControlMagic)) != 0 ||
      g_control.version != kVersion || g_control.size != sizeof(Control) ||
      g_control.enabled != 0 || g_control.completed != 0 ||
      g_control.active_helpers != 0 ||
      (g_control.frame_limit != 5 && g_control.frame_limit != 60 &&
       g_control.frame_limit != 900) ||
      g_control.generation == 0 || g_control.session_id == 0 ||
      g_control.expected_begin_owner == 0 ||
      g_control.expected_begin_owner_vptr == 0 ||
      g_control.expected_begin_owner != g_control.expected_interval_owner ||
      g_control.expected_begin_owner_vptr !=
          g_control.expected_interval_owner_vptr ||
      g_control.expected_interval_owner == 0 ||
      g_control.expected_interval_owner_vptr == 0 ||
      g_control.expected_player == 0 || g_control.expected_player_vptr == 0 ||
      g_control.lifecycle_state_address == 0 || g_control.game_base == 0 ||
      g_control.begin_continuation != 0 ||
      g_control.begin_branch_destination != 0)
    return false;
  for (std::uint32_t index = 0; index < kHookCount; ++index)
    if (g_control.target_entry[index] !=
            g_control.game_base + kTargetRvas[index] ||
        g_control.target_tail[index] !=
            g_control.target_entry[index] + HookPatchSize(index) ||
        g_control.wrapper[index] != kWrappers[index])
      return false;
  for (std::uint64_t value : g_control.reserved)
    if (value != 0) return false;
  return true;
}

bool ObjectIdentity(std::uint64_t object, std::uint64_t expected_vptr) {
  const auto* pointer = reinterpret_cast<const std::uintptr_t*>(object);
  return PrivateRw(QueryMapping(pointer), pointer, sizeof(*pointer)) &&
         __atomic_load_n(pointer, __ATOMIC_RELAXED) == expected_vptr;
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

bool InstallPassive(const GameImage& image) {
  std::int32_t expected = 0;
  if (!g_install_state.compare_exchange_strong(expected, -2)) {
    Fault(kErrorAlreadyInstalled);
    return false;
  }
  ResetEvidence();
  ++g_evidence.install_calls;
  if (!PassiveControlFresh()) {
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
    g_control.target_entry[index] = image.base + kTargetRvas[index];
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
  g_evidence.flags |= kAllPrologues;
  g_g3_interval_tail_v1 = g_control.target_tail[kTickHook];
  g_g3_final_tail_v1 = g_control.target_tail[kFinalWriterHook];
  g_g3_frame_tail_v1 = g_control.target_tail[kFrameEventHook];
  // Passive installation validates the exact three targets but changes no
  // game code. All hooks are session-scoped and published together at arm.
  g_evidence.flags |= kPayloadPreloaded;
  g_install_state.store(1, std::memory_order_release);
  __atomic_store_n(&g_evidence.status, kPassiveInstalled, __ATOMIC_RELEASE);
  return true;
}

void ResetForArm() {
  std::uint64_t raw_entries[kHookCount]{};
  std::memcpy(raw_entries, g_evidence.wrapper_entries, sizeof(raw_entries));
  const std::uint64_t install_calls = g_evidence.install_calls;
  const std::uint64_t flags = g_evidence.flags;
  ResetEvidence();
  std::memcpy(g_evidence.wrapper_entries, raw_entries, sizeof(raw_entries));
  g_evidence.install_calls = install_calls;
  g_evidence.flags = flags;
}

bool ArmSession() {
  if (g_install_state.load(std::memory_order_acquire) != 1 ||
      !ArmControlValid()) {
    Fault(kErrorControl);
    return false;
  }
  GameImage image{};
  dl_iterate_phdr(FindGameImage, &image);
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
  if (!ObjectIdentity(g_control.expected_interval_owner,
                      g_control.expected_interval_owner_vptr) ||
      !ObjectIdentity(g_control.expected_player,
                      g_control.expected_player_vptr)) {
    Fault(kErrorObjectIdentity);
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
  ResetForArm();
  g_evidence.flags |= kLifecycleIdentity | kObjectIdentity;
  if (adapter::Initialize(AdapterConfig(), &g_runtime) !=
      adapter::Result::kObserved) {
    Fault(kErrorAdapter);
    return false;
  }
  std::uint32_t published = 0;
  for (; published < kHookCount; ++published) {
    if (!PublishHook(published)) break;
  }
  if (published != kHookCount) {
    while (published > 0) (void)RestoreHook(--published);
    Fault(kErrorPatchReadback);
    return false;
  }
  g_evidence.flags |= kAllPatchesPublished | kAllPatchesReadBack |
                      kAllLogicalRx;
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
  bool restored = true;
  for (std::uint32_t index = kHookCount; index > 0; --index)
    restored = RestoreHook(index - 1) && restored;
  if (!restored) {
    Fault(kErrorRestoreReadback);
    g_install_state.store(-1);
    return false;
  }
  g_evidence.flags |= kAllRestored;
  // All three hooks are session-scoped and are restored together.
  g_g3_interval_tail_v1 = 0;
  g_g3_final_tail_v1 = 0;
  g_g3_frame_tail_v1 = 0;
  g_install_state.store(2, std::memory_order_release);
  __atomic_store_n(&g_evidence.status, kRestored, __ATOMIC_RELEASE);
  return true;
}

std::uint64_t Return(std::uint32_t tag) {
  return (static_cast<std::uint64_t>(Tid()) << 32) | tag;
}

}  // namespace

extern "C" __attribute__((constructor)) void G3InitializeV1() {
  Initialize();
}

extern "C" __attribute__((visibility("default"))) jlong
a9tas_g3_command_v1(JNIEnv*, jobject, jlong command) {
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
a9tas_g3_protocol_v1() {
  return kVersion;
}

extern "C" __attribute__((visibility("default"))) Control*
a9tas_g3_control_v1() {
  return &g_control;
}

extern "C" __attribute__((visibility("default"))) Evidence*
a9tas_g3_evidence_v1() {
  return &g_evidence;
}

extern "C" __attribute__((visibility("default"))) adapter::State*
a9tas_g3_runtime_v1() {
  return &g_runtime;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g3_control_data_v1 = reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g3_evidence_data_v1 = reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g3_runtime_data_v1 = reinterpret_cast<std::uintptr_t>(&g_runtime);
