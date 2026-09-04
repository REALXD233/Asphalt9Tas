#pragma once

// Narrow Android runtime bridge for the two upstream AluTasV2 barrel tails.
// It is intentionally not a collector: it is called only from the already
// proven authoritative owner-thread C9C -> F64 event loop.  RBX is certified
// at its two exact field stores; BarrelYaw uses a bounded transient vtable
// window whose wrapper accepts only the two exact caller returns.

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "barrel_post_c9c_slot_schedule_v1.h"
#include "barrel_post_c9c_watch_layout_v1.h"
#include "barrel_rbx_two_store_transaction_v1.h"
#include "barrel_yaw_tail_transport_v1.h"
#include "unified_tick_recording_v1.h"

namespace a9tas::barrel_successor_runtime_v1 {

namespace recording = a9tas::unified_tick_v1;
namespace rbx = a9tas::barrel_rbx_two_store_v1;
namespace schedule = a9tas::barrel_post_c9c_schedule_v1;
namespace watch = a9tas::barrel_post_c9c_watch_v1;
namespace yaw = a9tas::barrel_yaw_tail_transport_v1;
namespace yaw_protocol = a9tas::barrel_yaw_tail_payload_v1;
namespace semantic = a9tas::barrel_stabilization_replay_v1;

struct Statistics {
  std::uint32_t committed_frames{};
  std::uint32_t rbx_pairs{};
  std::uint32_t yaw_transactions{};
  std::uint32_t yaw_absent_cancellations{};
  std::uint32_t passive_aux_hits{};
  std::uint32_t failures{};
};

struct StackCertificate {
  std::uint32_t count{};
  std::uint32_t index{UINT32_MAX};
  std::uint64_t neighborhood[5]{};
};

inline bool ReadProcess(void* context, std::uintptr_t address, void* output,
                        std::size_t size) {
  if (context == nullptr || address == 0 || output == nullptr || size == 0)
    return false;
  const int fd = *static_cast<const int*>(context);
  auto* bytes = static_cast<std::uint8_t*>(output);
  std::size_t completed = 0;
  while (completed < size) {
    const ssize_t count = pread(fd, bytes + completed, size - completed,
                                static_cast<off_t>(address + completed));
    if (count <= 0) return false;
    completed += static_cast<std::size_t>(count);
  }
  return true;
}

inline bool WriteProcess(void* context, std::uintptr_t address,
                         const void* input, std::size_t size) {
  if (context == nullptr || address == 0 || input == nullptr || size == 0)
    return false;
  const int fd = *static_cast<const int*>(context);
  const auto* bytes = static_cast<const std::uint8_t*>(input);
  std::size_t completed = 0;
  while (completed < size) {
    const ssize_t count = pwrite(fd, bytes + completed, size - completed,
                                 static_cast<off_t>(address + completed));
    if (count <= 0) return false;
    completed += static_cast<std::size_t>(count);
  }
  return true;
}

inline bool CaptureStackCertificate(pid_t tid, int mem,
                                    std::uintptr_t marker,
                                    StackCertificate* output) {
  if (tid <= 0 || mem < 0 || marker == 0 || output == nullptr) return false;
  user_regs_struct regs{};
  if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1 || regs.rsp == 0)
    return false;
  std::uint64_t words[64]{};
  if (!ReadProcess(&mem, static_cast<std::uintptr_t>(regs.rsp), words,
                   sizeof(words)))
    return false;
  StackCertificate result{};
  for (std::uint32_t index = 0; index < std::size(words); ++index) {
    if (words[index] != marker) continue;
    ++result.count;
    result.index = index;
  }
  if (result.count != 1) {
    *output = result;
    return true;
  }
  for (std::uint32_t offset = 0; offset < 5; ++offset) {
    const std::int64_t candidate =
        static_cast<std::int64_t>(result.index) + offset - 2;
    result.neighborhood[offset] =
        candidate >= 0 && candidate < static_cast<std::int64_t>(std::size(words))
            ? words[candidate]
            : 0;
  }
  *output = result;
  return true;
}

inline bool SameStackCertificate(const StackCertificate& left,
                                 const StackCertificate& right) {
  return left.count == 1 && right.count == 1 && left.index == right.index &&
         std::memcmp(left.neighborhood, right.neighborhood,
                     sizeof(left.neighborhood)) == 0;
}

