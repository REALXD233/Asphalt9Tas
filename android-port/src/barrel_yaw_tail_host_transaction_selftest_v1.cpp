#include "barrel_yaw_tail_host_transaction_v1.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace host = a9tas::barrel_yaw_tail_host_v1;
namespace protocol = a9tas::barrel_yaw_tail_payload_v1;

namespace {

protocol::Control FreshControl() {
  protocol::Control value{};
  std::memcpy(value.magic, protocol::kControlMagic, 8);
  value.version = protocol::kProtocolVersion;
  value.size = sizeof(value);
  return value;
}

protocol::Evidence FreshEvidence() {
  protocol::Evidence value{};
  std::memcpy(value.magic, protocol::kEvidenceMagic, 8);
  value.version = protocol::kProtocolVersion;
  value.size = sizeof(value);
  value.last_status = protocol::kStatusPassive;
  return value;
}

protocol::Control ArmedControl(const host::Prepared& prepared,
                               std::uint32_t sequence,
                               std::uint32_t frame_index,
                               std::uint32_t audit_index) {
  protocol::Control value = prepared.published_control;
  value.active_audit_index = audit_index;
  value.active_frame_index = frame_index;
  value.active_token = protocol::ArmToken(
      value.session_generation, sequence);
  return value;
}

protocol::Evidence CompletedEvidence(const protocol::Evidence& before,
                                     const protocol::Control& control,
                                     std::uint64_t token,
                                     std::uint32_t audit_index,
                                     bool skipped) {
  protocol::Evidence value = before;
  value.wrapper_entries += 2;
  value.original_calls += 2;
  value.original_returns += 2;
  ++value.completed_transactions;
  if (skipped)
    ++value.skipped_transactions;
  else
    ++value.override_writes;
  value.active_call_count = 0;
  value.audit_count = audit_index + 1u;
  value.last_status = protocol::kStatusComplete;
  value.last_tid = control.expected_tid;
  value.last_token = token;
  value.last_object = control.expected_object;
  value.final_vptr = control.original_vptr;
  return value;
}

bool Configure(host::Transaction* transaction,
               const host::Prepared& prepared) {
  return transaction != nullptr &&
         transaction->MarkPayloadStaged() == host::Result::kOk &&
         transaction->MarkConfigurationPublished(
             prepared.published_control) == host::Result::kOk;
}

}  // namespace

