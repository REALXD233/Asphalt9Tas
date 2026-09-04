#include "g4_multi_hook_runtime_v1.h"

#include <cstdio>
#include <cstring>

namespace protocol = a9tas::g4_multi_hook_runtime_v1;

int main() {
  protocol::Control control{};
  std::memcpy(control.magic, protocol::kControlMagic,
              sizeof(protocol::kControlMagic));
  control.version = protocol::kVersion;
  control.size = sizeof(control);
  control.mode = static_cast<std::uint32_t>(protocol::RunMode::kReplay);
  control.frame_limit = 900;
  control.replay_frame_count = 900;
  control.replay_interval_count = 987;
  control.fixed_delta_us = 16667;
  const bool passed = sizeof(control) == 512 &&
                      sizeof(protocol::Evidence) == 768 &&
                       protocol::kVersion == 10 &&
                       std::memcmp(protocol::kControlMagic, "A9G4C10", 7) == 0 &&
                       std::memcmp(protocol::kEvidenceMagic, "A9G4E10", 7) == 0 &&
                      protocol::kHookCount == 7 &&
                      protocol::kSetterCount == 3 &&
                      protocol::kInstalledSetterCount == 2 &&
                       protocol::kMaximumFrames == 7200 &&
                       protocol::kMaximumIntervalSamples >= 987 &&
                       protocol::ArmFrameLimitValid(
                           7200, protocol::RunMode::kRecord,
                           protocol::CompletionPolicy::kRaceLifecycle,
                           false) &&
                       protocol::ArmFrameLimitValid(
                           7200, protocol::RunMode::kReplay,
                           protocol::CompletionPolicy::kFixedFrameLimit,
                           false) &&
                       protocol::ArmFrameLimitValid(
                           5002, protocol::RunMode::kReplay,
                           protocol::CompletionPolicy::kFixedFrameLimit,
                           false) &&
                       !protocol::ArmFrameLimitValid(
                           5002, protocol::RunMode::kRecord,
                           protocol::CompletionPolicy::kFixedFrameLimit,
                           false) &&
                       protocol::ArmFrameLimitValid(
                           429, protocol::RunMode::kReplay,
                           protocol::CompletionPolicy::kFixedFrameLimit,
                           true) &&
                       protocol::ArmFrameLimitValid(
                           7200, protocol::RunMode::kRecord,
                           protocol::CompletionPolicy::kRaceLifecycle,
                           true) &&
                       !protocol::ArmFrameLimitValid(
                           0, protocol::RunMode::kRecord,
                           protocol::CompletionPolicy::kRaceLifecycle,
                           false) &&
                       !protocol::ArmFrameLimitValid(
                           7201, protocol::RunMode::kRecord,
                           protocol::CompletionPolicy::kRaceLifecycle,
                           false) &&
                      sizeof(protocol::RecordingBundleHeaderV1) == 64 &&
                      protocol::kLegacyRecordingBundleVersion == 2 &&
                      protocol::kRecordingBundleVersion == 3 &&
                      protocol::kLegacyRecordingBundleFlags == 0x0f &&
                      protocol::kRecordingBundleFlags == 0x1f &&
                      control.replay_interval_count == 987 &&
                      offsetof(protocol::Control, expected_nitro_state) == 344 &&
                      offsetof(protocol::Control, setter_slot) == 352 &&
                      offsetof(protocol::Control, setter_original) == 376 &&
                      offsetof(protocol::Control, setter_wrapper) == 400 &&
                      protocol::kAdjustedSetterVtableRva == 0x7EED9E0 &&
                      protocol::kAdjustedSetterObjectOffset == 0x2A80 &&
                      protocol::kAdjustedSetterThisAdjustment == -0x2A80 &&
                      protocol::kSetterSlotOffsets[protocol::kBrakeSetter] == 0x2A8 &&
                      protocol::kSetterSlotOffsets[protocol::kSteeringSetter] == 0x2B0 &&
                      protocol::kSetterOriginalRvas[protocol::kBrakeSetter] == 0x36934B8 &&
                      protocol::kSetterOriginalRvas[protocol::kSteeringSetter] == 0x36934E0 &&
                      offsetof(protocol::Control, expected_setter_object) == 424 &&
                      offsetof(protocol::Control, expected_setter_vptr) == 432 &&
                      offsetof(protocol::Control, expected_backend_interface) == 440 &&
                      offsetof(protocol::Control,
                               expected_physics_velocity_interface) == 448 &&
                      offsetof(protocol::Control, expected_native_body) == 456 &&
                      protocol::kNitroDispatchThunkRva == 0x3674E50 &&
                      protocol::kNitroStateEntryRva == 0x36D8524 &&
                      protocol::kBarrelRollEntryRva == 0x369DE4C &&
                      protocol::kBarrelYawEntryRva == 0x369E30C &&
                      protocol::kBarrelRbxOffset == 0x1968 &&
                      protocol::kBarrelBackendOffset == 0x18 &&
                      protocol::kBarrelBackendNativeBodyOffset == 0x90 &&
                      offsetof(protocol::Control, expected_barrel_owner) == 464 &&
                       offsetof(protocol::Control,
                                expected_barrel_owner_vptr) == 472 &&
                       offsetof(protocol::Control, archived_generation) == 480 &&
                       offsetof(protocol::Control, archived_frame_count) == 488 &&
                       offsetof(protocol::Control, archived_interval_count) == 496 &&
                       static_cast<std::uint32_t>(
                           protocol::Command::kRearmArchivedReplay) == 4 &&
                       static_cast<std::uint32_t>(
                           protocol::Command::kRearmArchivedRecord) == 5 &&
                       static_cast<std::uint32_t>(
                           protocol::Command::kSealPausedRecord) == 6 &&
                       static_cast<std::uint32_t>(
                           protocol::Command::kRearmPausedReplayRecord) == 7 &&
                       static_cast<std::uint32_t>(
                           protocol::CompletionReason::kManualCheckpoint) == 3;
  std::printf("G4_MULTI_HOOK_PROTOCOL_SELFTEST passed=%d control=%zu "
               "evidence=%zu hooks=%u max_frames=%u max_intervals=%u "
               "extended_lifecycle_only=1\n",
               passed ? 1 : 0, sizeof(control), sizeof(protocol::Evidence),
               protocol::kHookCount, protocol::kMaximumFrames,
               protocol::kMaximumIntervalSamples);
  return passed ? 0 : 1;
}
