#include "controller_shadow_transaction_core_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace core = a9tas::controller_shadow_transaction_core_v1;
namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace writer = a9tas::final_writer_replay_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

namespace {

struct FakeMemory {
  std::vector<std::uint8_t> bytes;
  std::uint64_t writes{};
  std::uintptr_t last_write{};
  std::size_t last_write_size{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
  auto* memory = static_cast<FakeMemory*>(context);
  if (output == nullptr || address > memory->bytes.size() ||
      size > memory->bytes.size() - address)
    return false;
  std::memcpy(output, memory->bytes.data() + address, size);
  return true;
}

bool WriteVerified(void* context, std::uintptr_t address, const void* input,
                   std::size_t size) {
  auto* memory = static_cast<FakeMemory*>(context);
  if (input == nullptr || address > memory->bytes.size() ||
      size > memory->bytes.size() - address)
    return false;
  std::memcpy(memory->bytes.data() + address, input, size);
  if (std::memcmp(memory->bytes.data() + address, input, size) != 0)
    return false;
  ++memory->writes;
  memory->last_write = address;
  memory->last_write_size = size;
  return true;
}

template <typename T>
void Put(FakeMemory* memory, std::uintptr_t address, const T& value) {
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
  static constexpr std::uint32_t kSession = 0x1234;
  static constexpr std::uint32_t kTid = 77;
  static constexpr std::uint32_t kFrameCount = 5;

  FakeMemory memory{std::vector<std::uint8_t>(0x900000)};
  core::Backend backend{&memory, &Read, &WriteVerified};
  core::Guard guard{true, true, true, true, true};
  core::PayloadLayout payload_layout{
      kWrapper,
      kShadow,
      kControl,
      kEvidence,
      kFrames,
      payload::kControllerShadowSize,
      sizeof(payload::Control),
      sizeof(payload::Evidence),
      sizeof(recording::RecordingFrameV1),
      payload::kMaximumFrames,
  };
  core::RuntimeLayout runtime{kBase,          kController, kWriterControl,
                              kWriterEvidence, kMailbox,    kOwner,
                              kSession,       kTid};
  recording::RecordingFrameV1 frames[kFrameCount]{};
  std::uint8_t recording_hash[32]{};

  Fixture() {
    std::uint8_t table[payload::kControllerShadowSize]{};
    std::memcpy(table, core::kExpectedPrefixWords,
                sizeof(core::kExpectedPrefixWords));
    for (std::size_t index = 0;
         index < std::size(core::kExpectedPrimarySlotRvas); ++index) {
      const std::uintptr_t value =
          kBase + core::kExpectedPrimarySlotRvas[index];
      std::memcpy(table + payload::kControllerPrefixSize +
                      index * sizeof(value),
                  &value, sizeof(value));
    }
    std::memcpy(memory.bytes.data() + kBase + payload::kControllerPrefixRva,
                table, sizeof(table));
    const std::uintptr_t controller_vptr =
        kBase + payload::kControllerAddressPointRva;
    const std::uintptr_t source_vptr =
        kBase + payload::kKeyboardSourceAddressPointRva;
    Put(&memory, kController, controller_vptr);
    Put(&memory, kController + payload::kControllerSourceOffset, kSource);
    Put(&memory, kSource, source_vptr);

    payload::Control payload_control{};
    std::memcpy(payload_control.magic, payload::kControlMagic,
                sizeof(payload::kControlMagic));
    payload_control.version = payload::kProtocolVersion;
    payload_control.size = sizeof(payload_control);
    Put(&memory, kControl, payload_control);
    payload::Evidence payload_evidence{};
    std::memcpy(payload_evidence.magic, payload::kEvidenceMagic,
                sizeof(payload::kEvidenceMagic));
    payload_evidence.version = payload::kProtocolVersion;
    payload_evidence.size = sizeof(payload_evidence);
    Put(&memory, kEvidence, payload_evidence);
    memory.bytes[kShadow] = 0xA9;

    writer::Control writer_control{};
    std::memcpy(writer_control.magic, writer::kControlMagic,
                sizeof(writer::kControlMagic));
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
    std::memcpy(writer_evidence.magic, writer::kEvidenceMagic,
                sizeof(writer::kEvidenceMagic));
    writer_evidence.version = writer::kProtocolVersion;
    writer_evidence.size = sizeof(writer_evidence);
    Put(&memory, kWriterEvidence, writer_evidence);

    mailbox::Mailbox action_mailbox{};
    mailbox::Initialize(&action_mailbox);
    mailbox::StoreRelease(&action_mailbox.session_control,
                          (static_cast<std::uint64_t>(kSession) << 1) |
                              mailbox::kArmedBit);
    Put(&memory, kMailbox, action_mailbox);

    for (std::uint32_t index = 0; index < kFrameCount; ++index) {
      frames[index].tick = index;
      frames[index].flags = recording::kRequiredFrameFlags;
      frames[index].skip_override_flags =
          recording::kSkipAccelerator | recording::kSkipBarrelAngular |
          recording::kSkipBarrelRbx | recording::kSkipRespawnButton;
      frames[index].steering = index == 2 ? 0.5f : 0.0f;
      frames[index].brake = index == 3 ? -1.0f : 0.0f;
      frames[index].nitro_activation_count = index == 4 ? 2u : 0u;
    }
    recording_hash[0] = 0x72;
  }
};

bool HappyPathCommitAndRollback() {
  Fixture fixture;
  core::Prepared prepared{};
  if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                    fixture.runtime, fixture.frames, Fixture::kFrameCount,
                    fixture.recording_hash, &prepared) != core::Result::kOk)
    return false;
  if (fixture.memory.writes != 0 ||
      prepared.shadow_controller_vptr !=
          Fixture::kShadow + payload::kControllerPrefixSize)
    return false;
  std::uintptr_t patched_slot = 0;
  std::memcpy(&patched_slot,
              prepared.shadow.data() + payload::kControllerPrefixSize +
                  payload::kControllerUpdateSlotOffset,
              sizeof(patched_slot));
  if (patched_slot != Fixture::kWrapper) return false;

