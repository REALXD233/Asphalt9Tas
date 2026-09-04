#include "camera_manager_graph_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace active = a9tas::camera_active_state_resolver_v1;
namespace graph = a9tas::camera_manager_graph_v1;

namespace {

struct Memory {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x90000);
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
constexpr std::uintptr_t kWrapper = 0x24000;
constexpr std::uintptr_t kObjectA = 0x30000;
constexpr std::uintptr_t kObjectB = 0x31000;
constexpr std::uintptr_t kObjectC = 0x32000;
constexpr std::uintptr_t kObjectD = 0x33000;
constexpr std::uintptr_t kGameBase = 0x10000000;
constexpr std::uintptr_t kGameEnd = 0x1A600000;

std::vector<active::Mapping> Mappings() {
  return {
      {0x10000, 0x28000, true, true, false, true},
      {0x30000, 0x38000, true, true, false, true},
      {kGameBase, kGameEnd, true, false, false, true},
  };
}

active::Resolution Resolution() {
  active::Resolution result{};
  result.camera_manager = kManager;
  result.blended_camera = kWrapper;
  result.camera_vptr = kGameBase + graph::kExpectedWrapperVptrRva;
  return result;
}

void MakeValid(Memory* memory) {
  const std::uintptr_t primary =
      kGameBase + graph::kExpectedPrimaryVptrRva;
  const std::uintptr_t secondary =
      kGameBase + graph::kExpectedSecondaryVptrRva;
  const std::uintptr_t wrapper_vptr =
      kGameBase + graph::kExpectedWrapperVptrRva;
  const std::uintptr_t embedded_vptr = kGameBase + 0x8176408;
  const std::uintptr_t head_a = kGameBase + 0xA4172C0;
  const std::uintptr_t head_b = kGameBase + 0x9D84338;
  const std::uintptr_t head_c = kGameBase + 0x9D84870;
  const std::uintptr_t head_d = kGameBase + 0x7F10D90;
  const float scalar = 1.25f;
  Put(memory, kManager, primary);
  Put(memory, kManager + 0x8, embedded_vptr);
  Put(memory, kManager + 0xD8, kObjectA);
  Put(memory, kManager + 0xE8, kObjectB);
  Put(memory, kManager + 0xF0, kObjectC);
  Put(memory, kManager + 0xF8, kObjectD);
  Put(memory, kManager + graph::kBlendedWrapperOffset, kWrapper);
  Put(memory, kManager + graph::kScalarOffset, scalar);
  Put(memory, kManager + graph::kSecondaryOffset, secondary);
  Put(memory, kObjectA, head_a);
  Put(memory, kObjectB, head_b);
  Put(memory, kObjectC, head_c);
  Put(memory, kObjectD, head_d);
  Put(memory, kWrapper, wrapper_vptr);
  const std::uintptr_t secondary_interface =
      kManager + graph::kSecondaryOffset;
  Put(memory, kWrapper + graph::kWrapperOwnerOffsetA, secondary_interface);
  Put(memory, kWrapper + graph::kWrapperOwnerOffsetB, secondary_interface);
}

bool ExactManagerGraphPasses() {
  Memory memory;
  MakeValid(&memory);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
             graph::Result::kOk &&
         result.manager == kManager && result.blended_wrapper == kWrapper &&
         result.secondary_interface == kManager + 0x110 &&
         result.wrapper_owner_a == kManager + 0x110 &&
         result.wrapper_owner_b == kManager + 0x110 &&
         result.scalar_108 == 1.25f &&
         result.mapped_constructor_pointer_count == 5 &&
         result.private_object_pointer_count == 5 &&
         result.pointer_dereference_count == 5 &&
         result.object_head_image_pointer_count == 5;
}

bool DirectManagerFallbackPassesWithoutRaceView() {
  Memory memory;
  MakeValid(&memory);
  const auto mappings = Mappings();
  active::Resolution result{};
  graph::DirectManagerDiagnostics diagnostics{};
  return graph::ResolveDirectManager(
             {&memory, &Read}, mappings.data(), mappings.size(), kGameBase,
             kGameEnd, &result, &diagnostics) == active::Result::kOk &&
         result.race_view == 0 && result.camera_manager == kManager &&
         result.blended_camera == kWrapper &&
         result.camera_vptr == kGameBase + graph::kExpectedWrapperVptrRva &&
         result.structural_candidates == 1 &&
         diagnostics.manager_primary_matches == 1;
}

bool WrongPrimaryVptrRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t wrong = kGameBase + graph::kExpectedPrimaryVptrRva + 8;
  Put(&memory, kManager, wrong);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kPrimaryVptrMismatch;
}

bool WrongSecondaryVptrRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t wrong =
      kGameBase + graph::kExpectedSecondaryVptrRva + 8;
  Put(&memory, kManager + graph::kSecondaryOffset, wrong);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kSecondaryVptrMismatch;
}

bool ChangedBlendedWrapperRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t changed = kWrapper + 0x1000;
  Put(&memory, kManager + graph::kBlendedWrapperOffset, changed);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kBlendedWrapperMismatch;
}

bool WrongWrapperOwnerRejected() {
  Memory memory;
  MakeValid(&memory);
  const std::uintptr_t wrong = kManager;
  Put(&memory, kWrapper + graph::kWrapperOwnerOffsetB, wrong);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kWrapperOwnerMismatch;
}

bool SharedConstructorPointerIsNotDereferenced() {
  Memory memory;
  MakeValid(&memory);
  constexpr std::uintptr_t kShared = 0x40000;
  Put(&memory, kManager + 0xE8, kShared);
  auto mappings = Mappings();
  mappings.insert(mappings.begin() + 2,
                  {0x40000, 0x41000, true, true, false, false});
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
             graph::Result::kOk &&
         result.mapped_constructor_pointer_count == 5 &&
         result.private_object_pointer_count == 4 &&
         result.pointer_dereference_count == 4;
}

bool ReadFailureIsFatal() {
  Memory memory;
  MakeValid(&memory);
  memory.fail_reads = true;
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kReadFailed;
}

static_assert(graph::kManagerSize == 0x118);
static_assert(graph::kMaximumPointerDereferences == 5);
static_assert(graph::kReportedFieldOffsets.size() == 9);

}  // namespace

int main() {
  return ExactManagerGraphPasses() &&
                 DirectManagerFallbackPassesWithoutRaceView() &&
                 WrongPrimaryVptrRejected() &&
                 WrongSecondaryVptrRejected() &&
                 ChangedBlendedWrapperRejected() &&
                 WrongWrapperOwnerRejected() &&
                 SharedConstructorPointerIsNotDereferenced() &&
                 ReadFailureIsFatal()
             ? 0
             : 1;
}
