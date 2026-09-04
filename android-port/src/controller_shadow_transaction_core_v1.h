#pragma once

// Pure host-side transaction for the controller-shadow coordinator.
//
// Prepare is read-only. Stage writes payload-owned storage only and publishes
// the payload control flags last. Commit revalidates every live identity and
// performs the sole game-object mutation: one controller-vptr write. Rollback
// is conditional and refuses to overwrite an unknown third-party vptr.
//
// The caller must freeze a stable target thread set and pin the process
// generation before Stage/Commit/Rollback. This core has no process discovery,
// ptrace, thread control, ELF parsing or device access.

#include "controller_shadow_coordinator_protocol_v1.h"
#include "final_writer_replay_protocol_v1.h"
#include "natural_action_replay_transport_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::controller_shadow_transaction_core_v1 {

namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace writer = a9tas::final_writer_replay_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace action = a9tas::natural_action_replay_v1;
namespace recording = a9tas::unified_tick_v1;

using ReadFn = bool (*)(void*, std::uintptr_t, void*, std::size_t);
using WriteVerifiedFn = bool (*)(void*, std::uintptr_t, const void*,
                                std::size_t);

struct Backend {
  void* context{};
  ReadFn read{};
  WriteVerifiedFn write_verified{};
};

struct Guard {
  bool process_generation_pinned{};
  bool unique_controller_resolved{};
  bool all_target_threads_frozen{};
  bool payload_hash_verified{};
  bool game_build_id_verified{};
};