  if (core::Stage(fixture.backend, fixture.guard, fixture.payload_layout,
                  prepared, fixture.runtime,
                  fixture.frames) != core::Result::kOk)
    return false;
  std::uintptr_t current_vptr = 0;
  if (!Read(&fixture.memory, Fixture::kController, &current_vptr,
            sizeof(current_vptr)) ||
      current_vptr != prepared.original_controller_vptr)
    return false;
  if (core::Commit(fixture.backend, fixture.guard, fixture.payload_layout,
                   fixture.runtime, prepared,
                   fixture.frames) != core::Result::kOk ||
      fixture.memory.last_write != Fixture::kController ||
      fixture.memory.last_write_size != sizeof(std::uintptr_t))
    return false;
  if (!Read(&fixture.memory, Fixture::kController, &current_vptr,
            sizeof(current_vptr)) ||
      current_vptr != prepared.shadow_controller_vptr)
    return false;

  if (core::Rollback(fixture.backend, fixture.guard, fixture.payload_layout,
                     fixture.runtime, prepared) != core::Result::kOk)
    return false;
  payload::Control disabled{};
  return Read(&fixture.memory, Fixture::kController, &current_vptr,
              sizeof(current_vptr)) &&
         current_vptr == prepared.original_controller_vptr &&
         Read(&fixture.memory, Fixture::kControl, &disabled,
              sizeof(disabled)) &&
         disabled.flags == 0;
}

bool PrepareFailuresAreReadOnly() {
  {
    Fixture fixture;
    const std::uintptr_t bad_source_vptr = 0xDEADBEEF;
    Put(&fixture.memory, Fixture::kSource, bad_source_vptr);
    core::Prepared prepared{};
    if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                      fixture.runtime, fixture.frames, Fixture::kFrameCount,
                      fixture.recording_hash, &prepared) !=
            core::Result::kSourceIdentityMismatch ||
        fixture.memory.writes != 0)
      return false;
  }
  {
    Fixture fixture;
    fixture.frames[0].skip_override_flags |= recording::kSkipSteer;
    core::Prepared prepared{};
    if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                      fixture.runtime, fixture.frames, Fixture::kFrameCount,
                      fixture.recording_hash, &prepared) !=
            core::Result::kRecordingRejected ||
        fixture.memory.writes != 0)
      return false;
  }
  {
    Fixture fixture;
    writer::Control control{};
    if (!Read(&fixture.memory, Fixture::kWriterControl, &control,
              sizeof(control)))
      return false;
    control.reserved[0] = writer::FramePermit(0);
    Put(&fixture.memory, Fixture::kWriterControl, control);
    core::Prepared prepared{};
    if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                      fixture.runtime, fixture.frames, Fixture::kFrameCount,
                      fixture.recording_hash, &prepared) !=
            core::Result::kWriterNotReady ||
        fixture.memory.writes != 0)
      return false;
  }
  return true;
}

bool ForeignVptrRollbackRefused() {
  Fixture fixture;
  core::Prepared prepared{};
  if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                    fixture.runtime, fixture.frames, Fixture::kFrameCount,
                    fixture.recording_hash, &prepared) != core::Result::kOk)
    return false;
  const std::uintptr_t foreign = 0xCAFEBABE;
  Put(&fixture.memory, Fixture::kController, foreign);
  const std::uint64_t writes_before = fixture.memory.writes;
  return core::Rollback(fixture.backend, fixture.guard, fixture.payload_layout,
                        fixture.runtime, prepared) ==
             core::Result::kRollbackForeignVptr &&
         fixture.memory.writes == writes_before;
}

bool CommitRejectsStagedTamper() {
  Fixture fixture;
  core::Prepared prepared{};
  if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                    fixture.runtime, fixture.frames, Fixture::kFrameCount,
                    fixture.recording_hash, &prepared) != core::Result::kOk ||
      core::Stage(fixture.backend, fixture.guard, fixture.payload_layout,
                  prepared, fixture.runtime,
                  fixture.frames) != core::Result::kOk)
    return false;
  fixture.memory.bytes[Fixture::kShadow + payload::kControllerPrefixSize] ^=
      0x01;
  const std::uint64_t writes_before = fixture.memory.writes;
  return core::Commit(fixture.backend, fixture.guard, fixture.payload_layout,
                      fixture.runtime, prepared, fixture.frames) ==
             core::Result::kCommitRevalidationFailed &&
         fixture.memory.writes == writes_before;
}

bool StageRevalidatesMutableFrames() {
  Fixture fixture;
  core::Prepared prepared{};
  if (core::Prepare(fixture.backend, fixture.guard, fixture.payload_layout,
                    fixture.runtime, fixture.frames, Fixture::kFrameCount,
                    fixture.recording_hash, &prepared) != core::Result::kOk)
    return false;
  fixture.frames[1].steering = 2.0f;
  return core::Stage(fixture.backend, fixture.guard, fixture.payload_layout,
                     prepared, fixture.runtime, fixture.frames) ==
             core::Result::kInvalidArgument &&
         fixture.memory.writes == 0;
}

}  // namespace

int main() {
  return HappyPathCommitAndRollback() && PrepareFailuresAreReadOnly() &&
                 ForeignVptrRollbackRefused() && CommitRejectsStagedTamper() &&
                 StageRevalidatesMutableFrames()
             ? 0
             : 1;
}
