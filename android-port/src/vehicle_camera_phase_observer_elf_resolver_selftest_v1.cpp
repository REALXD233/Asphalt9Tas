#include "vehicle_camera_phase_observer_elf_resolver_v1.h"

namespace resolver = a9tas::vehicle_camera_phase_observer_elf_v1;

static_assert(sizeof(resolver::Layout::file_sha256) == 32);
static_assert(resolver::kVehicleWrapperRva == 0x2DB8);
static_assert(resolver::kCameraWrapperRva == 0x32B4);
static_assert(resolver::kExpectedPhaseEventsStorageRva == 0xDA0C0);

int main() {
  resolver::Layout layout{};
  return resolver::Resolve(0, -1, &layout) ? 1 : 0;
}
