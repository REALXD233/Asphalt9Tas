#pragma once

// Host-side transaction for the passive natural Nitro recorder.  The caller
// must keep every game thread stopped while Stage() and Finish() run.

#include "natural_action_recording_elf_resolver_v1.h"
#include "natural_action_recording_protocol_v1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace a9tas::natural_action_recording_host_v1 {

namespace protocol = a9tas::natural_action_recording_v1;
namespace elf = a9tas::natural_action_recording_elf_v1;

inline constexpr std::uintptr_t kServiceOffset = 0xcb8;
inline constexpr std::uintptr_t kServiceVtableRva = 0x7ee8a90;
inline constexpr std::uintptr_t kActivateRva = 0x3674e50;

struct Runtime {
  elf::Layout payload{};
  std::uintptr_t service{};
  std::uintptr_t original_vptr{};
  std::uintptr_t original_activate{};
  std::uint32_t frame_count{};
  std::uint32_t session_id{};
  bool staged{};
  bool armed{};
  bool restored{};
};

struct Result {
  protocol::Evidence evidence{};
  std::vector<std::uint32_t> counts;
  std::uint64_t count_sum{};
  bool service_restored{};
  bool evidence_exact{};
};

inline bool ReadExact(int mem, std::uintptr_t address, void* output,
                      std::size_t size) {
  if (mem < 0 || address == 0 || output == nullptr || size == 0 ||
      address > UINTPTR_MAX - size)
    return false;
  std::size_t done = 0;
  while (done < size) {
    const ssize_t got = pread(mem, static_cast<std::uint8_t*>(output) + done,
                              size - done,
                              static_cast<off_t>(address + done));
    if (got <= 0) return false;
    done += static_cast<std::size_t>(got);
  }
  return true;
}

inline bool WriteExactVerified(int mem, std::uintptr_t address,
                               const void* input, std::size_t size) {
  if (mem < 0 || address == 0 || input == nullptr || size == 0 ||
      address > UINTPTR_MAX - size)
    return false;
  std::size_t done = 0;
  while (done < size) {
    const ssize_t put = pwrite(mem,
                               static_cast<const std::uint8_t*>(input) + done,
                               size - done,
                               static_cast<off_t>(address + done));
    if (put <= 0) return false;
    done += static_cast<std::size_t>(put);
  }
  std::vector<std::uint8_t> check(size);
  return ReadExact(mem, address, check.data(), check.size()) &&
         std::memcmp(check.data(), input, size) == 0;
}

inline bool ControlIdentity(const protocol::Control& control) {
  return std::memcmp(control.magic, protocol::kControlMagic,
                     sizeof(control.magic)) == 0 &&
         control.version == protocol::kVersion &&
         control.size == sizeof(protocol::Control);
}

inline bool EvidenceIdentity(const protocol::Evidence& evidence) {
  return std::memcmp(evidence.magic, protocol::kEvidenceMagic,
                     sizeof(evidence.magic)) == 0 &&
         evidence.version == protocol::kVersion &&
         evidence.size == sizeof(protocol::Evidence);
}

inline bool Resolve(int mem, pid_t pid, std::uintptr_t game_base,
                    std::uintptr_t final_owner, std::uint32_t frame_count,
                    std::uint32_t session_id, Runtime* output,
                    const char** failure_reason = nullptr) {
  if (failure_reason) *failure_reason = "invalid_arguments";
  const auto fail = [&](const char* reason) {
    if (failure_reason) *failure_reason = reason;
    return false;
  };
  if (output == nullptr || final_owner == 0 || game_base == 0 ||
      frame_count == 0 || frame_count > protocol::kMaximumFrames ||
      session_id == 0 ||
      final_owner > UINTPTR_MAX - kServiceOffset)
    return fail("invalid_arguments");
  Runtime runtime{};
  const char* elf_failure = nullptr;
  if (!elf::Resolve(pid, mem, &runtime.payload, &elf_failure))
    return fail(elf_failure ? elf_failure : "payload_elf");
  if (!ReadExact(mem, final_owner + kServiceOffset, &runtime.service,
                 sizeof(runtime.service)))
    return fail("service_pointer_read");
  if (runtime.service == 0 || (runtime.service & 7u) != 0)
    return fail("service_pointer_shape");
  if (!ReadExact(mem, runtime.service, &runtime.original_vptr,
                 sizeof(runtime.original_vptr)))
    return fail("service_vptr_read");
  if (runtime.original_vptr != game_base + kServiceVtableRva)
    return fail("service_vptr_identity");
  if (!ReadExact(mem,
                 runtime.original_vptr + protocol::kActivateSlotOffset,
                 &runtime.original_activate,
                 sizeof(runtime.original_activate)))
    return fail("activate_slot_read");
  if (runtime.original_activate != game_base + kActivateRva)
    return fail("activate_slot_identity");
  runtime.frame_count = frame_count;
  runtime.session_id = session_id;
  *output = runtime;
  if (failure_reason) *failure_reason = "none";
  return true;
}

