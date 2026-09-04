#include "gameplay_input_controller_resolver_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace resolver = a9tas::gameplay_input_controller_resolver_v1;
namespace protocol = a9tas::controller_shadow_coordinator_v1;

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

constexpr std::uintptr_t kGameBase = 0x100000000ULL;
constexpr std::uintptr_t kController = 0x18000;
constexpr std::uintptr_t kSource = 0x28000;

void InstallCandidate(Memory* memory, std::uintptr_t controller,
                      std::uintptr_t source, bool valid_source = true) {
  const std::uintptr_t controller_vptr =
      kGameBase + protocol::kControllerAddressPointRva;
  const std::uintptr_t source_vptr = valid_source
                                         ? kGameBase +
                                               protocol::kKeyboardSourceAddressPointRva
                                         : 0xDEADBEEF;
  Put(memory, controller, controller_vptr);
  Put(memory, controller + protocol::kControllerSourceOffset, source);
  Put(memory, source, source_vptr);
}

bool UniqueCandidatePasses() {
  Memory memory;
  InstallCandidate(&memory, kController, kSource);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x30000, true, true, false},
      {0x30000, 0x40000, true, false, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) == resolver::Result::kOk &&
         resolution.controller == kController && resolution.source == kSource &&
         resolution.structural_candidates == 1 &&
         resolution.bytes_scanned == 0x20000;
}

bool ExactAlternateAddressPointPasses() {
  Memory memory;
  constexpr std::uintptr_t kAlternateAddressPointRva = 0x80C4DD8;
  const std::uintptr_t controller_vptr =
      kGameBase + kAlternateAddressPointRva;
  const std::uintptr_t source_vptr =
      kGameBase + protocol::kKeyboardSourceAddressPointRva;
  Put(&memory, kController, controller_vptr);
  Put(&memory, kController + protocol::kControllerSourceOffset, kSource);
  Put(&memory, kSource, source_vptr);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x30000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::ResolveAtAddressPoint(
             {&memory, &Read}, mappings, std::size(mappings), kGameBase,
             kAlternateAddressPointRva, &resolution) ==
             resolver::Result::kOk &&
         resolution.controller == kController &&
         resolution.controller_vptr == controller_vptr &&
         resolution.source == kSource;
}

bool DuplicateRejected() {
  Memory memory;
  InstallCandidate(&memory, kController, kSource);
  InstallCandidate(&memory, 0x1A000, 0x2A000);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x30000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) ==
         resolver::Result::kNotUnique;
}

bool BareVptrIsNotEnough() {
  Memory memory;
  InstallCandidate(&memory, kController, kSource, false);
  const resolver::Mapping mappings[] = {
      {0x10000, 0x30000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) ==
         resolver::Result::kNotFound;
}

bool ReadFailureIsFatal() {
  Memory memory;
  memory.fail_reads = true;
  const resolver::Mapping mappings[] = {
      {0x10000, 0x30000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) ==
             resolver::Result::kReadFailed &&
         resolution.failure_mapping_index == 0 &&
         resolution.failure_address == 0x10000 &&
         resolution.failure_size == 0x20000 &&
         resolution.bytes_scanned == 0;
}

bool OverlappingSnapshotRejected() {
  Memory memory;
  const resolver::Mapping mappings[] = {
      {0x10000, 0x30000, true, true, false},
      {0x20000, 0x40000, true, true, false},
  };
  resolver::Resolution resolution{};
  return resolver::Resolve({&memory, &Read}, mappings, std::size(mappings),
                           kGameBase, &resolution) ==
         resolver::Result::kMappingSnapshotInvalid;
}

}  // namespace

int main() {
  return UniqueCandidatePasses() && ExactAlternateAddressPointPasses() &&
                 DuplicateRejected() &&
                 BareVptrIsNotEnough() && ReadFailureIsFatal() &&
                 OverlappingSnapshotRejected()
             ? 0
             : 1;
}
