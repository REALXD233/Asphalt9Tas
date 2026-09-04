#include "in_process_tick_coordinator_v1.h"

#include <cstdint>
#include <cstring>

namespace coordinator = a9tas::in_process_tick_coordinator_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer = a9tas::final_writer_replay_v1;

namespace {

constexpr std::uint32_t kSession = 0x1234;
constexpr std::uint32_t kTid = 77;
constexpr std::uintptr_t kOwner = 0x11220000;

void InitializeFrame(recording::RecordingFrameV1* frame,
                     std::uint32_t index) {
  std::memset(frame, 0, sizeof(*frame));
  frame->tick = index;
  frame->flags = recording::kRequiredFrameFlags;
  frame->skip_override_flags = recording::kSkipAccelerator |
                               recording::kSkipBarrelAngular |
                               recording::kSkipBarrelRbx |
                               recording::kSkipRespawnButton;
}

void InitializeWriter(writer::Control* control, writer::Evidence* evidence,
                      std::uint32_t frame_count) {
  std::memset(control, 0, sizeof(*control));
  std::memcpy(control->magic, writer::kControlMagic,
              sizeof(writer::kControlMagic));
  control->version = writer::kProtocolVersion;
  control->size = sizeof(*control);
  control->flags =
      writer::kControlConfigured | writer::kControlTargetsLoaded;
  control->frame_count = frame_count;
  control->expected_object = 0x1000;
  control->original_vptr = 0x2000;
  control->shadow_vptr = 0x3000;
  control->original_callback = 0x4000;
  control->native_pose = 0x5000;
  control->native_linear = 0x6000;

  std::memset(evidence, 0, sizeof(*evidence));
  std::memcpy(evidence->magic, writer::kEvidenceMagic,
              sizeof(writer::kEvidenceMagic));
  evidence->version = writer::kProtocolVersion;
  evidence->size = sizeof(*evidence);
}

bool CompleteAction(mailbox::Mailbox* action_mailbox,
                    mailbox::RuntimeState* runtime, std::uint32_t tid,
                    std::uint32_t calls) {
  mailbox::Command command{};
  if (mailbox::ClaimAtNaturalCallback(action_mailbox, runtime, tid,
                                      &command) !=
      mailbox::Result::kClaimed)
    return false;
  return mailbox::CompleteNaturalCallback(action_mailbox, runtime,
                                          command.sequence, calls, true) ==
         mailbox::Result::kCompleted;
}

bool HappyPathAndReceiptBarrier() {
  recording::RecordingFrameV1 frames[2]{};
  InitializeFrame(&frames[0], 0);
  InitializeFrame(&frames[1], 1);
  frames[0].nitro_activation_count = 2;
  frames[1].nitro_activation_count = 2;
  frames[1].skip_override_flags |= recording::kSkipNitroActivation;

  writer::Control writer_control{};
  writer::Evidence writer_evidence{};
  InitializeWriter(&writer_control, &writer_evidence, 2);
  mailbox::Mailbox action_mailbox{};
  mailbox::Initialize(&action_mailbox);
  mailbox::StoreRelease(&action_mailbox.session_control,
                        (static_cast<std::uint64_t>(kSession) << 1) |
                            mailbox::kArmedBit);
  mailbox::RuntimeState action_runtime{};

  coordinator::Config config{2,          kSession, kOwner, kTid,
                             frames,     &writer_control,
                             &writer_evidence, &action_mailbox};
  coordinator::State state{};
  std::uint32_t selected = 99;
  if (coordinator::BeginTick(config, &state, kTid, &selected) !=
          coordinator::Result::kFrameSelected ||
      selected != 0 || state.phase != coordinator::Phase::kInFlight ||
      writer_control.reserved[0] != writer::FramePermit(0))
    return false;

  // A second gameplay tick cannot be selected until both downstream receipts
  // exist.  Use a copy so the intentional fault does not poison the happy
  // path state.
  coordinator::State premature = state;
  if (coordinator::BeginTick(config, &premature, kTid, &selected) !=
          coordinator::Result::kPreviousWriterPending ||
      premature.phase != coordinator::Phase::kFaulted)
    return false;

  if (!CompleteAction(&action_mailbox, &action_runtime, kTid, 2)) return false;
  writer_control.reserved[0] = writer::kFramePermitDisarmed;
  writer_evidence.processed_frames = 1;
  if (coordinator::BeginTick(config, &state, kTid, &selected) !=
          coordinator::Result::kFrameSelected ||
      selected != 1 || state.next_index != 1 ||
      writer_control.reserved[0] != writer::FramePermit(1))
    return false;

  // Skip-Nitro still publishes one ordered frame command, but its expected
  // action receipt contains zero real activation calls.
  if (!CompleteAction(&action_mailbox, &action_runtime, kTid, 0)) return false;
  writer_control.reserved[0] = writer::kFramePermitDisarmed;
  writer_evidence.processed_frames = 2;
  if (coordinator::BeginTick(config, &state, kTid, &selected) !=
          coordinator::Result::kComplete ||
      state.phase != coordinator::Phase::kComplete || state.next_index != 2)
    return false;
  return true;
}

bool RejectsWrongThreadBeforePublication() {
  recording::RecordingFrameV1 frame{};
  InitializeFrame(&frame, 0);
  frame.skip_override_flags |= recording::kSkipNitroActivation;
  writer::Control writer_control{};
  writer::Evidence writer_evidence{};
  InitializeWriter(&writer_control, &writer_evidence, 1);
  mailbox::Mailbox action_mailbox{};
  mailbox::Initialize(&action_mailbox);
  mailbox::StoreRelease(&action_mailbox.session_control,
                        (static_cast<std::uint64_t>(kSession) << 1) |
                            mailbox::kArmedBit);
  coordinator::Config config{1,          kSession, kOwner, kTid,
                             &frame,     &writer_control,
                             &writer_evidence, &action_mailbox};
  coordinator::State state{};
  std::uint32_t selected = 99;
  return coordinator::BeginTick(config, &state, kTid + 1, &selected) ==
             coordinator::Result::kWrongProducerThread &&
         state.phase == coordinator::Phase::kFaulted &&
         action_mailbox.published_selector == 0 &&
         writer_control.reserved[0] == writer::kFramePermitDisarmed;
}

}  // namespace

int main() {
  return HappyPathAndReceiptBarrier() && RejectsWrongThreadBeforePublication()
             ? 0
             : 1;
}
