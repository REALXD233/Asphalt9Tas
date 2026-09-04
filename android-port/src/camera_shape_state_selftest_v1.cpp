#include "camera_shape_state_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace active = a9tas::camera_active_state_resolver_v1;
namespace shape = a9tas::camera_shape_state_v1;

namespace {

struct Memory {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x40000);
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

constexpr std::uintptr_t kObject = 0x18000;
constexpr std::uintptr_t kGameBase = 0x10000000;

std::vector<active::Mapping> Mappings() {
  return {
      {0x10000, 0x28000, true, true, false, true},
      {kGameBase, kGameBase + 0xA600000, true, false, false, true},
  };
}

shape::Transform28 Transform(float seed) {
  return {{seed, seed + 1.0f, seed + 2.0f},
          {0.0f, 0.0f, 0.0f, 1.0f}};
}

void MakeValid(Memory* memory) {
  const std::uintptr_t primary =
      kGameBase + shape::kExpectedPrimaryVptrRva;
  const std::uintptr_t secondary =
      kGameBase + shape::kExpectedSecondaryVptrRva;
  const shape::MotionPair24 motion{{1.0f, 2.0f, 3.0f},
                                   {4.0f, 5.0f, 6.0f}};
  Put(memory, kObject, primary);
  Put(memory, kObject + shape::kStateAOffset, Transform(10.0f));
  Put(memory, kObject + shape::kStateBOffset, Transform(20.0f));
  Put(memory, kObject + shape::kMotionPairOffset, motion);
  Put(memory, kObject + shape::kStateCOffset, Transform(30.0f));
  Put(memory, kObject + shape::kSecondaryVptrOffset, secondary);
}

bool ExactSnapshotPasses() {
  Memory memory;
  MakeValid(&memory);
  const auto mappings = Mappings();
  shape::Snapshot result{};
  return shape::ReadSnapshot({&memory, &Read}, mappings.data(),
                             mappings.size(), kGameBase, kObject,
                             &result) == shape::Result::kOk &&
         result.object == kObject && result.state_a.position[0] == 10.0f &&
         result.state_b.position[0] == 20.0f &&
         result.state_c.position[0] == 30.0f &&
         result.motion_pair.second[2] == 6.0f &&
         result.finite_mask == 0xF &&
         result.quaternion_plausible_mask ==
             (shape::kStateAValid | shape::kStateBValid |
              shape::kStateCValid);
}

bool WrongPrimaryRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t wrong =
      kGameBase + shape::kExpectedPrimaryVptrRva + 8;
  Put(&memory, kObject, wrong);
  const auto mappings = Mappings();
  shape::Snapshot result{};
  return shape::ReadSnapshot({&memory, &Read}, mappings.data(),
                             mappings.size(), kGameBase, kObject,
                             &result) == shape::Result::kPrimaryVptrMismatch;
}

bool WrongSecondaryRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t wrong =
      kGameBase + shape::kExpectedSecondaryVptrRva + 8;
  Put(&memory, kObject + shape::kSecondaryVptrOffset, wrong);
  const auto mappings = Mappings();
  shape::Snapshot result{};
  return shape::ReadSnapshot({&memory, &Read}, mappings.data(),
                             mappings.size(), kGameBase, kObject,
                             &result) == shape::Result::kSecondaryVptrMismatch;
}

bool SharedObjectRejected() {
  Memory memory;
  MakeValid(&memory);
  auto mappings = Mappings();
  mappings[0].private_mapping = false;
  shape::Snapshot result{};
  return shape::ReadSnapshot({&memory, &Read}, mappings.data(),
                             mappings.size(), kGameBase, kObject,
                             &result) == shape::Result::kObjectMappingInvalid;
}

bool ReadFailureIsFatal() {
  Memory memory;
  MakeValid(&memory);
  memory.fail_reads = true;
  const auto mappings = Mappings();
  shape::Snapshot result{};
  return shape::ReadSnapshot({&memory, &Read}, mappings.data(),
                             mappings.size(), kGameBase, kObject,
                             &result) == shape::Result::kReadFailed;
}

static_assert(shape::kObjectSize == 0xF0);
static_assert(shape::kStateAOffset == 0x40);
static_assert(shape::kStateBOffset == 0x5C);
static_assert(shape::kStateCOffset == 0x90);

}  // namespace

int main() {
  return ExactSnapshotPasses() && WrongPrimaryRejected() &&
                 WrongSecondaryRejected() && SharedObjectRejected() &&
                 ReadFailureIsFatal()
             ? 0
             : 1;
}