class Runtime {
 public:
  bool Setup(pid_t pid, int mem, std::uintptr_t library_base,
             std::uintptr_t yaw_object, std::uintptr_t rbx_owner,
             std::uintptr_t native_angular, std::uintptr_t callback_flags,
             std::uintptr_t f64, std::uintptr_t world_commit,
             const std::vector<recording::RecordingFrameV1>& frames,
             const std::uint8_t recording_sha256[32]) {
    if (pid <= 0 || mem < 0 || library_base == 0 || yaw_object == 0 ||
        rbx_owner == 0 || native_angular == 0 || callback_flags == 0 ||
        f64 == 0 || world_commit == 0 || recording_sha256 == nullptr ||
        frames.size() < 2 ||
        frames.size() > yaw_protocol::kMaximumFrames ||
        !watch::Prepare(rbx_owner, native_angular, callback_flags, f64,
                        world_commit, &watch_layout_) ||
        !yaw::elf::Resolve(pid, mem, &payload_))
      return false;
    bool hash_nonzero = false;
    for (std::size_t index = 0; index < 32; ++index)
      hash_nonzero = hash_nonzero || recording_sha256[index] != 0;
    if (!hash_nonzero) return false;

    pid_ = pid;
    mem_ = mem;
    library_base_ = library_base;
    yaw_object_ = yaw_object;
    rbx_owner_ = rbx_owner;
    native_angular_ = native_angular;
    frames_ = &frames;
    std::memcpy(recording_sha256_.data(), recording_sha256,
                recording_sha256_.size());
    generation_ = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(pid) ^ library_base ^
        (library_base >> 32) ^ frames.size());
    if (generation_ == 0) generation_ = 1;

