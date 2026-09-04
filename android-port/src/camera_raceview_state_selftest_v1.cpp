#include "camera_raceview_state_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace active = a9tas::camera_active_state_resolver_v1;
namespace raceview = a9tas::camera_raceview_state_v1;
namespace shape = a9tas::camera_shape_state_v1;

namespace {

struct Memory {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x50000);
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

constexpr std::uintptr_t kManager = 0x18000;
constexpr std::uintptr_t kShape = 0x28000;
constexpr std::uintptr_t kGameBase = 0x10000000;

std::vector<active::Mapping> Mappings() {
  return {
      {0x10000, 0x40000, true, true, false, true},
      {kGameBase, kGameBase + 0xA600000, true, false, false, true},
  };
}

shape::Transform28 Transform(float seed) {
  return {{seed, seed + 1.0f, seed + 2.0f},
          {0.0f, 0.0f, 0.0f, 1.0f}};
}

void MakeValid(Memory* memory) {
  const std::uintptr_t primary =
      kGameBase + raceview::kExpectedPrimaryVptrRva;
  const std::uintptr_t embedded =
      kGameBase + raceview::kExpectedEmbeddedVptrRva;
  const std::uintptr_t secondary =
      kGameBase + raceview::kExpectedSecondaryVptrRva;
  const std::uintptr_t shape_primary =
      kGameBase + shape::kExpectedPrimaryVptrRva;
  const std::uintptr_t shape_secondary =
      kGameBase + shape::kExpectedSecondaryVptrRva;
  const shape::MotionPair24 motion{};
  const float fov = 0.8f;

  Put(memory, kManager, primary);
  Put(memory, kManager + raceview::kEmbeddedVptrOffset, embedded);
  Put(memory, kManager + raceview::kLocalStateOffset, Transform(10.0f));
  Put(memory, kManager + raceview::kWorldStateOffset, Transform(20.0f));
  Put(memory, kManager + raceview::kShapePointerOffset, kShape);
  Put(memory, kManager + raceview::kFovOffset, fov);
  Put(memory, kManager + raceview::kSecondaryVptrOffset, secondary);

  Put(memory, kShape, shape_primary);
  Put(memory, kShape + shape::kStateAOffset, Transform(20.0f));
  Put(memory, kShape + shape::kStateBOffset, Transform(20.0f));
  Put(memory, kShape + shape::kMotionPairOffset, motion);
  Put(memory, kShape + shape::kStateCOffset, Transform(20.0f));
  Put(memory, kShape + shape::kSecondaryVptrOffset, shape_secondary);
}

bool ExactRaceViewSnapshotPasses() {
  Memory memory;
  MakeValid(&memory);
  const auto mappings = Mappings();
  raceview::Snapshot result{};
  return raceview::ReadSnapshot({&memory, &Read}, mappings.data(),
                                mappings.size(), kGameBase, kManager, kShape,
                                &result) == raceview::Result::kOk &&
         result.manager == kManager && result.shape_object == kShape &&
         result.local_state.position[0] == 10.0f &&
         result.world_state.position[0] == 20.0f &&
         result.shape_state.state_a.position[0] == 20.0f &&
         result.fov_radians == 0.8f;
}

bool WrongManagerVptrRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t wrong =
      kGameBase + raceview::kExpectedPrimaryVptrRva + 8;
  Put(&memory, kManager, wrong);
  const auto mappings = Mappings();
  raceview::Snapshot result{};
  return raceview::ReadSnapshot({&memory, &Read}, mappings.data(),
                                mappings.size(), kGameBase, kManager, kShape,
                                &result) ==
         raceview::Result::kPrimaryVptrMismatch;
}

bool ShapeIdentityMismatchRejected() {
  Memory memory;
  MakeValid(&memory);
  const auto mappings = Mappings();
  raceview::Snapshot result{};
  return raceview::ReadSnapshot({&memory, &Read}, mappings.data(),
                                mappings.size(), kGameBase, kManager,
                                kShape + 0x100, &result) ==
         raceview::Result::kShapePointerMismatch;
}

bool InvalidFovRejected() {
  Memory memory;
  MakeValid(&memory);
  const float invalid = -1.0f;
  Put(&memory, kManager + raceview::kFovOffset, invalid);
  const auto mappings = Mappings();
  raceview::Snapshot result{};
  return raceview::ReadSnapshot({&memory, &Read}, mappings.data(),
                                mappings.size(), kGameBase, kManager, kShape,
                                &result) == raceview::Result::kFovInvalid;
}

bool ReadFailureIsFatal() {
  Memory memory;
  MakeValid(&memory);
  memory.fail_reads = true;
  const auto mappings = Mappings();
  raceview::Snapshot result{};
  return raceview::ReadSnapshot({&memory, &Read}, mappings.data(),
                                mappings.size(), kGameBase, kManager, kShape,
                                &result) == raceview::Result::kReadFailed;
}

static_assert(raceview::kManagerSize == 0x118);
static_assert(raceview::kLocalStateOffset == 0x10);
static_assert(raceview::kWorldStateOffset == 0x2C);
static_assert(raceview::kFovOffset == 0x108);

}  // namespace

int main() {
  return ExactRaceViewSnapshotPasses() && WrongManagerVptrRejected() &&
                 ShapeIdentityMismatchRejected() && InvalidFovRejected() &&
                 ReadFailureIsFatal()
             ? 0
             : 1;
}
