#pragma once

// Pure two-store certificate for the Android BarrelRoll stabilization tail.
// A runtime host watches owner+0x1968 and owner+0x196C as two consecutive
// four-byte watchpoint phases. It may apply the replay tail only after both
// exact field writes have completed in order on the authoritative physics
// thread. The ARM store RVAs below are static identity evidence; Houdini data
// watchpoints expose a host x86 RIP, so the runtime certificate must not claim
// that it observed an ARM PC directly.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "barrel_stabilization_replay_core_v1.h"

namespace a9tas::barrel_rbx_two_store_v1 {

namespace semantic = a9tas::barrel_stabilization_replay_v1;

inline constexpr std::uintptr_t kFirstStoreRva = 0x369E058;
inline constexpr std::uintptr_t kSecondStoreRva = 0x369E068;
inline constexpr std::uintptr_t kCallerReturnRva = 0x369CC08;
inline constexpr std::uintptr_t kFirstFieldOffset = 0x1968;
inline constexpr std::uintptr_t kSecondFieldOffset = 0x196C;

enum class StoreIdentity : std::uint32_t {
  kFirstField = 0,
  kSecondField = 1,
};

struct StoreCertificate {
  StoreIdentity store{};
  std::uintptr_t caller_return{};
  std::uint32_t caller_return_count{};
  std::uintptr_t watched_address{};
  std::uintptr_t owner{};
  std::uint32_t tid{};
};

enum class Result : std::int32_t {
  kIgnored = 0,
  kFirstStoreCertified = 1,
  kOverridden = 2,
  kNaturalSkipped = 3,
  kInvalidArgument = -1,
  kStaleFrame = -2,
  kUnexpectedStoreOrder = -3,
  kUnexpectedThread = -4,
  kUnexpectedOwner = -5,
  kIncompletePair = -6,
  kAmbiguousCallStack = -7,
  kUnqualifiedCallStack = -8,
};

enum class PairPhase : std::uint32_t {
  kIdle = 0,
  kFirstStoreSeen = 1,
  kFaulted = 2,
};

struct FrameContext {
  semantic::ActiveFrame active{};
  semantic::Permit expected_permit{};
  std::uintptr_t library_base{};
  std::uintptr_t expected_owner{};
  std::uint32_t expected_tid{};
};

struct Audit {
  std::uint32_t natural_pairs{};
  std::uint32_t overrides{};
  std::uint32_t skips{};
  std::uint32_t ignored_events{};
  // The first trap is ordering evidence only.  The authoritative rollback
  // pair is sampled contiguously at the second-store stop, after both natural
  // stores have completed.
  std::uint32_t first_store_certificate_bits{};
  std::uint32_t authoritative_before_bits[2]{};
  std::uint32_t final_bits[2]{};
  semantic::Capture capture{};
};

class Transaction {
 public:
  Result BeginFrame(const FrameContext& context) {
    if (!context.active.has_value || context.library_base == 0 ||
        context.library_base > UINTPTR_MAX - kCallerReturnRva ||
        context.expected_owner == 0 || context.expected_tid == 0 ||
        !semantic::SamePermit(context.active.permit,
                              context.expected_permit))
      return Fault(Result::kInvalidArgument);
    context_ = context;
    audit_ = {};
    phase_ = PairPhase::kIdle;
    active_ = true;
    return Result::kIgnored;
  }