struct PayloadLayout {
  std::uintptr_t wrapper{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t frames{};
  std::uint64_t shadow_size{};
  std::uint64_t control_size{};
  std::uint64_t evidence_size{};
  std::uint64_t frame_size{};
  std::uint64_t frame_capacity{};
};

struct RuntimeLayout {
  std::uintptr_t game_base{};
  std::uintptr_t controller{};
  std::uintptr_t writer_control{};
  std::uintptr_t writer_evidence{};
  std::uintptr_t action_mailbox{};
  std::uintptr_t vehicle_owner{};
  std::uint32_t session_id{};
  std::uint32_t producer_tid{};
};

struct Prepared {
  std::array<std::uint8_t, payload::kControllerShadowSize> shadow{};
  payload::Control unpublished_control{};
  payload::Control published_control{};
  payload::Evidence fresh_evidence{};
  std::uintptr_t original_controller_vptr{};
  std::uintptr_t shadow_controller_vptr{};
  std::uintptr_t original_source{};
  std::uintptr_t original_source_vptr{};
  std::uint32_t frame_count{};
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kGuardRejected = -2,
  kPayloadLayoutInvalid = -3,
  kGameVtableReadFailed = -4,
  kGameVtableMismatch = -5,
  kControllerIdentityMismatch = -6,
  kSourceIdentityMismatch = -7,
  kPayloadNotFresh = -8,
  kWriterNotReady = -9,
  kMailboxNotReady = -10,
  kRecordingRejected = -11,
  kDisableControlWriteFailed = -12,
  kEvidenceWriteFailed = -13,
  kFramesWriteFailed = -14,
  kShadowWriteFailed = -15,
  kPublishControlWriteFailed = -16,
  kCommitRevalidationFailed = -17,
  kVptrCommitFailed = -18,
  kRollbackForeignVptr = -19,
  kRollbackVptrFailed = -20,
  kRollbackDisableFailed = -21,
};

inline constexpr std::uint64_t kExpectedPrefixWords[4] = {
    0x260, 0x118, 0, 0,
};

inline constexpr std::uintptr_t kExpectedPrimarySlotRvas[28] = {
    0x386BA24, 0x386BA6C, 0x386AB60, 0x386AC18, 0x386AC50,
    0x386ACE8, 0x386B92C, 0x386B970, 0x386AD04, 0x386B1C4,
    0x386B154, 0x386B170, 0x386B18C, 0x386B1A8, 0x386B1E0,
    0x386B3D8, 0x386B490, 0x386B4AC, 0x386B4C8, 0x386B544,
    0x386B5AC, 0x386B5E4, 0x386B61C, 0x386B654, 0x386B670,
    0x386B68C, 0x386B6A8, 0x386B6C4,
};

static_assert(sizeof(kExpectedPrefixWords) == payload::kControllerPrefixSize,
              "controller prefix proof size");
static_assert(sizeof(kExpectedPrimarySlotRvas) ==
                  payload::kControllerPrimaryTableSize,
              "controller slot proof size");
static_assert(kExpectedPrimarySlotRvas[payload::kControllerUpdateSlotIndex] ==
                  payload::kControllerUpdateRva,
              "UpdatePerTick slot identity");

inline bool Add(std::uintptr_t base, std::uintptr_t offset,
                std::uintptr_t* output) noexcept {
  if (output == nullptr || base > UINTPTR_MAX - offset) return false;
  *output = base + offset;
  return true;
}

inline bool Span(std::uintptr_t begin, std::size_t size,
                 std::uintptr_t* end) noexcept {
  return begin != 0 && size != 0 && end != nullptr &&
         begin <= UINTPTR_MAX - size && (*end = begin + size, true);
}

inline bool BackendValid(const Backend& backend) noexcept {
  return backend.context != nullptr && backend.read != nullptr &&
         backend.write_verified != nullptr;
}

inline bool GuardValid(const Guard& guard) noexcept {
  return guard.process_generation_pinned && guard.unique_controller_resolved &&
         guard.all_target_threads_frozen && guard.payload_hash_verified &&
         guard.game_build_id_verified;
}

inline bool PayloadLayoutValid(const PayloadLayout& layout,
                               std::uint32_t frame_count) noexcept {
  if ((layout.wrapper & 3u) != 0 || (layout.shadow & 63u) != 0 ||
      (layout.control & 63u) != 0 || (layout.evidence & 63u) != 0 ||
      (layout.frames & 63u) != 0 ||
      layout.shadow_size != payload::kControllerShadowSize ||
      layout.control_size != sizeof(payload::Control) ||
      layout.evidence_size != sizeof(payload::Evidence) ||
      layout.frame_size != sizeof(recording::RecordingFrameV1) ||
      layout.frame_capacity != payload::kMaximumFrames || frame_count == 0 ||
      frame_count > layout.frame_capacity)
    return false;
  struct Range {
    std::uintptr_t begin;
    std::uintptr_t end;
  } ranges[4]{};
  const std::uintptr_t begins[4] = {
      layout.shadow, layout.control, layout.evidence, layout.frames,
  };
  const std::size_t sizes[4] = {
      payload::kControllerShadowSize, sizeof(payload::Control),
      sizeof(payload::Evidence),
      static_cast<std::size_t>(frame_count) *
          sizeof(recording::RecordingFrameV1),
  };
  for (std::size_t index = 0; index < 4; ++index) {
    ranges[index].begin = begins[index];
    if (!Span(begins[index], sizes[index], &ranges[index].end)) return false;
  }
  for (std::size_t left = 0; left < 4; ++left)
    for (std::size_t right = left + 1; right < 4; ++right)
      if (ranges[left].begin < ranges[right].end &&
          ranges[right].begin < ranges[left].end)
        return false;
  return true;
}

inline bool FactoryControl(const payload::Control& control) noexcept {
  payload::Control expected{};
  std::memcpy(expected.magic, payload::kControlMagic,
              sizeof(payload::kControlMagic));
  expected.version = payload::kProtocolVersion;
  expected.size = sizeof(payload::Control);
  return std::memcmp(&control, &expected, sizeof(control)) == 0;
}

inline bool FactoryEvidence(const payload::Evidence& evidence) noexcept {
  payload::Evidence expected{};
  std::memcpy(expected.magic, payload::kEvidenceMagic,
              sizeof(payload::kEvidenceMagic));
  expected.version = payload::kProtocolVersion;
  expected.size = sizeof(payload::Evidence);
  return std::memcmp(&evidence, &expected, sizeof(evidence)) == 0;
}

inline bool WriterReady(const writer::Control& control,
                        const writer::Evidence& evidence,
                        const RuntimeLayout& runtime,
                        std::uint32_t frame_count) noexcept {
  return std::memcmp(control.magic, writer::kControlMagic,
                     sizeof(writer::kControlMagic)) == 0 &&
         control.version == writer::kProtocolVersion &&
         control.size == sizeof(writer::Control) &&
         control.flags ==
             (writer::kControlConfigured | writer::kControlTargetsLoaded) &&
         control.frame_count == frame_count &&
         control.expected_object == runtime.vehicle_owner &&
         control.original_vptr != 0 && control.shadow_vptr != 0 &&
         control.original_callback != 0 && control.native_pose != 0 &&
         control.native_linear != 0 &&
         __atomic_load_n(&control.reserved[0], __ATOMIC_ACQUIRE) ==
             writer::kFramePermitDisarmed &&
         std::memcmp(evidence.magic, writer::kEvidenceMagic,
                     sizeof(writer::kEvidenceMagic)) == 0 &&
         evidence.version == writer::kProtocolVersion &&
         evidence.size == sizeof(writer::Evidence) &&
         evidence.processed_frames == 0 && evidence.failures == 0 &&
         evidence.recursive_entries == 0;
}

inline bool MailboxReady(const mailbox::Mailbox& snapshot,
                         const RuntimeLayout& runtime) noexcept {
  const std::uint64_t session = mailbox::LoadAcquire(
      &snapshot.session_control);
  const std::int32_t result =
      __atomic_load_n(&snapshot.result, __ATOMIC_ACQUIRE);
  return mailbox::MailboxValid(snapshot) && mailbox::Armed(session) &&
         mailbox::SessionId(session) == runtime.session_id &&
         mailbox::LoadAcquire(&snapshot.published_selector) == 0 &&
         mailbox::LoadAcquire(&snapshot.claimed_sequence) == 0 &&
         mailbox::LoadAcquire(&snapshot.completed_sequence) == 0 &&
         (result == static_cast<std::int32_t>(mailbox::Result::kIdle) ||
          result == static_cast<std::int32_t>(mailbox::Result::kPassive));
}

inline bool RecordingValid(const recording::RecordingFrameV1* frames,
                           std::uint32_t frame_count,
                           const RuntimeLayout& runtime) noexcept {
  if (frames == nullptr) return false;
  for (std::uint32_t index = 0; index < frame_count; ++index) {
    if (!payload::FrameInputSupported(frames[index], index)) return false;
    mailbox::Command command{};
    if (!action::BuildFrameCommand(
            frames[index], static_cast<std::uint64_t>(index) + 1u,
            runtime.session_id, runtime.vehicle_owner, runtime.producer_tid,
            &command))
      return false;
  }
  return true;
}

inline bool HashPresent(const std::uint8_t hash[32]) noexcept {
  std::uint8_t combined = 0;
  for (std::size_t index = 0; index < 32; ++index) combined |= hash[index];
  return combined != 0;
}

inline Result Prepare(const Backend& backend, const Guard& guard,
                      const PayloadLayout& payload_layout,
                      const RuntimeLayout& runtime,
                      const recording::RecordingFrameV1* frames,
                      std::uint32_t frame_count,
                      const std::uint8_t recording_sha256[32],
                      Prepared* output) noexcept {
  if (!BackendValid(backend) || output == nullptr || runtime.game_base == 0 ||
      runtime.controller == 0 || runtime.writer_control == 0 ||
      runtime.writer_evidence == 0 || runtime.action_mailbox == 0 ||
      runtime.vehicle_owner == 0 || runtime.session_id == 0 ||
      runtime.producer_tid == 0 || recording_sha256 == nullptr ||
      !HashPresent(recording_sha256))
    return Result::kInvalidArgument;
  if (!GuardValid(guard)) return Result::kGuardRejected;
  if (!PayloadLayoutValid(payload_layout, frame_count))
    return Result::kPayloadLayoutInvalid;
  if (!RecordingValid(frames, frame_count, runtime))
    return Result::kRecordingRejected;

  std::uintptr_t game_vtable = 0;
  if (!Add(runtime.game_base, payload::kControllerPrefixRva, &game_vtable) ||
      !backend.read(backend.context, game_vtable, output->shadow.data(),
                    output->shadow.size()))
    return Result::kGameVtableReadFailed;
  if (std::memcmp(output->shadow.data(), kExpectedPrefixWords,
                  sizeof(kExpectedPrefixWords)) != 0)
    return Result::kGameVtableMismatch;
  for (std::size_t index = 0; index < std::size(kExpectedPrimarySlotRvas);
       ++index) {
    std::uintptr_t observed = 0;
    std::memcpy(&observed,
                output->shadow.data() + payload::kControllerPrefixSize +
                    index * sizeof(observed),
                sizeof(observed));
    std::uintptr_t expected = 0;
    if (!Add(runtime.game_base, kExpectedPrimarySlotRvas[index], &expected) ||
        observed != expected)
      return Result::kGameVtableMismatch;
  }

  std::uintptr_t expected_controller_vptr = 0;
  std::uintptr_t expected_source_vptr = 0;
  std::uintptr_t expected_update = 0;
  if (!Add(runtime.game_base, payload::kControllerAddressPointRva,
           &expected_controller_vptr) ||
      !Add(runtime.game_base, payload::kKeyboardSourceAddressPointRva,
           &expected_source_vptr) ||
      !Add(runtime.game_base, payload::kControllerUpdateRva,
           &expected_update))
    return Result::kInvalidArgument;
  std::uintptr_t observed_controller_vptr = 0;
  std::uintptr_t source_slot = 0;
  std::uintptr_t observed_source = 0;
  std::uintptr_t observed_source_vptr = 0;
  if (!Add(runtime.controller, payload::kControllerSourceOffset,
           &source_slot) ||
      !backend.read(backend.context, runtime.controller,
                    &observed_controller_vptr,
                    sizeof(observed_controller_vptr)) ||
      observed_controller_vptr != expected_controller_vptr)
    return Result::kControllerIdentityMismatch;
  if (!backend.read(backend.context, source_slot, &observed_source,
                    sizeof(observed_source)) ||
      observed_source == 0 ||
      !backend.read(backend.context, observed_source,
                    &observed_source_vptr, sizeof(observed_source_vptr)) ||
      observed_source_vptr != expected_source_vptr)
    return Result::kSourceIdentityMismatch;

  payload::Control initial_control{};
  payload::Evidence initial_evidence{};
  std::array<std::uint8_t, payload::kControllerShadowSize> initial_shadow{};
  if (!backend.read(backend.context, payload_layout.control, &initial_control,
                    sizeof(initial_control)) ||
      !backend.read(backend.context, payload_layout.evidence,
                    &initial_evidence, sizeof(initial_evidence)) ||
      !backend.read(backend.context, payload_layout.shadow,
                    initial_shadow.data(), initial_shadow.size()) ||
      !FactoryControl(initial_control) || !FactoryEvidence(initial_evidence) ||
      initial_shadow[0] != 0xA9)
    return Result::kPayloadNotFresh;
  for (std::size_t index = 1; index < initial_shadow.size(); ++index)
    if (initial_shadow[index] != 0) return Result::kPayloadNotFresh;

  writer::Control writer_control{};
  writer::Evidence writer_evidence{};
  if (!backend.read(backend.context, runtime.writer_control, &writer_control,
                    sizeof(writer_control)) ||
      !backend.read(backend.context, runtime.writer_evidence, &writer_evidence,
                    sizeof(writer_evidence)) ||
      !WriterReady(writer_control, writer_evidence, runtime, frame_count))
    return Result::kWriterNotReady;
  mailbox::Mailbox mailbox_snapshot{};
  if (!backend.read(backend.context, runtime.action_mailbox, &mailbox_snapshot,
                    sizeof(mailbox_snapshot)) ||
      !MailboxReady(mailbox_snapshot, runtime))
    return Result::kMailboxNotReady;

  const std::uintptr_t shadow_vptr =
      payload_layout.shadow + payload::kControllerPrefixSize;
  std::memcpy(output->shadow.data() + payload::kControllerPrefixSize +
                  payload::kControllerUpdateSlotOffset,
              &payload_layout.wrapper, sizeof(payload_layout.wrapper));

  payload::Control unpublished{};
  std::memcpy(unpublished.magic, payload::kControlMagic,
              sizeof(payload::kControlMagic));
  unpublished.version = payload::kProtocolVersion;
  unpublished.size = sizeof(unpublished);
  unpublished.flags = 0;
  unpublished.frame_count = frame_count;
  unpublished.expected_controller = runtime.controller;
  unpublished.original_controller_vptr = expected_controller_vptr;
  unpublished.shadow_controller_vptr = shadow_vptr;
  unpublished.original_update = expected_update;
  unpublished.expected_source_vptr = expected_source_vptr;
  unpublished.writer_control = runtime.writer_control;
  unpublished.writer_evidence = runtime.writer_evidence;
  unpublished.action_mailbox = runtime.action_mailbox;
  unpublished.vehicle_owner = runtime.vehicle_owner;
  unpublished.session_id = runtime.session_id;
  unpublished.producer_tid = runtime.producer_tid;
  std::memcpy(unpublished.recording_sha256, recording_sha256, 32);
  payload::Control published = unpublished;
  published.flags = payload::kConfigured | payload::kFramesLoaded;

  payload::Evidence fresh{};
  std::memcpy(fresh.magic, payload::kEvidenceMagic,
              sizeof(payload::kEvidenceMagic));
  fresh.version = payload::kProtocolVersion;
  fresh.size = sizeof(fresh);

  output->unpublished_control = unpublished;
  output->published_control = published;
  output->fresh_evidence = fresh;
  output->original_controller_vptr = expected_controller_vptr;
  output->shadow_controller_vptr = shadow_vptr;
  output->original_source = observed_source;
  output->original_source_vptr = observed_source_vptr;
  output->frame_count = frame_count;
  return Result::kOk;
}

inline Result Stage(const Backend& backend, const Guard& guard,
                    const PayloadLayout& layout, const Prepared& prepared,
                    const RuntimeLayout& runtime,
                    const recording::RecordingFrameV1* frames) noexcept {
  if (!BackendValid(backend) || frames == nullptr || !GuardValid(guard) ||
      !PayloadLayoutValid(layout, prepared.frame_count) ||
      !RecordingValid(frames, prepared.frame_count, runtime) ||
      prepared.unpublished_control.flags != 0 ||
      prepared.published_control.flags !=
          (payload::kConfigured | payload::kFramesLoaded) ||
      prepared.unpublished_control.frame_count != prepared.frame_count ||
      prepared.published_control.frame_count != prepared.frame_count ||
      prepared.shadow_controller_vptr !=
          layout.shadow + payload::kControllerPrefixSize)
    return Result::kInvalidArgument;
  const auto write = [&](std::uintptr_t address, const void* bytes,
                         std::size_t size) {
    return backend.write_verified(backend.context, address, bytes, size);
  };
  if (!write(layout.control, &prepared.unpublished_control,
             sizeof(prepared.unpublished_control)))
    return Result::kDisableControlWriteFailed;
  if (!write(layout.evidence, &prepared.fresh_evidence,
             sizeof(prepared.fresh_evidence)))
    return Result::kEvidenceWriteFailed;
  if (!write(layout.frames, frames,
             static_cast<std::size_t>(prepared.frame_count) *
                 sizeof(recording::RecordingFrameV1)))
    return Result::kFramesWriteFailed;
  if (!write(layout.shadow, prepared.shadow.data(), prepared.shadow.size()))
    return Result::kShadowWriteFailed;
  if (!write(layout.control, &prepared.published_control,
             sizeof(prepared.published_control))) {
    if (!write(layout.control, &prepared.unpublished_control,
               sizeof(prepared.unpublished_control)))
      return Result::kRollbackDisableFailed;
    return Result::kPublishControlWriteFailed;
  }
  return Result::kOk;
}

inline bool RevalidateCommit(const Backend& backend,
                             const PayloadLayout& layout,
                             const RuntimeLayout& runtime,
                             const Prepared& prepared,
                             const recording::RecordingFrameV1* frames) noexcept {
  if (frames == nullptr || !PayloadLayoutValid(layout, prepared.frame_count))
    return false;
  if (!RecordingValid(frames, prepared.frame_count, runtime)) return false;
  std::uintptr_t controller_vptr = 0;
  std::uintptr_t source_slot = 0;
  std::uintptr_t source = 0;
  std::uintptr_t source_vptr = 0;
  payload::Control staged_control{};
  payload::Evidence staged_evidence{};
  std::array<std::uint8_t, payload::kControllerShadowSize> staged_shadow{};
  writer::Control writer_control{};
  writer::Evidence writer_evidence{};
  mailbox::Mailbox mailbox_snapshot{};
  if (!Add(runtime.controller, payload::kControllerSourceOffset,
           &source_slot) ||
      !backend.read(backend.context, layout.control, &staged_control,
                    sizeof(staged_control)) ||
      std::memcmp(&staged_control, &prepared.published_control,
                  sizeof(staged_control)) != 0 ||
      !backend.read(backend.context, layout.evidence, &staged_evidence,
                    sizeof(staged_evidence)) ||
      std::memcmp(&staged_evidence, &prepared.fresh_evidence,
                  sizeof(staged_evidence)) != 0 ||
      !backend.read(backend.context, layout.shadow, staged_shadow.data(),
                    staged_shadow.size()) ||
      std::memcmp(staged_shadow.data(), prepared.shadow.data(),
                  staged_shadow.size()) != 0)
    return false;
  for (std::uint32_t index = 0; index < prepared.frame_count; ++index) {
    recording::RecordingFrameV1 staged_frame{};
    std::uintptr_t frame_address = 0;
    const std::uintptr_t frame_offset =
        static_cast<std::uintptr_t>(index) * sizeof(staged_frame);
    if (!Add(layout.frames, frame_offset, &frame_address) ||
        !backend.read(backend.context, frame_address, &staged_frame,
                      sizeof(staged_frame)) ||
        std::memcmp(&staged_frame, &frames[index], sizeof(staged_frame)) != 0)
      return false;
  }
  return
         backend.read(backend.context, runtime.controller, &controller_vptr,
                      sizeof(controller_vptr)) &&
         controller_vptr == prepared.original_controller_vptr &&
         backend.read(backend.context, source_slot, &source, sizeof(source)) &&
         source == prepared.original_source &&
         backend.read(backend.context, source, &source_vptr,
                      sizeof(source_vptr)) &&
         source_vptr == prepared.original_source_vptr &&
         backend.read(backend.context, runtime.writer_control, &writer_control,
                      sizeof(writer_control)) &&
         backend.read(backend.context, runtime.writer_evidence,
                      &writer_evidence, sizeof(writer_evidence)) &&
         WriterReady(writer_control, writer_evidence, runtime,
                     prepared.frame_count) &&
         backend.read(backend.context, runtime.action_mailbox,
                      &mailbox_snapshot, sizeof(mailbox_snapshot)) &&
         MailboxReady(mailbox_snapshot, runtime);
}

inline Result Commit(const Backend& backend, const Guard& guard,
                     const PayloadLayout& layout,
                     const RuntimeLayout& runtime,
                     const Prepared& prepared,
                     const recording::RecordingFrameV1* frames) noexcept {
  if (!BackendValid(backend) || !GuardValid(guard))
    return Result::kInvalidArgument;
  if (!RevalidateCommit(backend, layout, runtime, prepared, frames))
    return Result::kCommitRevalidationFailed;
  if (!backend.write_verified(backend.context, runtime.controller,
                              &prepared.shadow_controller_vptr,
                              sizeof(prepared.shadow_controller_vptr)))
    return Result::kVptrCommitFailed;
  return Result::kOk;
}

inline Result Rollback(const Backend& backend, const Guard& guard,
                       const PayloadLayout& layout,
                       const RuntimeLayout& runtime,
                       const Prepared& prepared) noexcept {
  if (!BackendValid(backend) || !GuardValid(guard))
    return Result::kInvalidArgument;
  std::uintptr_t observed = 0;
  if (!backend.read(backend.context, runtime.controller, &observed,
                    sizeof(observed)))
    return Result::kRollbackVptrFailed;
  if (observed != prepared.original_controller_vptr &&
      observed != prepared.shadow_controller_vptr)
    return Result::kRollbackForeignVptr;
  if (observed == prepared.shadow_controller_vptr &&
      !backend.write_verified(backend.context, runtime.controller,
                              &prepared.original_controller_vptr,
                              sizeof(prepared.original_controller_vptr)))
    return Result::kRollbackVptrFailed;
  if (!backend.write_verified(backend.context, layout.control,
                              &prepared.unpublished_control,
                              sizeof(prepared.unpublished_control)))
    return Result::kRollbackDisableFailed;
  return Result::kOk;
}

}  // namespace a9tas::controller_shadow_transaction_core_v1
