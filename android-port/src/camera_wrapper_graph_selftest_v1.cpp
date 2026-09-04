#include "camera_wrapper_graph_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace active = a9tas::camera_active_state_resolver_v1;
namespace graph = a9tas::camera_wrapper_graph_v1;

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

constexpr std::uintptr_t kWrapper = 0x18000;
constexpr std::uintptr_t kOwner = 0x30000;
constexpr std::uintptr_t kGameBase = 0x10000000;
constexpr std::uintptr_t kGameEnd = 0x1A600000;

active::Resolution Resolution() {
  active::Resolution resolution{};
  resolution.blended_camera = kWrapper;
  resolution.camera_vptr = kGameBase + graph::kExpectedPrimaryVptrRva;
  return resolution;
}

std::vector<active::Mapping> Mappings() {
  return {
      {0x10000, 0x28000, true, true, false, true},
      {0x30000, 0x38000, true, true, false, true},
      {kGameBase, kGameEnd, true, false, false, true},
  };
}

bool ExactBoundedGraphPasses() {
  Memory memory;
  const std::uintptr_t primary =
      kGameBase + graph::kExpectedPrimaryVptrRva;
  const std::uintptr_t owner_vptr = kGameBase + 0x8174F40;
  Put(&memory, kWrapper, primary);
  Put(&memory, kWrapper + 0x90, kOwner);
  Put(&memory, kOwner, owner_vptr);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
             graph::Result::kOk &&
         result.wrapper == kWrapper && result.words.size() == 19 &&
         result.words[0].value_is_known_interface_vptr &&
         result.words[18].value == kOwner &&
         result.words[18].pointee_head == owner_vptr &&
         result.known_interface_vptr_count == 1 &&
         result.private_object_pointer_count == 1 &&
         result.pointer_dereference_count == 1 &&
         result.object_head_image_pointer_count == 1;
}

bool WrongPrimaryVptrRejected() {
  Memory memory;
  active::Resolution resolution = Resolution();
  ++resolution.camera_vptr;
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, resolution, &result) ==
         graph::Result::kPrimaryVptrMismatch;
}

bool ChangedWrapperAfterResolutionRejected() {
  Memory memory;
  const std::uintptr_t wrong_live_vptr =
      kGameBase + graph::kExpectedPrimaryVptrRva + 8;
  Put(&memory, kWrapper, wrong_live_vptr);
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kPrimaryVptrMismatch;
}

bool SharedTargetIsNotDereferenced() {
  Memory memory;
  const std::uintptr_t primary =
      kGameBase + graph::kExpectedPrimaryVptrRva;
  Put(&memory, kWrapper, primary);
  Put(&memory, kWrapper + 0x90, kOwner);
  auto mappings = Mappings();
  mappings[1].private_mapping = false;
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
             graph::Result::kOk &&
         result.private_object_pointer_count == 0 &&
         result.pointer_dereference_count == 0;
}

bool ReadFailureIsFatal() {
  Memory memory;
  memory.fail_reads = true;
  const auto mappings = Mappings();
  graph::Graph result{};
  return graph::Build({&memory, &Read}, mappings.data(), mappings.size(),
                      kGameBase, kGameEnd, Resolution(), &result) ==
         graph::Result::kReadFailed;
}

}  // namespace

int main() {
  return ExactBoundedGraphPasses() && WrongPrimaryVptrRejected() &&
                 ChangedWrapperAfterResolutionRejected() &&
                 SharedTargetIsNotDereferenced() && ReadFailureIsFatal()
             ? 0
             : 1;
}
