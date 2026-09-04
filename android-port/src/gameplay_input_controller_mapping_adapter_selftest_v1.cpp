#include "gameplay_input_controller_mapping_adapter_v1.h"

#include <cstdint>
#include <vector>

namespace adapter =
    a9tas::gameplay_input_controller_mapping_adapter_v1;
namespace resolver = a9tas::gameplay_input_controller_resolver_v1;

namespace {

struct SourceMapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  char perms[5]{};
};

SourceMapping Make(std::uintptr_t begin, const char* perms) {
  SourceMapping mapping{begin, begin + 0x1000, {}};
  for (int index = 0; index < 4; ++index) mapping.perms[index] = perms[index];
  return mapping;
}

bool LargeLdPlayerShapeFiltersBelowBound() {
  std::vector<SourceMapping> source;
  source.reserve(7075);
  std::uintptr_t cursor = 0x10000;
  std::size_t expected_objects = 0;
  for (std::size_t index = 0; index < 7075; ++index) {
    const bool object = index % 3 == 0;
    source.push_back(Make(cursor, object ? "rw-p" : "r--p"));
    if (object) ++expected_objects;
    cursor += 0x1000;
  }
  std::vector<resolver::Mapping> output;
  if (!adapter::ConvertObjectMappings(source, &output) ||
      output.size() != expected_objects ||
      output.size() > resolver::kMaximumMappings)
    return false;
  for (const auto& mapping : output)
    if (!resolver::ObjectMapping(&mapping)) return false;
  return true;
}

bool EmptyAndNonObjectOnlyFailClosed() {
  std::vector<SourceMapping> empty;
  std::vector<resolver::Mapping> output;
  if (adapter::ConvertObjectMappings(empty, &output)) return false;
  const std::vector<SourceMapping> source{
      Make(0x10000, "r--p"), Make(0x11000, "r-xp"),
      Make(0x12000, "rw-s")};
  return !adapter::ConvertObjectMappings(source, &output);
}

bool OverlappingObjectsRemainRejected() {
  std::vector<SourceMapping> source{
      Make(0x10000, "rw-p"), Make(0x10000, "rw-p")};
  std::vector<resolver::Mapping> output;
  return !adapter::ConvertObjectMappings(source, &output);
}

}  // namespace

int main() {
  return LargeLdPlayerShapeFiltersBelowBound() &&
                 EmptyAndNonObjectOnlyFailClosed() &&
                 OverlappingObjectsRemainRejected()
             ? 0
             : 1;
}
