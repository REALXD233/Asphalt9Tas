#pragma once

// Optional live-candidate integration used only when the unified executor is
// explicitly compiled with A9TAS_FINAL_WRITER_REPLAY_V1.  The surrounding
// translation unit supplies the proven scheduler/HWBP primitives.

#include <sys/stat.h>

#include <array>
#include <optional>
#include <vector>

#include "final_writer_cursor_binding_v1.h"
#include "final_writer_replay_elf_resolver_v1.h"
#include "final_writer_storage_transaction_v1.h"
#include "final_writer_target_blob_protocol_v1.h"
#include "final_writer_transaction_core_v1.h"

#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
#if !defined(A9TAS_NAL_ACTION_PAYLOAD_REVIEW) || \
    A9TAS_NAL_ACTION_PAYLOAD_REVIEW != 2
#error "final-writer natural action requires the hash-pinned replay payload"
#endif
#include "final_writer_natural_action_runtime_v1.h"
#include "natural_action_lifecycle_elf_resolver_v1.h"
#include "natural_action_scheduler_report_v1.h"
#endif

extern const char* g_a9tas_final_writer_target_blob_path_v1;
extern const char* g_a9tas_final_writer_payload_report_path_v1;

namespace a9tas::final_writer_unified_v1 {

using final_writer_replay_v1::Control;
using final_writer_replay_v1::Evidence;
using final_writer_replay_v1::FrameAudit;
using final_writer_replay_v1::FrameTarget;

struct Runtime {
  final_writer_replay_elf_v1::Layout payload{};
  final_writer_transaction_core_v1::Prepared prepared{};
  std::vector<std::uint8_t> target_blob;
  std::vector<FrameTarget> targets;
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
  natural_action_lifecycle_elf_v1::Layout natural_payload{};
  std::optional<final_writer_natural_action_runtime_v1::Runtime>
      natural_runtime;
#else
  final_writer_cursor_binding_v1::Binding binding{2};
#endif
  bool configured{};
  bool installed{};
  bool first_frame_others_frozen{};
  bool prearm_all_threads_frozen{};
};

enum class PrearmInstallResult : std::uint32_t {
  kOk = 0,
  kInvalidRuntime = 1,
  kStopTrackedThread = 2,
  kAttachNewThread = 3,
  kThreadSetDidNotStabilize = 4,
  kReadObjectVptr = 5,
  kUnexpectedObjectVptr = 6,
  kWriteShadowVptr = 7,
};

#pragma pack(push, 1)
struct PayloadReportHeader {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint32_t audit_size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uint32_t reserved0;
  std::uint64_t pid;
  std::uint64_t library_base;
  std::uint64_t object;
  std::uint64_t wrapper;
  std::uint64_t shadow_vptr;
  std::uint64_t original_vptr;
  std::uint64_t control_address;
  std::uint64_t target_address;
  std::uint64_t audit_address;
  std::uint64_t evidence_address;
  std::uint8_t recording_sha256[32];
  std::uint8_t payload_sha256[32];
  Evidence evidence;
};
#pragma pack(pop)

static_assert(sizeof(PayloadReportHeader) == 368,
              "final-writer payload report ABI");

inline constexpr char kPayloadReportMagic[8] = {
    'A', '9', 'F', 'W', 'R', '1', 0, 0};
inline constexpr std::uint32_t kPayloadReportFlags = 0x1f;

inline bool ReadWholeFile(const char* path, std::vector<std::uint8_t>* output,
                          std::uint32_t* size_out,
                          std::uint8_t sha256_out[32]) {
  if (path == nullptr || output == nullptr || size_out == nullptr ||
      sha256_out == nullptr)
    return false;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info {};
  bool ok = fstat(fd, &info) == 0 && info.st_size > 0 &&
            info.st_size <= 16 * 1024 * 1024 &&
            static_cast<std::uint64_t>(info.st_size) <= UINT32_MAX;
  std::vector<std::uint8_t> bytes;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(info.st_size));
    ok = final_writer_replay_elf_v1::detail::ReadAt(
        fd, 0, bytes.data(), bytes.size());
  }
  if (ok) {
    ok = final_writer_replay_elf_v1::detail::HashFile(fd, sha256_out);
  }
  close(fd);
  if (!ok) return false;
  *size_out = static_cast<std::uint32_t>(bytes.size());
  *output = std::move(bytes);
  return true;
}

