#include "camera_active_state_resolver_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace resolver = a9tas::camera_active_state_resolver_v1;

namespace {

struct Memory {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x80000);
  bool fail_reads{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
  auto* memory = static_cast<Memory*>(context);
  if (memory == nullptr || memory->fail_reads || output == nullptr ||
      address > memory->bytes.size() || size > memory->bytes.size() - address)
    return false;
  std::memcpy(output, memory->bytes.data() + address, size);
  return true;
}

template <typename T>
void Put(Memory* memory, std::uintptr_t address, const T& value) {
  std::memcpy(memory->bytes.data() + address, &value, sizeof(value));
}

constexpr std::uintptr_t kGameBase = 0x1000;
constexpr std::uintptr_t kRaceView = 0x18000;
constexpr std::uintptr_t kManager = 0x30000;
constexpr std::uintptr_t kCamera = 0x40000;
constexpr std::uintptr_t kActualBase = 0x50000;

void InstallCandidate(Memory* memory, std::uintptr_t race_view,
                      std::uintptr_t manager, std::uintptr_t camera) {
  Put(memory, race_view, kGameBase + resolver::kRaceViewPrimaryVptrRva);
  Put(memory, race_view + resolver::kRaceViewSecondaryOffset,
      kGameBase + resolver::kRaceViewSecondaryVptrRva);
  Put(memory, race_view + resolver::kCameraManagerOffset, manager);
  Put(memory, manager + resolver::kBlendedCameraOffset, camera);
  Put(memory, camera, kGameBase + resolver::kCameraVptrMinimumRva + 0x1000);
  Put(memory, camera + resolver::kActualBasePointerOffset, kActualBase);
}

resolver::CameraState GoodState() {
  resolver::CameraState state{};
  state.fov = 1.1f;
  state.aspect = 16.0f / 9.0f;
  state.position[0] = 10.0f;
  state.position[1] = 2.0f;
  state.position[2] = -5.0f;
  state.rotation_xyzw[3] = 1.0f;
  return state;
}

bool UniqueRouteAndSamplesPass() {
  Memory memory;
  InstallCandidate(&memory, kRaceView, kManager, kCamera);
  const resolver::CameraState state = GoodState();
  Put(&memory, kCamera + resolver::kControllerStateOffset, state);
  Put(&memory, kActualBase + resolver::kCompactStateOffset, state);
  Put(&memory, kActualBase + resolver::kPositionOffset, state.position);
  Put(&memory, kActualBase + resolver::kRotationOffset, state.rotation_xyzw);
  Put(&memory, kActualBase + resolver::kUpstreamFovOffset, state.fov);
  Put(&memory, kActualBase + resolver::kUpstreamAspectOffset, state.aspect);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x70000, true, true, false},
  };
  resolver::Resolution resolution{};
  if (resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                        kGameBase, &resolution) != resolver::Result::kOk ||
      resolution.race_view != kRaceView ||
      resolution.blended_camera != kCamera ||
      resolution.actual_base_candidate != kActualBase)
    return false;
  resolver::StateSamples samples{};
  return resolver::ReadStateSamples({&memory, &Read}, mappings,
                                    std::size(mappings), resolution,
                                    &samples) == resolver::Result::kOk &&
         samples.valid_mask == 7 && samples.plausible_mask == 7;
}

bool BarePrimaryVptrRejected() {
  Memory memory;
  Put(&memory, kRaceView,
      kGameBase + resolver::kRaceViewPrimaryVptrRva);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x70000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) == resolver::Result::kNotFound;
}

bool DuplicateRejected() {
  Memory memory;
  InstallCandidate(&memory, kRaceView, kManager, kCamera);
  InstallCandidate(&memory, 0x1A000, 0x32000, 0x42000);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x70000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) == resolver::Result::kNotUnique;
}

bool ReadFailureIsFatal() {
  Memory memory;
  memory.fail_reads = true;
  const resolver::Mapping mappings[] = {
      {0x10000, 0x70000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) == resolver::Result::kReadFailed;
}

bool LdPlayerMappingCountAccepted() {
  constexpr std::size_t kObservedLdPlayerMappings = 6771;
  std::vector<resolver::Mapping> mappings(kObservedLdPlayerMappings);
  for (std::size_t index = 0; index < mappings.size(); ++index) {
    const std::uintptr_t begin = 0x10000 + index * 0x2000;
    mappings[index] = {begin, begin + 0x1000, true, false, false};
  }
  return resolver::MappingSnapshotValid(mappings.data(), mappings.size());
}

bool ExcessiveMappingCountRejected() {
  std::vector<resolver::Mapping> mappings(resolver::kMaximumMappings + 1);
  return !resolver::MappingSnapshotValid(mappings.data(), mappings.size());
}

bool SharedIpcMappingRejectedAsObjectArena() {
  const resolver::Mapping shared_mapping = {
      0x10000, 0x20000, true, true, false, false,
  };
  const resolver::Mapping private_mapping = {
      0x20000, 0x30000, true, true, false, true,
  };
  return !resolver::ObjectMapping(&shared_mapping) &&
         resolver::ObjectMapping(&private_mapping);
}

bool ReadableGarbageStateRejected() {
  Memory memory;
  InstallCandidate(&memory, kRaceView, kManager, kCamera);
  resolver::CameraState garbage{};
  garbage.fov = -1000.0f;
  Put(&memory, kCamera + resolver::kControllerStateOffset, garbage);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x70000, true, true, false},
  };
  resolver::Resolution resolution{};
  if (resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                        kGameBase, &resolution) != resolver::Result::kOk)
    return false;
  resolver::StateSamples samples{};
  return resolver::ReadStateSamples({&memory, &Read}, mappings,
                                    std::size(mappings), resolution,
                                    &samples) ==
             resolver::Result::kStateUnavailable &&
         samples.valid_mask != 0 && samples.plausible_mask == 0;
}

}  // namespace

int main() {
  return UniqueRouteAndSamplesPass() && BarePrimaryVptrRejected() &&
                 DuplicateRejected() && ReadFailureIsFatal() &&
                 LdPlayerMappingCountAccepted() &&
                 ExcessiveMappingCountRejected() &&
                 SharedIpcMappingRejectedAsObjectArena() &&
                 ReadableGarbageStateRejected()
             ? 0
             : 1;
}
