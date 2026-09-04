#include "physics_interval_getter_payload_elf_resolver_v2.h"

using a9tas::physics_interval_getter_elf_v2::Layout;
using namespace a9tas::physics_interval_payload_v2;

static_assert(sizeof(Layout::file_sha256) == 32);
static_assert(sizeof(Control) == 64);
static_assert(sizeof(Evidence) == 128);
static_assert(sizeof(Event) == 64);

int main() {
    Layout layout{};
    return a9tas::physics_interval_getter_elf_v2::Resolve(0, -1, &layout)
               ? 1
               : 0;
}