inline protocol::Control MakeControl(const Runtime& runtime,
                                     std::uint32_t flags,
                                     std::uint64_t sequence) {
  protocol::Control control{};
  std::memcpy(control.magic, protocol::kControlMagic, sizeof(control.magic));
  control.version = protocol::kVersion;
  control.size = sizeof(control);
  control.flags = flags;
  control.frame_count = runtime.frame_count;
  control.session_id = runtime.session_id;
  control.expected_service = runtime.service;
  control.original_vptr = runtime.original_vptr;
  control.shadow_vptr = runtime.payload.shadow;
  control.original_activate = runtime.original_activate;
  control.active_sequence = sequence;
  return control;
}

inline protocol::Evidence MakeEvidence() {
  protocol::Evidence evidence{};
  std::memcpy(evidence.magic, protocol::kEvidenceMagic,
              sizeof(evidence.magic));
  evidence.version = protocol::kVersion;
  evidence.size = sizeof(evidence);
  evidence.last_status = protocol::kPassive;
  return evidence;
}

inline bool Stage(int mem, Runtime* runtime) {
  if (!runtime || runtime->staged || runtime->armed || runtime->restored)
    return false;
  protocol::Control old_control{};
  protocol::Evidence old_evidence{};
  std::uintptr_t live_vptr = 0;
  std::vector<std::uint32_t> old_counts(protocol::kMaximumFrames);
  if (!ReadExact(mem, runtime->payload.control, &old_control,
                 sizeof(old_control)) ||
      !ReadExact(mem, runtime->payload.evidence, &old_evidence,
                 sizeof(old_evidence)) ||
      !ReadExact(mem, runtime->payload.counts, old_counts.data(),
                 old_counts.size() * sizeof(old_counts.front())) ||
      !ReadExact(mem, runtime->service, &live_vptr, sizeof(live_vptr)) ||
      !ControlIdentity(old_control) || !EvidenceIdentity(old_evidence) ||
      old_control.flags != 0 || old_control.active_sequence != 0 ||
      old_evidence.wrapper_entries != 0 || live_vptr != runtime->original_vptr ||
      std::any_of(old_counts.begin(), old_counts.end(),
                  [](std::uint32_t value) { return value != 0; }))
    return false;

  std::vector<std::uint8_t> shadow(protocol::kShadowVtableSize);
  if (!ReadExact(mem, runtime->original_vptr, shadow.data(), shadow.size()))
    return false;
  std::memcpy(shadow.data() + protocol::kActivateSlotOffset,
              &runtime->payload.wrapper, sizeof(runtime->payload.wrapper));
  const protocol::Evidence evidence = MakeEvidence();
  std::vector<std::uint32_t> zero_counts(protocol::kMaximumFrames);
  protocol::Control control = MakeControl(*runtime, protocol::kConfigured, 0);
  if (!WriteExactVerified(mem, runtime->payload.shadow, shadow.data(),
                          shadow.size()) ||
      !WriteExactVerified(mem, runtime->payload.evidence, &evidence,
                          sizeof(evidence)) ||
      !WriteExactVerified(mem, runtime->payload.counts, zero_counts.data(),
                          zero_counts.size() * sizeof(zero_counts.front())) ||
      !WriteExactVerified(mem, runtime->payload.control, &control,
                          sizeof(control)))
    return false;
  control.flags = protocol::kConfigured | protocol::kInstalled;
  if (!WriteExactVerified(mem, runtime->payload.control, &control,
                          sizeof(control)))
    return false;
  const std::uintptr_t shadow_vptr = runtime->payload.shadow;
  if (!WriteExactVerified(mem, runtime->service, &shadow_vptr,
                          sizeof(shadow_vptr))) {
    std::uintptr_t observed_vptr = 0;
    if (ReadExact(mem, runtime->service, &observed_vptr,
                  sizeof(observed_vptr)) &&
        observed_vptr == shadow_vptr) {
      (void)WriteExactVerified(mem, runtime->service,
                               &runtime->original_vptr,
                               sizeof(runtime->original_vptr));
    }
    control.flags = 0;
    (void)WriteExactVerified(mem, runtime->payload.control, &control,
                             sizeof(control));
    return false;
  }
  runtime->staged = true;
  return true;
}