inline bool WriteArbitraryVerified(void* context, std::uintptr_t address,
                                   const void* data, std::size_t size) {
  if (context == nullptr || address == 0 || data == nullptr || size == 0)
    return false;
  const int mem = *static_cast<const int*>(context);
  const auto* input = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const ssize_t count = pwrite(mem, input + written, size - written,
                                 static_cast<off_t>(address + written));
    if (count <= 0) return false;
    written += static_cast<std::size_t>(count);
  }
  std::vector<std::uint8_t> verify(size);
  return ReadExact(mem, address, verify.data(), verify.size()) &&
         std::memcmp(verify.data(), data, size) == 0;
}

#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
inline bool ReadArbitrary(void* context, std::uintptr_t address, void* output,
                          std::size_t size) {
  return context != nullptr && output != nullptr && address != 0 && size != 0 &&
         ReadExact(*static_cast<const int*>(context), address, output, size);
}

inline bool ConfigureNaturalAction(pid_t pid, int mem,
                                   std::uintptr_t vehicle_owner,
                                   std::uint32_t frame_count,
                                   Runtime* runtime) {
  using NalControl = natural_action_scheduler_report_v1::PayloadControl;
  using NalEvidence = natural_action_scheduler_report_v1::PayloadEvidence;
  using Mailbox = natural_action_callback_v1::Mailbox;
  if (runtime == nullptr || vehicle_owner == 0 || frame_count < 2) return false;
  const char* failure = nullptr;
  if (!natural_action_lifecycle_elf_v1::Resolve(
          pid, mem, &runtime->natural_payload, &failure)) {
    (void)failure;
    return false;
  }
  NalControl control{};
  NalEvidence evidence{};
  Mailbox mailbox{};
  if (!ReadExact(mem, runtime->natural_payload.control, &control,
                 sizeof(control)) ||
      !ReadExact(mem, runtime->natural_payload.evidence, &evidence,
                 sizeof(evidence)) ||
      !ReadExact(mem, runtime->natural_payload.mailbox, &mailbox,
                 sizeof(mailbox)))
    return false;
  const char control_magic[8] = {'A', '9', 'N', 'A', 'L', '1', 0, 0};
  const char evidence_magic[8] = {'A', '9', 'N', 'A', 'X', '1', 0, 0};
  const std::uint64_t session =
      natural_action_callback_v1::LoadAcquire(&mailbox.session_control);
  const std::uint64_t claimed =
      natural_action_callback_v1::LoadAcquire(&mailbox.claimed_sequence);
  const std::uint64_t completed =
      natural_action_callback_v1::LoadAcquire(&mailbox.completed_sequence);
  if (std::memcmp(control.magic, control_magic, sizeof(control_magic)) != 0 ||
      control.version != 1 || control.size != sizeof(control) ||
      control.expected_car != vehicle_owner ||
      control.vehicle_owner != vehicle_owner ||
      control.dedicated_object != runtime->natural_payload.dedicated_object ||
      control.dedicated_vptr != runtime->natural_payload.dedicated_vptr ||
      control.session_id == 0 || control.expected_producer_tid == 0 ||
      control.remove_requested != 0 || control.reserved0 != 0 ||
      control.reserved[0] == 0 ||
      std::memcmp(evidence.magic, evidence_magic, sizeof(evidence_magic)) != 0 ||
      evidence.version != 1 || evidence.size != sizeof(evidence) ||
      evidence.bootstrap_entries != 1 || evidence.original_calls != 1 ||
      evidence.original_returns != 1 || evidence.registration_attempts != 1 ||
      evidence.registration_returns != 1 || evidence.failures != 0 ||
      evidence.protocol_state != 1 || evidence.last_status != 1 ||
      evidence.removal_attempts != 0 || evidence.removal_returns != 0 ||
      !natural_action_callback_v1::MailboxValid(mailbox) ||
      !natural_action_callback_v1::Armed(session) ||
      natural_action_callback_v1::SessionId(session) != control.session_id ||
      claimed != 0 || completed != 0 ||
      natural_action_callback_v1::LoadAcquire(
          &mailbox.published_selector) != 0)
    return false;
  runtime->natural_runtime.emplace(
      frame_count, control.session_id, vehicle_owner,
      control.expected_producer_tid,
      natural_action_replay_host_v1::Layout{
          runtime->natural_payload.mailbox});
  return !runtime->natural_runtime->faulted();
}

