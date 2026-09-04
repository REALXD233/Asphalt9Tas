#include "barrel_rbx_two_store_transaction_v1.h"

#include <cstdio>

namespace rbx = a9tas::barrel_rbx_two_store_v1;
namespace semantic = a9tas::barrel_stabilization_replay_v1;

struct PairMemory {
  std::uintptr_t address{};
  std::uint32_t words[2]{};
  std::uint32_t reads{};
  std::uint32_t writes{};
  std::uint32_t fail_write_call{};
  bool fail_all_writes{};
  bool fail_after_mutation{};
};

bool ReadPair(void* context, std::uintptr_t address, void* output,
              std::size_t size) {
  auto* memory = static_cast<PairMemory*>(context);
  if (memory == nullptr || output == nullptr || address != memory->address ||
      size != sizeof(memory->words))
    return false;
  ++memory->reads;
  std::memcpy(output, memory->words, sizeof(memory->words));
  return true;
}

bool WritePair(void* context, std::uintptr_t address, const void* input,
               std::size_t size) {
  auto* memory = static_cast<PairMemory*>(context);
  if (memory == nullptr || input == nullptr || address != memory->address ||
      size != sizeof(memory->words))
    return false;
  ++memory->writes;
  const bool fail = memory->fail_all_writes ||
                    memory->fail_write_call == memory->writes;
  if (!fail || memory->fail_after_mutation)
    std::memcpy(memory->words, input, sizeof(memory->words));
  return !fail;
}

