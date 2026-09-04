#include "controller_shadow_host_session_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace session = a9tas::controller_shadow_host_session_v1;
namespace tx = a9tas::controller_shadow_transaction_core_v1;
namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer = a9tas::final_writer_replay_v1;
namespace mailbox = a9tas::natural_action_callback_v1;

namespace {

struct Memory {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x900000);
  std::uintptr_t reject_write{};
  std::uint64_t writes{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
  auto* memory = static_cast<Memory*>(context);
  if (memory == nullptr || output == nullptr ||
      address > memory->bytes.size() || size > memory->bytes.size() - address)
    return false;
  std::memcpy(output, memory->bytes.data() + address, size);
  return true;
}

bool Write(void* context, std::uintptr_t address, const void* input,
           std::size_t size) {
  auto* memory = static_cast<Memory*>(context);
  if (memory == nullptr || input == nullptr || address == memory->reject_write ||
      address > memory->bytes.size() || size > memory->bytes.size() - address)
    return false;
  std::memcpy(memory->bytes.data() + address, input, size);
  ++memory->writes;
  return std::memcmp(memory->bytes.data() + address, input, size) == 0;
}

template <typename T>
void Put(Memory* memory, std::uintptr_t address, const T& value) {
  std::memcpy(memory->bytes.data() + address, &value, sizeof(value));
}

struct Fixture {
  static constexpr std::uintptr_t kBase = 0x100000;
  static constexpr std::uintptr_t kController = 0x300000;
  static constexpr std::uintptr_t kSource = 0x310000;
  static constexpr std::uintptr_t kWriterControl = 0x400000;
  static constexpr std::uintptr_t kWriterEvidence = 0x401000;
  static constexpr std::uintptr_t kMailbox = 0x402000;
  static constexpr std::uintptr_t kOwner = 0x500000;
  static constexpr std::uintptr_t kWrapper = 0x700000;
  static constexpr std::uintptr_t kShadow = 0x800000;
  static constexpr std::uintptr_t kControl = 0x801000;
  static constexpr std::uintptr_t kEvidence = 0x802000;
  static constexpr std::uintptr_t kFrames = 0x803000;
  static constexpr std::uint32_t kFrameCount = 2;

  Memory memory{};
  tx::Backend backend{&memory, &Read, &Write};
  tx::Guard guard{true, true, true, true, true};
  session::elf::Layout elf_layout{};
  tx::RuntimeLayout runtime{kBase, kController, kWriterControl,
                            kWriterEvidence, kMailbox, kOwner, 0x1234, 77};
  recording::RecordingFrameV1 frames[kFrameCount]{};
  std::uint8_t hash[32]{1};

