#include "vehicle_camera_phase_observer_protocol_v1.h"

#include <cstddef>
#include <cstdint>

namespace phase = a9tas::vehicle_camera_phase_observer_v1;

static_assert(phase::kVersion == 1);
static_assert(phase::kMaximumEvents == 16384);
static_assert(phase::kVehicleBeforeFinalWriter == 1);
static_assert(phase::kVehicleAfterFinalWriter == 2);
static_assert(phase::kCameraAfterOriginal == 4);
static_assert(phase::kRejectedBuildOnly == -100);
static_assert(sizeof(phase::Event) == 80);
static_assert(sizeof(phase::Control) == 128);
static_assert(sizeof(phase::Evidence) == 192);
static_assert(offsetof(phase::Event, vehicle_pose_hash) == 40);
static_assert(offsetof(phase::Event, camera_fov_bits) == 72);
static_assert(offsetof(phase::Control, original_camera_callback) == 48);
static_assert(offsetof(phase::Evidence, camera_frames) == 104);

int main() {
  phase::Control control{};
  phase::Evidence evidence{};
  phase::Event event{};
  return static_cast<int>(control.flags + evidence.camera_frames + event.kind);
}