inline natural_action_replay_host_v1::Backend NaturalBackend(int* mem) {
  return {mem, &ReadArbitrary, &WriteArbitraryVerified};
}
#endif

inline bool Setup(pid_t pid, int mem, std::uintptr_t library_base,
                  std::uintptr_t object, std::uintptr_t native_pose,
                  std::uintptr_t native_linear, const char* recording_path,
                  std::uint32_t frame_count, Runtime* runtime) {
  using namespace final_writer_replay_v1;
  if (runtime == nullptr || recording_path == nullptr ||
      g_a9tas_final_writer_target_blob_path_v1 == nullptr ||
      frame_count < 2 || frame_count > kMaximumFrames)
    return false;
  Runtime result{};
#ifndef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
  result.binding = final_writer_cursor_binding_v1::Binding(frame_count);
#endif
  if (!final_writer_replay_elf_v1::Resolve(pid, mem, &result.payload))
    return false;
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
  if (!ConfigureNaturalAction(pid, mem, object, frame_count, &result))
    return false;
#endif

  std::vector<std::uint8_t> recording_bytes;
  std::uint32_t recording_size = 0;
  std::uint8_t recording_sha256[32]{};
  if (!ReadWholeFile(recording_path, &recording_bytes, &recording_size,
                     recording_sha256))
    return false;
  std::uint32_t blob_size = 0;
  std::uint8_t blob_sha256[32]{};
  if (!ReadWholeFile(g_a9tas_final_writer_target_blob_path_v1,
                     &result.target_blob, &blob_size, blob_sha256))
    return false;
  (void)blob_sha256;
  final_writer_target_blob_v1::View view{};
  if (!final_writer_target_blob_v1::Decode(
          result.target_blob.data(), result.target_blob.size(),
          recording_sha256, recording_size, &view) ||
      view.header.frame_count != frame_count)
    return false;
  result.targets.resize(frame_count);
  for (std::uint32_t index = 0; index < frame_count; ++index)
    std::memcpy(&result.targets[index],
                view.records + static_cast<std::size_t>(index) *
                                   sizeof(FrameTarget),
                sizeof(FrameTarget));

  std::array<std::uint8_t, kPrimaryShadowSize> original_table{};
  Control initial_control{};
  Evidence initial_evidence{};
  std::uintptr_t observed_vptr = 0;
  if (!ReadExact(mem, library_base + kPrimaryPrefixRva,
                 original_table.data(), original_table.size()) ||
      !ReadExact(mem, object, &observed_vptr, sizeof(observed_vptr)) ||
      !ReadExact(mem, result.payload.control, &initial_control,
                 sizeof(initial_control)) ||
      !ReadExact(mem, result.payload.evidence, &initial_evidence,
                 sizeof(initial_evidence)))
    return false;
  if (final_writer_transaction_core_v1::Prepare(
          original_table.data(), library_base, object, observed_vptr,
          result.payload.shadow, result.payload.wrapper, native_pose,
          native_linear, recording_sha256, frame_count, initial_control,
          initial_evidence, &result.prepared) !=
      final_writer_transaction_core_v1::Result::kOk)
    return false;

  final_writer_storage_transaction_v1::Backend storage_backend{
      const_cast<int*>(&mem), &WriteArbitraryVerified};
  const final_writer_storage_transaction_v1::Layout storage_layout{
      result.payload.shadow, result.payload.control, result.payload.targets,
      result.payload.audits, result.payload.evidence};
  if (final_writer_storage_transaction_v1::Stage(
          storage_backend, storage_layout, result.prepared,
          result.targets.data(), frame_count) !=
      final_writer_storage_transaction_v1::Result::kOk)
    return false;
  result.configured = true;
  *runtime = std::move(result);
  return true;
}

inline bool ReadEvidence(int mem, const Runtime& runtime, Evidence* output) {
  return output != nullptr && runtime.configured &&
         ReadExact(mem, runtime.payload.evidence, output, sizeof(*output));
}