inline bool PublishSequence(int mem, Runtime* runtime,
                            std::uint64_t sequence) {
  if (!runtime || !runtime->staged || runtime->restored ||
      sequence > runtime->frame_count)
    return false;
  const std::uintptr_t address = runtime->payload.control +
      offsetof(protocol::Control, active_sequence);
  if (!WriteExactVerified(mem, address, &sequence, sizeof(sequence)))
    return false;
  runtime->armed = sequence != 0;
  return true;
}

inline bool ArmFirstFrame(int mem, Runtime* runtime) {
  return PublishSequence(mem, runtime, 1);
}

inline bool CommitFrame(int mem, Runtime* runtime,
                        std::uint32_t committed_frames) {
  if (!runtime || !runtime->armed || committed_frames == 0 ||
      committed_frames > runtime->frame_count)
    return false;
  const std::uint64_t next = committed_frames == runtime->frame_count
      ? 0u : static_cast<std::uint64_t>(committed_frames) + 1u;
  return PublishSequence(mem, runtime, next);
}

inline bool Finish(int mem, Runtime* runtime, Result* output) {
  if (!runtime || !output || !runtime->staged || runtime->restored)
    return false;
  Result result{};
  const std::uint64_t disarmed = 0;
  const bool disarm_ok = WriteExactVerified(
      mem, runtime->payload.control +
               offsetof(protocol::Control, active_sequence),
      &disarmed, sizeof(disarmed));
  runtime->armed = false;

  std::uintptr_t live_vptr = 0;
  const bool shadow_live = ReadExact(mem, runtime->service, &live_vptr,
                                     sizeof(live_vptr)) &&
                           live_vptr == runtime->payload.shadow;
  const bool restore_ok = shadow_live &&
      WriteExactVerified(mem, runtime->service, &runtime->original_vptr,
                         sizeof(runtime->original_vptr));
  std::uintptr_t restored_vptr = 0;
  result.service_restored = restore_ok &&
      ReadExact(mem, runtime->service, &restored_vptr,
                sizeof(restored_vptr)) &&
      restored_vptr == runtime->original_vptr;
  runtime->restored = result.service_restored;

  const bool evidence_read = ReadExact(mem, runtime->payload.evidence,
                                       &result.evidence,
                                       sizeof(result.evidence));
  result.counts.resize(runtime->frame_count);
  const bool counts_read = ReadExact(
      mem, runtime->payload.counts, result.counts.data(),
      result.counts.size() * sizeof(result.counts.front()));
  bool counts_bounded = counts_read;
  if (counts_read) {
    for (const std::uint32_t count : result.counts) {
      if (count > 2u) counts_bounded = false;
      result.count_sum += count;
    }
  }
  result.evidence_exact = evidence_read && counts_bounded &&
      EvidenceIdentity(result.evidence) &&
      result.evidence.failures == 0 &&
      result.evidence.out_of_window_calls == 0 &&
      result.evidence.overflow_calls == 0 &&
      result.evidence.wrapper_entries == result.evidence.original_calls &&
      result.evidence.original_calls == result.evidence.clean_returns &&
      result.evidence.counted_calls == result.evidence.wrapper_entries &&
      result.evidence.counted_calls == result.count_sum;

  // The returned Result owns the completed receipt.  Reset the mapped
  // evidence/count storage after the service has been restored so a second
  // recording transaction in the same preloaded process starts from the
  // exact pristine state required by Stage().
  const protocol::Evidence cleared_evidence = MakeEvidence();
  std::vector<std::uint32_t> cleared_counts(protocol::kMaximumFrames);
  const bool evidence_cleared = WriteExactVerified(
      mem, runtime->payload.evidence, &cleared_evidence,
      sizeof(cleared_evidence));
  const bool counts_cleared = WriteExactVerified(
      mem, runtime->payload.counts, cleared_counts.data(),
      cleared_counts.size() * sizeof(cleared_counts.front()));
  protocol::Control control = MakeControl(*runtime, 0, 0);
  const bool control_cleared = WriteExactVerified(
      mem, runtime->payload.control, &control, sizeof(control));
  *output = std::move(result);
  return disarm_ok && result.service_restored && output->evidence_exact &&
         evidence_cleared && counts_cleared && control_cleared;
}

}  // namespace a9tas::natural_action_recording_host_v1
