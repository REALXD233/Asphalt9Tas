#include "src/natural_action_lifecycle_elf_resolver_v1.h"

#include <cstdint>

static_assert(sizeof(a9tas::natural_action_lifecycle_elf_v1::Layout) >=
              sizeof(std::uintptr_t) * 13);

int main() {
  a9tas::natural_action_lifecycle_elf_v1::Layout layout{};
  return a9tas::natural_action_lifecycle_elf_v1::Resolve(-1, -1, &layout)
             ? 1
             : 0;
}