inline bool ResetUninstalled(int mem, Runtime* runtime) {
  using namespace final_writer_replay_v1;
  if (runtime == nullptr || !runtime->configured || runtime->installed)
    return false;
  const Control& prepared_control = runtime->prepared.unpublished_control;
  std::uintptr_t current = 0;
  if (!ReadExact(mem, prepared_control.expected_object, &current,
                 sizeof(current)) ||
      current != prepared_control.original_vptr)
    return false;
  Control fresh_control{};
  std::memcpy(fresh_control.magic, kControlMagic, 8);
  fresh_control.version = kProtocolVersion;
  fresh_control.size = sizeof(Control);
  Evidence fresh_evidence{};
  std::memcpy(fresh_evidence.magic, kEvidenceMagic, 8);
  fresh_evidence.version = kProtocolVersion;
  fresh_evidence.size = sizeof(Evidence);
  fresh_evidence.last_status = kStatusPassive;
  return WriteArbitraryVerified(const_cast<int*>(&mem), runtime->payload.evidence,
                                &fresh_evidence, sizeof(fresh_evidence)) &&
         WriteArbitraryVerified(const_cast<int*>(&mem), runtime->payload.control,
                                &fresh_control, sizeof(fresh_control));
}

#ifndef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
inline bool BeginTick(int mem, std::uint32_t external_index,
                      Runtime* runtime) {
  Evidence evidence{};
  return runtime != nullptr && ReadEvidence(mem, *runtime, &evidence) &&
         runtime->binding.BeginTick(external_index, evidence) ==
             final_writer_cursor_binding_v1::Result::kOk;
}
#endif

#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
inline bool BeginTick(int mem, std::uint32_t external_index,
                      const RecordingFrameV1& frame, Runtime* runtime) {
  Evidence evidence{};
  return runtime != nullptr && runtime->natural_runtime.has_value() &&
         ReadEvidence(mem, *runtime, &evidence) &&
         runtime->natural_runtime->BeginAndPublish(
             NaturalBackend(&mem), external_index, frame, evidence) ==
             final_writer_natural_action_runtime_v1::Result::kOk;
}
#endif

inline bool ArmWriterForTick(int mem, std::uint32_t external_index,
                             Runtime* runtime) {
  using namespace final_writer_replay_v1;
  if (runtime == nullptr || !runtime->configured || !runtime->installed ||
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
      !runtime->natural_runtime.has_value() ||
      runtime->natural_runtime->binding().writer_binding().phase() !=
          final_writer_cursor_binding_v1::Phase::kAwaitingWriter ||
      runtime->natural_runtime->next_index() != external_index ||
#else
      runtime->binding.phase() !=
          final_writer_cursor_binding_v1::Phase::kAwaitingWriter ||
      runtime->binding.next_index() != external_index ||
#endif
      external_index >= runtime->prepared.published_control.frame_count)
    return false;
  Control live_control{};
  const Control& expected = runtime->prepared.published_control;
  if (!ReadExact(mem, runtime->payload.control, &live_control,
                 sizeof(live_control)) ||
      std::memcmp(&live_control, &expected, sizeof(live_control)) != 0)
    return false;
  const std::uint64_t permit = FramePermit(external_index);
  return WriteArbitraryVerified(
      const_cast<int*>(&mem),
      runtime->payload.control + offsetof(Control, reserved), &permit,
      sizeof(permit));
}

inline bool FreezeOthers(pid_t pid, pid_t owner, std::uintptr_t delta,
                         std::uintptr_t c98, std::uintptr_t c9c,
                         std::uintptr_t world_accumulator,
                         std::vector<TracedThread>* threads,
                         std::uint64_t* additions) {
  if (threads == nullptr || additions == nullptr) return false;
  for (int pass = 0; pass < 6; ++pass) {
    for (auto& thread : *threads) {
      if (!thread.live || thread.tid == owner || thread.stopped) continue;
      if (!StopThread(thread.tid)) return false;
      thread.stopped = true;
    }
    std::uint64_t failures = 0;
    const std::size_t added = AttachNewThreadsStopped(
        pid, delta, c98, c9c, world_accumulator, threads, &failures,
        BoundaryDr7(true));
    *additions += added;
    if (failures != 0) return false;
    if (added == 0) return true;
  }
  return false;
}

