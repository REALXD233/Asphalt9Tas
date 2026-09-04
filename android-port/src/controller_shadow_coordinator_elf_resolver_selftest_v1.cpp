#include "controller_shadow_coordinator_elf_resolver_v1.h"

namespace resolver = a9tas::controller_shadow_coordinator_elf_v1;
namespace protocol = a9tas::controller_shadow_coordinator_v1;

static_assert(sizeof(resolver::Layout::file_sha256) == 32);
static_assert(sizeof(protocol::Control) == 192);
static_assert(sizeof(protocol::Evidence) == 192);
static_assert(protocol::kControllerShadowSize == 0x100);
static_assert(protocol::kControllerUpdateSlotOffset == 14 * sizeof(void*));

int main() {
  resolver::Layout layout{};
  return resolver::Resolve(0, -1, &layout) ? 1 : 0;
}