int main() {
  constexpr std::uintptr_t kLibraryBase = 0x20000000u;
  constexpr std::uintptr_t kObject = 0x10002000u;
  constexpr std::uintptr_t kOriginalVptr =
      kLibraryBase + protocol::kPhysicsBackendVptrRva;
  constexpr std::uintptr_t kPayloadShadow = 0x30004000u;
  constexpr std::uintptr_t kWrapper = 0x40005000u;
  constexpr std::uintptr_t kOriginalBoundary =
      kLibraryBase + protocol::kOriginalBoundaryCallbackRva;
  constexpr std::uintptr_t kNativeAngular = 0x60007160u;

  std::array<std::uint8_t, protocol::kShadowSize> table{};
  for (std::size_t index = 0; index < table.size(); ++index)
    table[index] = static_cast<std::uint8_t>((index * 37u + 11u) & 0xffu);
  std::memcpy(table.data() + protocol::kVptrPrefixSize +
                  protocol::kBoundarySlotOffset,
              &kOriginalBoundary, sizeof(kOriginalBoundary));
  std::array<std::uint8_t, 32> source_hash{};
  source_hash[0] = 0xA9;
  host::Prepared prepared{};
  const bool prepared_ok = host::Prepare(
      table.data(), kObject, kOriginalVptr, kPayloadShadow, kWrapper,
      kNativeAngular, 123u, 7u, source_hash.data(), 900u, FreshControl(),
      FreshEvidence(), &prepared) == host::Result::kOk;

  bool complete_shadow = prepared_ok && prepared.library_base == kLibraryBase &&
      prepared.shadow_vptr == kPayloadShadow + protocol::kVptrPrefixSize &&
      prepared.original_boundary_callback == kOriginalBoundary &&
      prepared.published_control.expected_first_caller_return ==
          kLibraryBase + protocol::kFirstBoundaryCallerReturnRva &&
      prepared.published_control.expected_second_caller_return ==
          kLibraryBase + protocol::kSecondBoundaryCallerReturnRva;
  for (std::size_t index = 0; index < table.size() && complete_shadow; ++index) {
    const std::size_t patch_begin =
        protocol::kVptrPrefixSize + protocol::kBoundarySlotOffset;
    const bool patched = index >= patch_begin &&
                         index < patch_begin + sizeof(std::uintptr_t);
    if (!patched && (prepared.original[index] != table[index] ||
                     prepared.shadow[index] != table[index]))
      complete_shadow = false;
  }
  std::uintptr_t patched_callback = 0;
  std::memcpy(&patched_callback,
              prepared.shadow.data() + protocol::kVptrPrefixSize +
                  protocol::kBoundarySlotOffset,
              sizeof(patched_callback));
  complete_shadow = complete_shadow && patched_callback == kWrapper &&
                    prepared.original[protocol::kShadowSize - 1] ==
                        table[protocol::kShadowSize - 1];

  host::Transaction lifecycle;
  bool lifecycle_ok = complete_shadow && Configure(&lifecycle, prepared);
  protocol::Evidence evidence0 = prepared.fresh_evidence;
  protocol::Control armed1 = ArmedControl(prepared, 1, 40, 0);
  lifecycle_ok = lifecycle_ok &&
      lifecycle.MarkShadowInstalled(prepared.shadow_vptr, armed1) ==
          host::Result::kOk;
  protocol::Evidence evidence1 = CompletedEvidence(
      evidence0, prepared.published_control, armed1.active_token, 0, false);
  protocol::Control terminal1 = armed1;
  terminal1.active_token = protocol::kDisarmedToken;
  lifecycle_ok = lifecycle_ok &&
      lifecycle.MarkFrameTerminal(kOriginalVptr, terminal1, evidence0,
                                  evidence1) == host::Result::kOk;

  protocol::Control armed2 = ArmedControl(prepared, 2, 41, 1);
  lifecycle_ok = lifecycle_ok &&
      lifecycle.MarkShadowInstalled(prepared.shadow_vptr, armed2) ==
          host::Result::kOk;
  protocol::Evidence evidence2 = CompletedEvidence(
      evidence1, prepared.published_control, armed2.active_token, 1, true);
  protocol::Control terminal2 = armed2;
  terminal2.active_token = protocol::kDisarmedToken;
  lifecycle_ok = lifecycle_ok &&
      lifecycle.MarkFrameTerminal(kOriginalVptr, terminal2, evidence1,
                                  evidence2) == host::Result::kOk &&
      lifecycle.MarkReplayComplete(terminal2, evidence2, 2) ==
          host::Result::kOk &&
      lifecycle.MarkRolledBack(kOriginalVptr, kOriginalVptr, terminal2,
                               evidence2) == host::Result::kOk &&
      lifecycle.phase() == host::Phase::kRolledBack;

  host::Transaction aba;
  bool rejects_aba = Configure(&aba, prepared);
  protocol::Control aba_first = ArmedControl(prepared, 1, 50, 0);
  protocol::Control aba_terminal = aba_first;
  aba_terminal.active_token = protocol::kDisarmedToken;
  rejects_aba = rejects_aba &&
      aba.MarkShadowInstalled(prepared.shadow_vptr, aba_first) ==
          host::Result::kOk &&
      aba.MarkFrameCancelled(kOriginalVptr, aba_terminal, evidence0,
                             evidence0) == host::Result::kOk &&
      aba.MarkShadowInstalled(prepared.shadow_vptr, aba_first) ==
          host::Result::kNonMonotonicSequence &&
      aba.phase() == host::Phase::kRollbackRequired;

  host::Transaction per_frame;
  bool rejects_third = Configure(&per_frame, prepared);
  for (std::uint32_t sequence = 1; sequence <= 2 && rejects_third;
       ++sequence) {
    protocol::Control armed = ArmedControl(prepared, sequence, 60, 0);
    protocol::Control terminal = armed;
    terminal.active_token = protocol::kDisarmedToken;
    rejects_third =
        per_frame.MarkShadowInstalled(prepared.shadow_vptr, armed) ==
            host::Result::kOk &&
        per_frame.MarkFrameCancelled(kOriginalVptr, terminal, evidence0,
                                     evidence0) == host::Result::kOk;
  }
  protocol::Control third = ArmedControl(prepared, 3, 60, 0);
  rejects_third = rejects_third &&
      per_frame.MarkShadowInstalled(prepared.shadow_vptr, third) ==
          host::Result::kPerFrameTransactionLimit &&
      per_frame.phase() == host::Phase::kRollbackRequired;

  auto bad_delta = evidence1;
  ++bad_delta.wrapper_entries;
  const bool rejects_bad_delta =
      !host::EvidenceDeltaMatches(prepared.published_control, evidence0,
                                  bad_delta, armed1.active_token, 0);

  auto bad_table = table;
  const std::uintptr_t wrong_boundary = kOriginalBoundary + 4u;
  std::memcpy(bad_table.data() + protocol::kVptrPrefixSize +
                  protocol::kBoundarySlotOffset,
              &wrong_boundary, sizeof(wrong_boundary));
  const bool rejects_wrong_slot = host::Prepare(
      bad_table.data(), kObject, kOriginalVptr, kPayloadShadow, kWrapper,
      kNativeAngular, 123u, 7u, source_hash.data(), 900u, FreshControl(),
      FreshEvidence(), &prepared) == host::Result::kUnexpectedBoundarySlot;
  const bool rejects_wrong_vptr = host::Prepare(
      table.data(), kObject, kOriginalVptr + 0x10u, kPayloadShadow, kWrapper,
      kNativeAngular, 123u, 7u, source_hash.data(), 900u, FreshControl(),
      FreshEvidence(), &prepared) == host::Result::kUnexpectedBuildIdentity;
  std::array<std::uint8_t, 32> zero_hash{};
  const bool rejects_zero_hash = host::Prepare(
      table.data(), kObject, kOriginalVptr, kPayloadShadow, kWrapper,
      kNativeAngular, 123u, 7u, zero_hash.data(), 900u, FreshControl(),
      FreshEvidence(), &prepared) == host::Result::kInvalidArgument;

  const bool passed = lifecycle_ok && rejects_aba && rejects_third &&
                      rejects_bad_delta && rejects_wrong_slot &&
                      rejects_wrong_vptr && rejects_zero_hash;
  std::printf(
      "BARREL_YAW_HOST_TRANSACTION_SELFTEST passed=%u complete_shadow=1 "
      "transient_shadow=1 exact_lr=1 monotonic_sequence=1 per_frame_limit=1 "
      "field_delta=1 original_finish=1 zero_hash_rejected=1 runtime=disabled\n",
      passed ? 1u : 0u);
  return passed ? 0 : 1;
}