inline bool InstallAtCertifiedPrefix(
    pid_t pid, int mem, pid_t owner, std::uintptr_t delta,
    std::uintptr_t c98, std::uintptr_t c9c,
    std::uintptr_t world_accumulator, std::uint32_t external_index,
    std::vector<TracedThread>* threads, std::uint64_t* additions,
    Runtime* runtime) {
  if (runtime == nullptr || !runtime->configured || runtime->installed ||
      external_index != 0 ||
      !FreezeOthers(pid, owner, delta, c98, c9c, world_accumulator,
                    threads, additions))
    return false;
  std::uintptr_t current = 0;
  if (!ReadExact(mem, runtime->prepared.unpublished_control.expected_object,
                 &current, sizeof(current)) ||
      current != runtime->prepared.unpublished_control.original_vptr ||
      !WriteArbitraryVerified(
          const_cast<int*>(&mem),
          runtime->prepared.unpublished_control.expected_object,
          &runtime->prepared.shadow_vptr, sizeof(runtime->prepared.shadow_vptr)))
    return false;
  runtime->installed = true;
  runtime->first_frame_others_frozen = true;
  return true;
}

// Original AluTasV2 installs its detour before replay begins.  The Android
// gate mirrors that lifetime: while the game is still paused, stop every
// traced game thread, install the already-staged shadow vptr once, and keep
// all threads frozen until the host has queued the single resume input.
inline PrearmInstallResult InstallBeforeResume(
    pid_t pid, int mem, std::uintptr_t delta, std::uintptr_t c98,
    std::uintptr_t c9c, std::uintptr_t world_accumulator,
    std::vector<TracedThread>* threads, std::uint64_t* additions,
    Runtime* runtime) {
  if (runtime == nullptr || threads == nullptr || additions == nullptr ||
      !runtime->configured || runtime->installed)
    return PrearmInstallResult::kInvalidRuntime;
  bool stable = false;
  for (int pass = 0; pass < 4; ++pass) {
    for (auto& thread : *threads) {
      if (!thread.live || thread.stopped) continue;
      if (!StopThread(thread.tid))
        return PrearmInstallResult::kStopTrackedThread;
      thread.stopped = true;
    }
    std::uint64_t failures = 0;
    const std::size_t added = AttachNewThreadsStopped(
        pid, delta, c98, c9c, world_accumulator, threads, &failures,
        BoundaryDr7(true));
    *additions += added;
    if (failures != 0) return PrearmInstallResult::kAttachNewThread;
    if (added == 0) {
      stable = true;
      break;
    }
  }
  if (!stable) return PrearmInstallResult::kThreadSetDidNotStabilize;
  std::uintptr_t current = 0;
  if (!ReadExact(mem, runtime->prepared.unpublished_control.expected_object,
                 &current, sizeof(current)))
    return PrearmInstallResult::kReadObjectVptr;
  if (current != runtime->prepared.unpublished_control.original_vptr)
    return PrearmInstallResult::kUnexpectedObjectVptr;
  if (!WriteArbitraryVerified(
          const_cast<int*>(&mem),
          runtime->prepared.unpublished_control.expected_object,
          &runtime->prepared.shadow_vptr,
          sizeof(runtime->prepared.shadow_vptr)))
    return PrearmInstallResult::kWriteShadowVptr;
  runtime->installed = true;
  runtime->prearm_all_threads_frozen = true;
  return PrearmInstallResult::kOk;
}

inline bool ResumeAfterPrearm(std::vector<TracedThread>* threads,
                              Runtime* runtime) {
  if (runtime == nullptr || threads == nullptr ||
      !runtime->prearm_all_threads_frozen)
    return false;
  for (auto& thread : *threads) {
    if (!thread.live || !thread.stopped) continue;
    if (!ContinueThread(thread.tid)) return false;
    thread.stopped = false;
  }
  runtime->prearm_all_threads_frozen = false;
  return true;
}