  Result OnCompletedStore(const StoreCertificate& certificate,
                          std::uint32_t live_bits[2]) {
    if (!active_ || live_bits == nullptr)
      return Fault(Result::kStaleFrame);
    if (certificate.caller_return_count == 0 ||
        certificate.caller_return !=
            context_.library_base + kCallerReturnRva)
      return Fault(Result::kUnqualifiedCallStack);
    if (certificate.caller_return_count != 1)
      return Fault(Result::kAmbiguousCallStack);
    if (certificate.owner != context_.expected_owner)
      return Fault(Result::kUnexpectedOwner);
    if (certificate.tid != context_.expected_tid)
      return Fault(Result::kUnexpectedThread);

    if (certificate.owner > UINTPTR_MAX - kSecondFieldOffset)
      return Fault(Result::kUnexpectedOwner);
    const std::uintptr_t first_address =
        certificate.owner + kFirstFieldOffset;
    const std::uintptr_t second_address =
        certificate.owner + kSecondFieldOffset;
    if (certificate.store == StoreIdentity::kFirstField) {
      if (phase_ != PairPhase::kIdle ||
          certificate.watched_address != first_address)
        return Fault(Result::kUnexpectedStoreOrder);
      audit_.first_store_certificate_bits = live_bits[0];
      phase_ = PairPhase::kFirstStoreSeen;
      return Result::kFirstStoreCertified;
    }

    if (certificate.store != StoreIdentity::kSecondField ||
        phase_ != PairPhase::kFirstStoreSeen ||
        certificate.watched_address != second_address)
      return Fault(Result::kUnexpectedStoreOrder);
    audit_.authoritative_before_bits[0] = live_bits[0];
    audit_.authoritative_before_bits[1] = live_bits[1];
    semantic::PostOriginalContext post{context_.expected_permit, true, {}};
    const semantic::Result semantic_result =
        semantic::OnBarrelRollPostOriginal(context_.active, post, live_bits,
                                           &audit_.capture);
    audit_.final_bits[0] = live_bits[0];
    audit_.final_bits[1] = live_bits[1];
    ++audit_.natural_pairs;
    phase_ = PairPhase::kIdle;
    if (semantic_result == semantic::Result::kOverridden) {
      ++audit_.overrides;
      return Result::kOverridden;
    }
    if (semantic_result == semantic::Result::kNaturalSkipped) {
      ++audit_.skips;
      return Result::kNaturalSkipped;
    }
    return Fault(Result::kStaleFrame);
  }

  Result FinishFrame() {
    if (!active_) return Fault(Result::kStaleFrame);
    if (phase_ != PairPhase::kIdle)
      return Fault(Result::kIncompletePair);
    active_ = false;
    return Result::kIgnored;
  }

  const Audit& audit() const { return audit_; }
  PairPhase phase() const { return phase_; }
  bool active() const { return active_; }

 private:
  Result Fault(Result result) {
    phase_ = PairPhase::kFaulted;
    active_ = false;
    return result;
  }

  FrameContext context_{};
  Audit audit_{};
  PairPhase phase_{PairPhase::kIdle};
  bool active_{};
};

// Runtime-primitive-independent exact 8-byte transport.  The outer executor
// supplies read/write functions while the authoritative physics thread and
// every other potential user of the object are stopped.  At the second store
// stop this layer takes one contiguous authoritative snapshot, computes the
// AluTas tail locally, performs one contiguous 8-byte publish, and reads it
// back.  Any uncertain publish is rolled back from that same second-stop
// snapshot and still faults the frame so the outer executor cannot resume it.
using ReadFn = bool (*)(void*, std::uintptr_t, void*, std::size_t);
using WriteFn = bool (*)(void*, std::uintptr_t, const void*, std::size_t);

struct Io {
  void* context{};
  ReadFn read{};
  WriteFn write{};
};

enum class RemoteResult : std::int32_t {
  kFirstStoreCertified = 1,
  kOverridden = 2,
  kNaturalSkipped = 3,
  kInvalidArgument = -1,
  kUnexpectedThread = -2,
  kReadFailed = -3,
  kCoreRejected = -4,
  kWriteFailedRolledBack = -5,
  kReadbackMismatchRolledBack = -6,
  kMutationUncertain = -7,
  kIncomplete = -8,
};

class RemotePairTransaction {
 public:
  RemoteResult BeginFrame(const FrameContext& context) {
    if (context.expected_owner == 0 ||
        context.expected_owner > UINTPTR_MAX - kSecondFieldOffset ||
        ((context.expected_owner + kFirstFieldOffset) & 7u) != 0 ||
        context.expected_tid == 0 ||
        core_.BeginFrame(context) != Result::kIgnored)
      return Fault(RemoteResult::kInvalidArgument, false);
    context_ = context;
    pair_address_ = context.expected_owner + kFirstFieldOffset;
    active_ = true;
    completed_ = false;
    faulted_ = false;
    mutation_uncertain_ = false;
    rollback_succeeded_ = false;
    last_success_ = RemoteResult::kIncomplete;
    return RemoteResult::kFirstStoreCertified;
  }

