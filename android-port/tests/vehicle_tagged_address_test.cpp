#include "../src/vehicle_state_resolver_v1.h"
using a9tas::vehicle_state_v1::RemoteAddress;
static_assert(RemoteAddress(0xb400006faad40a78ULL) == 0x6faad40a78ULL);
static_assert(RemoteAddress(0x6faad40a78ULL) == 0x6faad40a78ULL);
static_assert(RemoteAddress(0) == 0);
static_assert(RemoteAddress(0xb400006faad40a78ULL + 0x20) == 0x6faad40a98ULL);
// Applying an Itanium subobject adjustment must preserve the stored tag.
constexpr auto adjusted = static_cast<std::uintptr_t>(
    static_cast<std::intptr_t>(0xb400006faad40a78ULL) - 0x18);
static_assert(adjusted == 0xb400006faad40a60ULL);
static_assert(RemoteAddress(adjusted) == 0x6faad40a60ULL);
constexpr bool mapped(std::uintptr_t address) {
    return address >= 0x7599000000ULL && address < 0x759a000000ULL;
}
static_assert(!mapped(0xb400007599082a78ULL));
static_assert(mapped(RemoteAddress(0xb400007599082a78ULL)));
static_assert(mapped(RemoteAddress(0xb400007599082a78ULL) + 0x20));
static_assert(!mapped(RemoteAddress(0xb40000759a000000ULL)));
int main() { return 0; }