inline bool ResumeFrozenOthers(pid_t owner,
                               std::vector<TracedThread>* threads,
                               Runtime* runtime) {
  if (runtime == nullptr || threads == nullptr ||
      !runtime->first_frame_others_frozen)
    return false;
  for (auto& thread : *threads) {
    if (!thread.live || thread.tid == owner || !thread.stopped) continue;
    if (!ContinueThread(thread.tid)) return false;
    thread.stopped = false;
  }
  runtime->first_frame_others_frozen = false;
  return true;
}

inline bool AcknowledgeAtCallbackClose(
    int mem, std::uint32_t external_index, const RecordingFrameV1& frame,
    FrameAudit* payload_audit, Evidence* evidence, Runtime* runtime) {
  using namespace final_writer_replay_v1;
  Control live_control{};
  if (runtime == nullptr || payload_audit == nullptr || evidence == nullptr ||
      !ReadExact(mem, runtime->payload.control, &live_control,
                 sizeof(live_control)) ||
      std::memcmp(&live_control, &runtime->prepared.published_control,
                  sizeof(live_control)) != 0 ||
      !ReadExact(mem,
                 runtime->payload.audits +
                     static_cast<std::size_t>(external_index) *
                         sizeof(FrameAudit),
                 payload_audit, sizeof(*payload_audit)) ||
      !ReadEvidence(mem, *runtime, evidence))
    return false;
  const std::uint32_t required = kAuditOriginalReturned | kAuditImmediateExact;
  const bool exactly_one_result =
      ((payload_audit->flags & kAuditEqual) != 0) !=
      ((payload_audit->flags & kAuditCorrected) != 0);
  const bool receipts_valid = payload_audit->frame_index == external_index &&
         (payload_audit->flags & required) == required && exactly_one_result &&
         std::memcmp(payload_audit->immediate_transform,
                     frame.transform_bits, kTransformSize) == 0 &&
         std::memcmp(payload_audit->immediate_linear,
                     frame.linear_velocity_bits, kLinearSize) == 0;
  if (!receipts_valid) return false;
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
  return runtime->natural_runtime.has_value() &&
         runtime->natural_runtime->ObserveCallbackClose(
             NaturalBackend(&mem), external_index, *evidence) ==
             final_writer_natural_action_runtime_v1::Result::kOk;
#else
  return
         runtime->binding.AcknowledgeWriter(external_index, *evidence) ==
             final_writer_cursor_binding_v1::Result::kOk;
#endif
}

inline bool CommitAtWorldBoundary(int mem, std::uint32_t external_index,
                                  Runtime* runtime) {
  Evidence evidence{};
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
  return runtime != nullptr && runtime->natural_runtime.has_value() &&
         ReadEvidence(mem, *runtime, &evidence) &&
         runtime->natural_runtime->CommitWorld(external_index, evidence) ==
             final_writer_natural_action_runtime_v1::Result::kOk;
#else
  return runtime != nullptr && ReadEvidence(mem, *runtime, &evidence) &&
         runtime->binding.CommitTick(external_index, evidence) ==
             final_writer_cursor_binding_v1::Result::kOk;
#endif
}

inline bool ConditionalRollback(int mem, Runtime* runtime) {
  if (runtime == nullptr || !runtime->configured) return false;
  using namespace final_writer_replay_v1;
  const Control& control = runtime->prepared.published_control;
  Control live_control{};
  bool permit_ok =
      ReadExact(mem, runtime->payload.control, &live_control,
                sizeof(live_control));
  if (permit_ok) {
    const std::uint64_t live_permit = live_control.reserved[0];
    live_control.reserved[0] = kFramePermitDisarmed;
    permit_ok =
        live_permit <= FramePermit(control.frame_count - 1u) &&
        std::memcmp(&live_control, &control, sizeof(live_control)) == 0;
    if (permit_ok && live_permit != kFramePermitDisarmed) {
      const std::uint64_t disarmed = kFramePermitDisarmed;
      permit_ok = WriteArbitraryVerified(
          const_cast<int*>(&mem),
          runtime->payload.control + offsetof(Control, reserved), &disarmed,
          sizeof(disarmed));
    }
  }
  std::uintptr_t current = 0;
  if (!ReadExact(mem, control.expected_object, &current, sizeof(current)))
    return false;
  if (current == control.original_vptr) return permit_ok;
  if (current != control.shadow_vptr) return false;
  return WriteArbitraryVerified(const_cast<int*>(&mem), control.expected_object,
                                &control.original_vptr,
                                sizeof(control.original_vptr)) &&
         permit_ok;
}

