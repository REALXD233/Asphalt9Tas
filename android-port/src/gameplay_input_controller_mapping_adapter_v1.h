#pragma once

#include "gameplay_input_controller_resolver_v1.h"

#include <vector>

namespace a9tas::gameplay_input_controller_mapping_adapter_v1 {

namespace resolver = a9tas::gameplay_input_controller_resolver_v1;

// The resolver scans native process-local objects.  On LDPlayer, readable and
// writable shared device mappings (for example /dev/fastpipe) advertise rw-
// permissions but reject /proc/PID/mem reads and cannot own this process-local
// C++ controller/source pair.  Keep only private rw-p, non-executable maps.
template <typename SourceMapping>
bool ConvertObjectMappings(const std::vector<SourceMapping>& source,
                           std::vector<resolver::Mapping>* output) {
  if (output == nullptr || source.empty()) return false;
  output->clear();
  output->reserve(source.size());
  for (const auto& mapping : source) {
    const bool readable = mapping.perms[0] == 'r';
    const bool writable = mapping.perms[1] == 'w';
    const bool executable = mapping.perms[2] == 'x';
    const bool private_mapping = mapping.perms[3] == 'p';
    if (!readable || !writable || executable || !private_mapping) continue;
    output->push_back(
        {mapping.begin, mapping.end, true, true, false});
  }
  return resolver::MappingSnapshotValid(output->data(), output->size());
}

}  // namespace a9tas::gameplay_input_controller_mapping_adapter_v1