  Fixture() {
    elf_layout.load_bias = 0x600000;
    elf_layout.wrapper = kWrapper;
    elf_layout.shadow = kShadow;
    elf_layout.control = kControl;
    elf_layout.evidence = kEvidence;
    elf_layout.frames = kFrames;
    elf_layout.shadow_size = payload::kControllerShadowSize;
    elf_layout.control_size = sizeof(payload::Control);
    elf_layout.evidence_size = sizeof(payload::Evidence);
    elf_layout.frame_size = sizeof(recording::RecordingFrameV1);
    elf_layout.frame_capacity = payload::kMaximumFrames;
    elf_layout.prefix_size = payload::kControllerPrefixSize;
    elf_layout.update_slot = payload::kControllerUpdateSlotOffset;
    std::memcpy(elf_layout.file_sha256, session::elf::kExpectedSha256, 32);

    std::array<std::uint8_t, payload::kControllerShadowSize> table{};
    std::memcpy(table.data(), tx::kExpectedPrefixWords,
                sizeof(tx::kExpectedPrefixWords));
    for (std::size_t index = 0;
         index < std::size(tx::kExpectedPrimarySlotRvas); ++index) {
      const std::uintptr_t slot =
          kBase + tx::kExpectedPrimarySlotRvas[index];
      std::memcpy(table.data() + payload::kControllerPrefixSize +
                      index * sizeof(slot),
                  &slot, sizeof(slot));
    }
    std::memcpy(memory.bytes.data() + kBase + payload::kControllerPrefixRva,
                table.data(), table.size());
    const std::uintptr_t controller_vptr =
        kBase + payload::kControllerAddressPointRva;
    const std::uintptr_t source_vptr =
        kBase + payload::kKeyboardSourceAddressPointRva;
    Put(&memory, kController, controller_vptr);
    Put(&memory, kController + payload::kControllerSourceOffset, kSource);
    Put(&memory, kSource, source_vptr);

    payload::Control controller_control{};
    std::memcpy(controller_control.magic, payload::kControlMagic, 8);
    controller_control.version = payload::kProtocolVersion;
    controller_control.size = sizeof(controller_control);
    Put(&memory, kControl, controller_control);
    payload::Evidence controller_evidence{};
    std::memcpy(controller_evidence.magic, payload::kEvidenceMagic, 8);
    controller_evidence.version = payload::kProtocolVersion;
    controller_evidence.size = sizeof(controller_evidence);
    Put(&memory, kEvidence, controller_evidence);
    memory.bytes[kShadow] = 0xA9;

    writer::Control writer_control{};
    std::memcpy(writer_control.magic, writer::kControlMagic, 8);
    writer_control.version = writer::kProtocolVersion;
    writer_control.size = sizeof(writer_control);
    writer_control.flags =
        writer::kControlConfigured | writer::kControlTargetsLoaded;
    writer_control.frame_count = kFrameCount;
    writer_control.expected_object = kOwner;
    writer_control.original_vptr = 0x1111;
    writer_control.shadow_vptr = 0x2222;
    writer_control.original_callback = 0x3333;
    writer_control.native_pose = 0x4444;
    writer_control.native_linear = 0x5555;
    Put(&memory, kWriterControl, writer_control);
    writer::Evidence writer_evidence{};
    std::memcpy(writer_evidence.magic, writer::kEvidenceMagic, 8);
    writer_evidence.version = writer::kProtocolVersion;
    writer_evidence.size = sizeof(writer_evidence);
    Put(&memory, kWriterEvidence, writer_evidence);
    mailbox::Mailbox action{};
    mailbox::Initialize(&action);
    mailbox::StoreRelease(&action.session_control,
                          (static_cast<std::uint64_t>(runtime.session_id) << 1) |
                              mailbox::kArmedBit);
    Put(&memory, kMailbox, action);
    for (std::uint32_t index = 0; index < kFrameCount; ++index) {
      frames[index].tick = index;
      frames[index].flags = recording::kRequiredFrameFlags;
      frames[index].skip_override_flags =
          recording::kSkipAccelerator | recording::kSkipBarrelAngular |
          recording::kSkipBarrelRbx | recording::kSkipRespawnButton;
    }
  }
};

bool InstallAndUninstallPass() {
  Fixture fixture;
  session::Session state{};
  if (session::Install(fixture.backend, fixture.guard, fixture.elf_layout,
                       fixture.runtime, fixture.frames, Fixture::kFrameCount,
                       fixture.hash, &state) != session::Result::kOk ||
      state.phase != session::Phase::kInstalled)
    return false;
  std::uintptr_t current = 0;
  if (!Read(&fixture.memory, Fixture::kController, &current, sizeof(current)) ||
      current != state.prepared.shadow_controller_vptr)
    return false;
  return session::Uninstall(fixture.backend, fixture.guard, &state) ==
             session::Result::kOk &&
         state.phase == session::Phase::kClean &&
         Read(&fixture.memory, Fixture::kController, &current,
              sizeof(current)) &&
         current == state.prepared.original_controller_vptr;
}

bool CommitFailureCleansAutomatically() {
  Fixture fixture;
  fixture.memory.reject_write = Fixture::kController;
  session::Session state{};
  const session::Result result = session::Install(
      fixture.backend, fixture.guard, fixture.elf_layout, fixture.runtime,
      fixture.frames, Fixture::kFrameCount, fixture.hash, &state);
  payload::Control control{};
  return result == session::Result::kCommitRejectedClean &&
         state.phase == session::Phase::kClean &&
         state.commit_result == tx::Result::kVptrCommitFailed &&
         state.rollback_result == tx::Result::kOk &&
         Read(&fixture.memory, Fixture::kControl, &control, sizeof(control)) &&
         control.flags == 0;
}

bool InvalidPayloadIsReadOnly() {
  Fixture fixture;
  session::Session state{};
  session::elf::Layout invalid{};
  const std::uint64_t writes = fixture.memory.writes;
  return session::Install(fixture.backend, fixture.guard, invalid,
                          fixture.runtime, fixture.frames,
                          Fixture::kFrameCount, fixture.hash, &state) ==
             session::Result::kPayloadBindingRejected &&
         state.phase == session::Phase::kEmpty &&
         fixture.memory.writes == writes;
}

bool FinalInFlightCompletionPassesWithoutExtraControllerTick() {
  Fixture fixture;
  session::Session state{};
  if (session::Install(fixture.backend, fixture.guard, fixture.elf_layout,
                       fixture.runtime, fixture.frames, Fixture::kFrameCount,
                       fixture.hash, &state) != session::Result::kOk)
    return false;

  payload::Evidence controller_evidence{};
  std::memcpy(controller_evidence.magic, payload::kEvidenceMagic, 8);
  controller_evidence.version = payload::kProtocolVersion;
  controller_evidence.size = sizeof(controller_evidence);
  controller_evidence.wrapper_entries = Fixture::kFrameCount;
  controller_evidence.original_calls = Fixture::kFrameCount;
  controller_evidence.original_returns = Fixture::kFrameCount;
  controller_evidence.source_swaps = Fixture::kFrameCount;
  controller_evidence.source_restores = Fixture::kFrameCount;
  controller_evidence.selected_frames = Fixture::kFrameCount;
  controller_evidence.completed_frames = Fixture::kFrameCount - 1u;
  controller_evidence.last_controller = Fixture::kController;
  controller_evidence.observed_source_vptr =
      Fixture::kBase + payload::kKeyboardSourceAddressPointRva;
  controller_evidence.last_selected_frame = Fixture::kFrameCount - 1u;
  controller_evidence.last_tid = 77;
  controller_evidence.last_status = payload::kRunning;
  controller_evidence.last_coordinator_result =
      static_cast<std::int32_t>(
          a9tas::in_process_tick_coordinator_v1::Result::kFrameSelected);
  controller_evidence.coordinator_phase = static_cast<std::uint32_t>(
      a9tas::in_process_tick_coordinator_v1::Phase::kInFlight);
  controller_evidence.next_frame = Fixture::kFrameCount - 1u;
  Put(&fixture.memory, Fixture::kEvidence, controller_evidence);

  writer::Control writer_control{};
  Read(&fixture.memory, Fixture::kWriterControl, &writer_control,
       sizeof(writer_control));
  writer_control.reserved[0] = writer::kFramePermitDisarmed;
  Put(&fixture.memory, Fixture::kWriterControl, writer_control);
  writer::Evidence writer_evidence{};
  std::memcpy(writer_evidence.magic, writer::kEvidenceMagic, 8);
  writer_evidence.version = writer::kProtocolVersion;
  writer_evidence.size = sizeof(writer_evidence);
  writer_evidence.processed_frames = Fixture::kFrameCount - 1u;
  Put(&fixture.memory, Fixture::kWriterEvidence, writer_evidence);

  mailbox::Mailbox action{};
  Read(&fixture.memory, Fixture::kMailbox, &action, sizeof(action));
  action.completed_sequence = Fixture::kFrameCount;
  action.completed_frame = Fixture::kFrameCount - 1u;
  action.calls_submitted = 0;
  action.result = static_cast<std::int32_t>(mailbox::Result::kCompleted);
  Put(&fixture.memory, Fixture::kMailbox, action);

  session::Completion rejected{};
  if (session::ValidateFinalInFlight(fixture.backend, fixture.frames, state,
                                     &rejected) !=
          session::Result::kCompletionRejected ||
      rejected.controller.completed_frames != Fixture::kFrameCount - 1u ||
      rejected.final_writer.processed_frames != Fixture::kFrameCount - 1u ||
      mailbox::LoadAcquire(&rejected.action_mailbox.completed_sequence) !=
          Fixture::kFrameCount)
    return false;

  writer_evidence.processed_frames = Fixture::kFrameCount;
  Put(&fixture.memory, Fixture::kWriterEvidence, writer_evidence);
  session::Completion completion{};
  const bool accepted = session::ValidateFinalInFlight(
                            fixture.backend, fixture.frames, state,
                            &completion) == session::Result::kOk;
  return accepted &&
         session::Uninstall(fixture.backend, fixture.guard, &state) ==
             session::Result::kOk;
}

}  // namespace

int main() {
  return InstallAndUninstallPass() && CommitFailureCleansAutomatically() &&
                 InvalidPayloadIsReadOnly() &&
                 FinalInFlightCompletionPassesWithoutExtraControllerTick()
             ? 0
             : 1;
}