inline bool ValidateFinal(int mem, Runtime* runtime) {
  if (runtime == nullptr || !runtime->configured ||
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
      !runtime->natural_runtime.has_value() ||
      !runtime->natural_runtime->complete())
#else
      runtime->binding.phase() != final_writer_cursor_binding_v1::Phase::kComplete)
#endif
    return false;
  Evidence evidence{};
  Control live_control{};
  std::uintptr_t current = 0;
  const Control& control = runtime->prepared.published_control;
  return ReadEvidence(mem, *runtime, &evidence) &&
         ReadExact(mem, runtime->payload.control, &live_control,
                   sizeof(live_control)) &&
         std::memcmp(&live_control, &control, sizeof(live_control)) == 0 &&
         final_writer_transaction_core_v1::EvidenceComplete(evidence, control) &&
         ReadExact(mem, control.expected_object, &current, sizeof(current)) &&
         current == control.original_vptr;
}

inline bool WritePayloadReport(pid_t pid, std::uintptr_t library_base, int mem,
                               Runtime* runtime) {
  using namespace final_writer_replay_v1;
  if (runtime == nullptr ||
      g_a9tas_final_writer_payload_report_path_v1 == nullptr ||
      !ValidateFinal(mem, runtime))
    return false;
  const std::uint32_t frame_count = runtime->prepared.published_control.frame_count;
  std::vector<FrameAudit> audits(frame_count);
  Evidence evidence{};
  if (!ReadExact(mem, runtime->payload.audits, audits.data(),
                 audits.size() * sizeof(FrameAudit)) ||
      !ReadEvidence(mem, *runtime, &evidence))
    return false;
  for (std::uint32_t index = 0; index < frame_count; ++index) {
    const std::uint32_t required = kAuditOriginalReturned | kAuditImmediateExact;
    const bool exactly_one = ((audits[index].flags & kAuditEqual) != 0) !=
                             ((audits[index].flags & kAuditCorrected) != 0);
    if (audits[index].frame_index != index ||
        (audits[index].flags & required) != required || !exactly_one)
      return false;
  }
  // Value-initialization does not guarantee canonical bytes for the trailing
  // padding inside the alignas(64) Evidence member.  The binary report ABI
  // requires every reserved/padding byte to be zero, so clear the complete
  // packed envelope before assigning its logical fields.
  PayloadReportHeader header;
  std::memset(&header, 0, sizeof(header));
  std::memcpy(header.magic, kPayloadReportMagic, 8);
  header.version = 1;
  header.header_size = sizeof(header);
  header.audit_size = sizeof(FrameAudit);
  header.flags = kPayloadReportFlags;
  header.frame_count = frame_count;
  header.pid = static_cast<std::uint64_t>(pid);
  header.library_base = library_base;
  header.object = runtime->prepared.published_control.expected_object;
  header.wrapper = runtime->payload.wrapper;
  header.shadow_vptr = runtime->prepared.published_control.shadow_vptr;
  header.original_vptr = runtime->prepared.published_control.original_vptr;
  header.control_address = runtime->payload.control;
  header.target_address = runtime->payload.targets;
  header.audit_address = runtime->payload.audits;
  header.evidence_address = runtime->payload.evidence;
  std::memcpy(header.recording_sha256,
              runtime->prepared.published_control.recording_sha256, 32);
  std::memcpy(header.payload_sha256, runtime->payload.file_sha256, 32);
  header.evidence = evidence;
  const int fd = open(g_a9tas_final_writer_payload_report_path_v1,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  FILE* file = fdopen(fd, "wb");
  if (!file) {
    close(fd);
    std::remove(g_a9tas_final_writer_payload_report_path_v1);
    return false;
  }
  const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                  std::fwrite(audits.data(), sizeof(FrameAudit), audits.size(),
                              file) == audits.size() &&
                  std::fflush(file) == 0 && std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    std::remove(g_a9tas_final_writer_payload_report_path_v1);
    return false;
  }
  return true;
}

}  // namespace a9tas::final_writer_unified_v1