    yaw_targets_.resize(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index) {
      std::memcpy(yaw_targets_[index].angular_bits,
                  frames[index].barrel_angular_velocity,
                  sizeof(yaw_targets_[index].angular_bits));
      yaw_targets_[index].skip_override_flags =
          frames[index].skip_override_flags;
    }
    configured_ = true;
    return true;
  }

  bool OnCertifiedC9C(std::uint32_t stopped_tid,
                      std::uint32_t frame_index) {
    if (!configured_ || faulted_ || frame_active_ || frames_ == nullptr ||
        frame_index >= frames_->size())
      return Fail(false);
    const yaw::Io io{&mem_, &ReadProcess, &WriteProcess};
    if (!yaw_session_.installed) {
      expected_tid_ = stopped_tid;
      if (!yaw::Install(io, payload_, yaw_object_, native_angular_,
                        expected_tid_, generation_, recording_sha256_.data(),
                        yaw_targets_.data(),
                        static_cast<std::uint32_t>(yaw_targets_.size()),
                        &yaw_session_))
        return Fail(false);
    } else if (stopped_tid != expected_tid_) {
      return Fail(true);
    }
    if (!yaw::BeginFrame(io, frame_index, stopped_tid, &yaw_session_))
      return Fail(true);

    interlocked_ = {};
    std::uint32_t actions = 0;
    if (!schedule::AdvanceInterlocked(
            &interlocked_, schedule::InterlockedEvent::kCompletionStore,
            &actions) ||
        !schedule::AdvanceInterlocked(
            &interlocked_, schedule::InterlockedEvent::kCertifiedC9C,
            &actions) ||
        actions != schedule::kActionInstallParallelWatchSet)
      return Fail(true);
    current_frame_ = frame_index;
    frame_active_ = true;
    rbx_transaction_ = {};
    first_stack_ = {};
    return true;
  }

  bool OnRbxFirstStore(std::uint32_t stopped_tid) {
    if (!ActiveOwner(stopped_tid)) return Fail(true);
    const semantic::ActiveFrame active = ActiveFrame(current_frame_);
    const semantic::Permit permit{generation_, current_frame_, 0};
    const rbx::FrameContext context{active, permit, library_base_, rbx_owner_,
                                    stopped_tid};
    rbx_transaction_ = {};
    if (rbx_transaction_.BeginFrame(context) !=
        rbx::RemoteResult::kFirstStoreCertified)
      return Fail(true);
    if (!CaptureStackCertificate(
            static_cast<pid_t>(stopped_tid), mem_,
            library_base_ + rbx::kCallerReturnRva, &first_stack_) ||
        first_stack_.count != 1)
      return Fail(false);
    const rbx::StoreCertificate certificate{
        rbx::StoreIdentity::kFirstField,
        library_base_ + rbx::kCallerReturnRva,
        first_stack_.count,
        watch_layout_.dr0_rbx_first,
        rbx_owner_, stopped_tid};
    const rbx::Io io{&mem_, &ReadProcess, &WriteProcess};
    if (rbx_transaction_.OnFirstStore(io, certificate, stopped_tid) !=
        rbx::RemoteResult::kFirstStoreCertified)
      return Fail(rbx_transaction_.mutation_uncertain());
    std::uint32_t actions = 0;
    if (!schedule::AdvanceInterlocked(
            &interlocked_, schedule::InterlockedEvent::kRbxFirstStore,
            &actions) ||
        actions != schedule::kActionSwapRbxFirstForSecond)
      return Fail(true);
    return true;
  }

  bool OnRbxSecondStore(std::uint32_t stopped_tid) {
    if (!ActiveOwner(stopped_tid)) return Fail(true);
    StackCertificate second_stack{};
    if (!CaptureStackCertificate(
            static_cast<pid_t>(stopped_tid), mem_,
            library_base_ + rbx::kCallerReturnRva, &second_stack) ||
        !SameStackCertificate(first_stack_, second_stack))
      return Fail(false);
    const rbx::StoreCertificate certificate{
        rbx::StoreIdentity::kSecondField,
        library_base_ + rbx::kCallerReturnRva,
        second_stack.count,
        watch_layout_.dr0_rbx_second,
        rbx_owner_, stopped_tid};
    const rbx::Io io{&mem_, &ReadProcess, &WriteProcess};
    const rbx::RemoteResult result =
        rbx_transaction_.OnSecondStore(io, certificate, stopped_tid);
    if ((result != rbx::RemoteResult::kOverridden &&
         result != rbx::RemoteResult::kNaturalSkipped) ||
        rbx_transaction_.FinishFrame() != result)
      return Fail(rbx_transaction_.mutation_uncertain());
    std::uint32_t actions = 0;
    if (!schedule::AdvanceInterlocked(
            &interlocked_, schedule::InterlockedEvent::kRbxSecondStore,
            &actions) ||
        actions != (schedule::kActionApplyRbxTail |
                    schedule::kActionResetRbxFirstWatch))
      return Fail(true);
    ++statistics_.rbx_pairs;
    first_stack_ = {};
    return true;
  }

  bool OnAngularAux(std::uint32_t stopped_tid) {
    if (!ActiveOwner(stopped_tid)) return Fail(true);
    const yaw::Io io{&mem_, &ReadProcess, &WriteProcess};
    if (yaw_session_.frame_armed) {
      std::uintptr_t vptr = 0;
      if (!ReadProcess(&mem_, yaw_object_, &vptr, sizeof(vptr)))
        return Fail(true);
      if (vptr == yaw_session_.original_vptr) {
        if (!yaw::ReconcileCompletedWindow(io, stopped_tid, false,
                                           &yaw_session_))
          return Fail(true);
        std::uint32_t actions = 0;
        if (!schedule::AdvanceInterlocked(
                &interlocked_,
                schedule::InterlockedEvent::kAngularPayloadComplete,
                &actions))
          return Fail(true);
        ++statistics_.yaw_transactions;
      } else if (vptr == yaw_session_.prepared.shadow_vptr) {
        ++statistics_.passive_aux_hits;
        return true;
      } else {
        return Fail(true);
      }
    }
    if (interlocked_.barrel.angular_transactions >=
        schedule::kMaximumTransactionsPerFrame) {
      ++statistics_.passive_aux_hits;
      return true;
    }
    std::uint32_t actions = 0;
    if (!schedule::AdvanceInterlocked(
            &interlocked_,
            schedule::InterlockedEvent::kAngularSetterCertificate,
            &actions) ||
        actions != schedule::kActionArmAngularTransientWindow ||
        !yaw::ArmFrame(io, current_frame_, yaw_targets_[current_frame_],
                       stopped_tid, &yaw_session_))
      return Fail(true);
    return true;
  }

  bool OnF64(std::uint32_t stopped_tid, std::uint16_t callback_flags,
             bool final_frame) {
    if (!ActiveOwner(stopped_tid) || (callback_flags & 0xffu) != 1u)
      return Fail(true);
    const yaw::Io io{&mem_, &ReadProcess, &WriteProcess};
    std::uint32_t actions = 0;
    if (yaw_session_.frame_armed) {
      std::uintptr_t vptr = 0;
      if (!ReadProcess(&mem_, yaw_object_, &vptr, sizeof(vptr)))
        return Fail(true);
      if (vptr == yaw_session_.original_vptr) {
        if (!yaw::ObserveFrameTerminalAtF64(io, stopped_tid,
                                            &yaw_session_) ||
            !schedule::AdvanceInterlocked(
                &interlocked_,
                schedule::InterlockedEvent::kAngularPayloadComplete,
                &actions))
          return Fail(true);
        ++statistics_.yaw_transactions;
      } else if (vptr == yaw_session_.prepared.shadow_vptr) {
        if (!yaw::CancelAbsentFrameAtF64(io, stopped_tid, &yaw_session_) ||
            !schedule::AdvanceInterlocked(
                &interlocked_,
                schedule::InterlockedEvent::kAngularAbsentCancelled,
                &actions))
          return Fail(true);
        ++statistics_.yaw_absent_cancellations;
      } else {
        return Fail(true);
      }
    } else if (yaw_session_.frame_window_ever_armed) {
      if (!yaw::ObserveSettledAtF64(io, stopped_tid, &yaw_session_))
        return Fail(true);
    } else if (!yaw::ObserveIdleAtF64(io, stopped_tid, &yaw_session_)) {
      return Fail(true);
    }
    if (!schedule::AdvanceInterlocked(
            &interlocked_,
            schedule::InterlockedEvent::kAngularTerminalReceipt,
            &actions) ||
        !schedule::AdvanceInterlocked(
            &interlocked_, schedule::InterlockedEvent::kF64, &actions) ||
        actions != (schedule::kActionVerifyBarrelTerminal |
                    schedule::kActionRestorePostF64WatchSet))
      return Fail(true);
    if (final_frame &&
        !yaw::Finish(io, stopped_tid,
                     yaw_session_.completed_transactions != 0,
                     &yaw_session_))
      return Fail(true);
    if (final_frame) transport_finished_ = true;
    return true;
  }

  bool OnWorldCommit(std::uint32_t frame_index, bool callback_close_seen,
                     bool deferred_callback_clear_seen) {
    if (!frame_active_ || frame_index != current_frame_ ||
        !callback_close_seen || !deferred_callback_clear_seen ||
        !AdvanceSimple(schedule::InterlockedEvent::kCallbackClose) ||
        !AdvanceSimple(schedule::InterlockedEvent::kDeferredCallbackClear) ||
        !AdvanceSimple(schedule::InterlockedEvent::kWorldCommit) ||
        !schedule::InterlockedTerminal(interlocked_))
      return Fail(true);
    ++statistics_.committed_frames;
    frame_active_ = false;
    return true;
  }

  const watch::Layout& watch_layout() const { return watch_layout_; }
  const Statistics& statistics() const { return statistics_; }
  bool waiting_rbx_second() const {
    return interlocked_.barrel.rbx_phase == schedule::RbxPhase::kAwaitSecond;
  }
  bool frame_active() const { return frame_active_; }
  bool faulted() const { return faulted_; }
  bool requires_process_stop() const {
    return mutation_uncertain_ || yaw_session_.frame_armed ||
           yaw_session_.faulted;
  }
  bool Success(std::size_t expected_frames) const {
    return configured_ && !faulted_ && !frame_active_ && transport_finished_ &&
           statistics_.committed_frames == expected_frames;
  }

 private:
  bool ActiveOwner(std::uint32_t stopped_tid) const {
    return frame_active_ && stopped_tid != 0 && stopped_tid == expected_tid_;
  }

  semantic::ActiveFrame ActiveFrame(std::uint32_t frame_index) const {
    semantic::ActiveFrame active{};
    active.has_value = true;
    active.permit = {generation_, frame_index, 0};
    const auto& frame = (*frames_)[frame_index];
    active.skip_override_flags = frame.skip_override_flags;
    std::memcpy(active.angular_bits, frame.barrel_angular_velocity,
                sizeof(active.angular_bits));
    std::memcpy(active.rbx_bits, frame.barrel_rbx,
                sizeof(active.rbx_bits));
    return active;
  }

  bool AdvanceSimple(schedule::InterlockedEvent event) {
    if (faulted_) return false;
    std::uint32_t actions = 0;
    return schedule::AdvanceInterlocked(&interlocked_, event, &actions) &&
           actions == schedule::kActionNone;
  }

  bool Fail(bool mutation_uncertain) {
    faulted_ = true;
    mutation_uncertain_ = mutation_uncertain_ || mutation_uncertain;
    ++statistics_.failures;
    return false;
  }

  pid_t pid_{};
  int mem_{-1};
  std::uintptr_t library_base_{};
  std::uintptr_t yaw_object_{};
  std::uintptr_t rbx_owner_{};
  std::uintptr_t native_angular_{};
  std::uint32_t expected_tid_{};
  std::uint32_t generation_{};
  std::uint32_t current_frame_{};
  const std::vector<recording::RecordingFrameV1>* frames_{};
  std::array<std::uint8_t, 32> recording_sha256_{};
  std::vector<yaw_protocol::FrameTarget> yaw_targets_;
  yaw::elf::Layout payload_{};
  yaw::Session yaw_session_{};
  watch::Layout watch_layout_{};
  schedule::InterlockedState interlocked_{};
  rbx::RemotePairTransaction rbx_transaction_{};
  StackCertificate first_stack_{};
  Statistics statistics_{};
  bool configured_{};
  bool frame_active_{};
  bool transport_finished_{};
  bool faulted_{};
  bool mutation_uncertain_{};
};

}  // namespace a9tas::barrel_successor_runtime_v1
