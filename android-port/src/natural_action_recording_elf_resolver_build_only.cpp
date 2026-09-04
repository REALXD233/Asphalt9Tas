#include "natural_action_recording_elf_resolver_v1.h"

int main() {
  a9tas::natural_action_recording_elf_v1::Layout layout{};
  return a9tas::natural_action_recording_elf_v1::Resolve(-1, -1, &layout)
      ? 1 : 0;
}