int main() {
  constexpr std::uintptr_t kBase = 0x10000000u;
  constexpr std::uintptr_t kOwner = 0x20000000u;
  constexpr std::uint32_t kTid = 321u;
  rbx::FrameContext context{};
  context.active.has_value = true;
  context.active.permit = {9u, 42u, 0u};
  context.active.rbx_bits[0] = 0x3f800000u;
  context.active.rbx_bits[1] = 0x40000000u;
  context.expected_permit = context.active.permit;
  context.library_base = kBase;
  context.expected_owner = kOwner;
  context.expected_tid = kTid;

  std::uint32_t live[2] = {0x40400000u, 0x40800000u};
  const auto certificate = [&](rbx::StoreIdentity store,
                               std::uintptr_t address,
                               std::uint32_t marker_count = 1u) {
    return rbx::StoreCertificate{
        store, kBase + rbx::kCallerReturnRva, marker_count,
        address, kOwner, kTid};
  };
  rbx::Transaction transaction;
  const bool normal =
      transaction.BeginFrame(context) == rbx::Result::kIgnored &&
      transaction.OnCompletedStore(
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset), live) ==
          rbx::Result::kFirstStoreCertified &&
      live[0] == 0x40400000u && live[1] == 0x40800000u;
  // Model an intervening natural writer.  The second-store stop is the only
  // authoritative rollback snapshot; the first trap remains order evidence.
  live[0] = 0x40e00000u;
  const bool normal_tail =
      normal &&
      transaction.OnCompletedStore(
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset), live) ==
          rbx::Result::kOverridden &&
      live[0] == 0x3f800000u && live[1] == 0x40000000u &&
      transaction.audit().natural_pairs == 1 &&
      transaction.audit().overrides == 1 &&
      transaction.audit().first_store_certificate_bits == 0x40400000u &&
      transaction.audit().authoritative_before_bits[0] == 0x40e00000u &&
      transaction.audit().authoritative_before_bits[1] == 0x40800000u &&
      transaction.FinishFrame() == rbx::Result::kIgnored;

  context.active.skip_override_flags =
      a9tas::unified_tick_v1::kSkipBarrelRbx;
  live[0] = 0x40a00000u;
  live[1] = 0x40c00000u;
  rbx::Transaction skipped;
  const bool skip =
      skipped.BeginFrame(context) == rbx::Result::kIgnored &&
      skipped.OnCompletedStore(
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset), live) ==
          rbx::Result::kFirstStoreCertified &&
      skipped.OnCompletedStore(
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset), live) ==
          rbx::Result::kNaturalSkipped &&
      live[0] == 0x40a00000u && live[1] == 0x40c00000u &&
      skipped.audit().capture.rbx_bits[0] == live[0] &&
      skipped.audit().capture.rbx_bits[1] == live[1] &&
      skipped.FinishFrame() == rbx::Result::kIgnored;

  context.active.skip_override_flags = 0;
  rbx::Transaction incomplete;
  const bool rejects_incomplete =
      incomplete.BeginFrame(context) == rbx::Result::kIgnored &&
      incomplete.OnCompletedStore(
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset), live) ==
          rbx::Result::kFirstStoreCertified &&
      incomplete.FinishFrame() == rbx::Result::kIncompletePair &&
      incomplete.phase() == rbx::PairPhase::kFaulted;

  rbx::Transaction wrong_watch_phase;
  const bool rejects_wrong_watch_phase =
      wrong_watch_phase.BeginFrame(context) == rbx::Result::kIgnored &&
      wrong_watch_phase.OnCompletedStore(
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset), live) ==
          rbx::Result::kUnexpectedStoreOrder &&
      wrong_watch_phase.phase() == rbx::PairPhase::kFaulted;

  rbx::Transaction unqualified;
  const bool rejects_unqualified =
      unqualified.BeginFrame(context) == rbx::Result::kIgnored &&
      unqualified.OnCompletedStore(
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset, 0), live) ==
          rbx::Result::kUnqualifiedCallStack &&
      unqualified.phase() == rbx::PairPhase::kFaulted;

  rbx::Transaction ambiguous;
  const bool rejects_ambiguous =
      ambiguous.BeginFrame(context) == rbx::Result::kIgnored &&
      ambiguous.OnCompletedStore(
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset, 2), live) ==
          rbx::Result::kAmbiguousCallStack;

  context.active.skip_override_flags = 0;
  PairMemory remote_memory{
      kOwner + rbx::kFirstFieldOffset,
      {0x40400000u, 0x40800000u}};
  rbx::Io remote_io{&remote_memory, ReadPair, WritePair};
  rbx::RemotePairTransaction remote;
  const bool remote_first =
      remote.BeginFrame(context) == rbx::RemoteResult::kFirstStoreCertified &&
      remote.OnFirstStore(
          remote_io,
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset),
          kTid) == rbx::RemoteResult::kFirstStoreCertified &&
      remote_memory.reads == 1 && remote_memory.writes == 0;
  remote_memory.words[0] = 0x40e00000u;
  const bool remote_exact_pair =
      remote_first &&
      remote.OnSecondStore(
          remote_io,
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset),
          kTid) == rbx::RemoteResult::kOverridden &&
      remote_memory.words[0] == context.active.rbx_bits[0] &&
      remote_memory.words[1] == context.active.rbx_bits[1] &&
      remote_memory.reads == 3 && remote_memory.writes == 1 &&
      remote.audit().authoritative_before_bits[0] == 0x40e00000u &&
      remote.audit().authoritative_before_bits[1] == 0x40800000u &&
      remote.FinishFrame() == rbx::RemoteResult::kOverridden;

  context.active.skip_override_flags =
      a9tas::unified_tick_v1::kSkipBarrelRbx;
  PairMemory remote_skip_memory{
      kOwner + rbx::kFirstFieldOffset,
      {0x40a00000u, 0x40c00000u}};
  rbx::Io remote_skip_io{&remote_skip_memory, ReadPair, WritePair};
  rbx::RemotePairTransaction remote_skip;
  const bool remote_no_write_skip =
      remote_skip.BeginFrame(context) ==
          rbx::RemoteResult::kFirstStoreCertified &&
      remote_skip.OnFirstStore(
          remote_skip_io,
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset),
          kTid) == rbx::RemoteResult::kFirstStoreCertified &&
      remote_skip.OnSecondStore(
          remote_skip_io,
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset),
          kTid) == rbx::RemoteResult::kNaturalSkipped &&
      remote_skip_memory.reads == 2 && remote_skip_memory.writes == 0 &&
      remote_skip.FinishFrame() == rbx::RemoteResult::kNaturalSkipped;

  context.active.skip_override_flags = 0;
  PairMemory rollback_memory{
      kOwner + rbx::kFirstFieldOffset,
      {0x40a00000u, 0x40c00000u}};
  rollback_memory.fail_write_call = 1;
  rollback_memory.fail_after_mutation = true;
  rbx::Io rollback_io{&rollback_memory, ReadPair, WritePair};
  rbx::RemotePairTransaction rollback;
  const bool exact_rollback =
      rollback.BeginFrame(context) ==
          rbx::RemoteResult::kFirstStoreCertified &&
      rollback.OnFirstStore(
          rollback_io,
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset),
          kTid) == rbx::RemoteResult::kFirstStoreCertified &&
      rollback.OnSecondStore(
          rollback_io,
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset),
          kTid) == rbx::RemoteResult::kWriteFailedRolledBack &&
      rollback_memory.words[0] == 0x40a00000u &&
      rollback_memory.words[1] == 0x40c00000u &&
      rollback_memory.writes == 2 && rollback.rollback_succeeded() &&
      rollback.faulted() && !rollback.mutation_uncertain();

  PairMemory uncertain_memory{
      kOwner + rbx::kFirstFieldOffset,
      {0x40a00000u, 0x40c00000u}};
  uncertain_memory.fail_all_writes = true;
  uncertain_memory.fail_after_mutation = true;
  rbx::Io uncertain_io{&uncertain_memory, ReadPair, WritePair};
  rbx::RemotePairTransaction uncertain;
  const bool uncertain_fail_closed =
      uncertain.BeginFrame(context) ==
          rbx::RemoteResult::kFirstStoreCertified &&
      uncertain.OnFirstStore(
          uncertain_io,
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset),
          kTid) == rbx::RemoteResult::kFirstStoreCertified &&
      uncertain.OnSecondStore(
          uncertain_io,
          certificate(rbx::StoreIdentity::kSecondField,
                      kOwner + rbx::kSecondFieldOffset),
          kTid) == rbx::RemoteResult::kMutationUncertain &&
      uncertain.faulted() && uncertain.mutation_uncertain();

  PairMemory wrong_tid_memory{
      kOwner + rbx::kFirstFieldOffset,
      {0x40a00000u, 0x40c00000u}};
  rbx::Io wrong_tid_io{&wrong_tid_memory, ReadPair, WritePair};
  rbx::RemotePairTransaction wrong_tid_remote;
  const bool remote_tid_bound =
      wrong_tid_remote.BeginFrame(context) ==
          rbx::RemoteResult::kFirstStoreCertified &&
      wrong_tid_remote.OnFirstStore(
          wrong_tid_io,
          certificate(rbx::StoreIdentity::kFirstField,
                      kOwner + rbx::kFirstFieldOffset),
          kTid + 1u) == rbx::RemoteResult::kUnexpectedThread &&
      wrong_tid_memory.reads == 0 && wrong_tid_memory.writes == 0;

  rbx::FrameContext misaligned_context = context;
  misaligned_context.expected_owner = kOwner + 4u;
  rbx::RemotePairTransaction misaligned;
  const bool rejects_misaligned_pair =
      misaligned.BeginFrame(misaligned_context) ==
      rbx::RemoteResult::kInvalidArgument;

  const bool passed = normal_tail && skip && rejects_incomplete &&
                      rejects_wrong_watch_phase && rejects_unqualified &&
                      rejects_ambiguous && remote_exact_pair &&
                      remote_no_write_skip && exact_rollback &&
                      uncertain_fail_closed && remote_tid_bound &&
                      rejects_misaligned_pair;
  std::printf(
      "BARREL_RBX_TWO_STORE_SELFTEST passed=%u post_original=1 "
      "two_phase_watch_certificate=1 second_stop_snapshot=1 unique_caller_marker=1 "
      "independent_skip=1 exact_8byte_transport=1 stopped_tid_binding=1 "
      "single_publish_readback=1 conditional_rollback=1 "
      "mutation_uncertain_fail_closed=1 alignment_guard=1 runtime=disabled\n",
      passed ? 1u : 0u);
  return passed ? 0 : 1;
}