  RemoteResult OnFirstStore(const Io& io,
                            const StoreCertificate& certificate,
                            std::uint32_t stopped_tid) {
    if (!Ready(io, stopped_tid))
      return Fault(RemoteResult::kUnexpectedThread, false);
    std::uint32_t observed[2]{};
    if (!io.read(io.context, pair_address_, observed, sizeof(observed)))
      return Fault(RemoteResult::kReadFailed, false);
    if (core_.OnCompletedStore(certificate, observed) !=
        Result::kFirstStoreCertified)
      return Fault(RemoteResult::kCoreRejected, false);
    return RemoteResult::kFirstStoreCertified;
  }

  RemoteResult OnSecondStore(const Io& io,
                             const StoreCertificate& certificate,
                             std::uint32_t stopped_tid) {
    if (!Ready(io, stopped_tid))
      return Fault(RemoteResult::kUnexpectedThread, false);
    std::uint32_t before[2]{};
    if (!io.read(io.context, pair_address_, before, sizeof(before)))
      return Fault(RemoteResult::kReadFailed, false);
    std::uint32_t desired[2] = {before[0], before[1]};
    const Result core_result = core_.OnCompletedStore(certificate, desired);
    if (core_result == Result::kNaturalSkipped) {
      if (std::memcmp(before, desired, sizeof(before)) != 0)
        return Fault(RemoteResult::kCoreRejected, false);
      completed_ = true;
      last_success_ = RemoteResult::kNaturalSkipped;
      return RemoteResult::kNaturalSkipped;
    }
    if (core_result != Result::kOverridden)
      return Fault(RemoteResult::kCoreRejected, false);

    // One exact 8-byte publish.  A false return is mutation-uncertain because
    // a remote writer may have copied a prefix before reporting failure.
    if (!io.write(io.context, pair_address_, desired, sizeof(desired)))
      return RollBack(io, before, RemoteResult::kWriteFailedRolledBack);
    std::uint32_t readback[2]{};
    if (!io.read(io.context, pair_address_, readback, sizeof(readback)) ||
        std::memcmp(readback, desired, sizeof(readback)) != 0)
      return RollBack(io, before,
                      RemoteResult::kReadbackMismatchRolledBack);
    completed_ = true;
    last_success_ = RemoteResult::kOverridden;
    return RemoteResult::kOverridden;
  }

  RemoteResult FinishFrame() {
    if (!active_ || faulted_ || !completed_ ||
        core_.FinishFrame() != Result::kIgnored)
      return Fault(RemoteResult::kIncomplete, mutation_uncertain_);
    active_ = false;
    return last_success_;
  }

  const Audit& audit() const { return core_.audit(); }
  bool faulted() const { return faulted_; }
  bool mutation_uncertain() const { return mutation_uncertain_; }
  bool rollback_succeeded() const { return rollback_succeeded_; }
  std::uintptr_t pair_address() const { return pair_address_; }

 private:
  bool Ready(const Io& io, std::uint32_t stopped_tid) const {
    return active_ && !faulted_ && io.read != nullptr &&
           io.write != nullptr && stopped_tid == context_.expected_tid;
  }

  RemoteResult RollBack(const Io& io, const std::uint32_t before[2],
                        RemoteResult rolled_back_result) {
    std::uint32_t restored[2]{};
    const bool restored_exactly =
        io.write(io.context, pair_address_, before, sizeof(restored)) &&
        io.read(io.context, pair_address_, restored, sizeof(restored)) &&
        std::memcmp(restored, before, sizeof(restored)) == 0;
    rollback_succeeded_ = restored_exactly;
    return Fault(restored_exactly ? rolled_back_result
                                  : RemoteResult::kMutationUncertain,
                 !restored_exactly);
  }

  RemoteResult Fault(RemoteResult result, bool mutation_uncertain) {
    faulted_ = true;
    active_ = false;
    mutation_uncertain_ = mutation_uncertain;
    return result;
  }

  FrameContext context_{};
  Transaction core_{};
  std::uintptr_t pair_address_{};
  RemoteResult last_success_{RemoteResult::kOverridden};
  bool active_{};
  bool completed_{};
  bool faulted_{};
  bool mutation_uncertain_{};
  bool rollback_succeeded_{};
};

}  // namespace a9tas::barrel_rbx_two_store_v1
