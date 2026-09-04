#include "src/fc2_payload_elf_resolver_v1.h"
#include "src/fc2_transaction_report_v1.h"

#include <cstdint>

static_assert(sizeof(a9tas::fc2_payload_elf_v1::Layout) >=
              sizeof(std::uintptr_t) * 11);
static_assert(sizeof(a9tas::fc2_report_v1::Report) == 528);

int main() {
  a9tas::fc2_payload_elf_v1::Layout layout{};
  return a9tas::fc2_payload_elf_v1::Resolve(-1, -1, &layout) ? 1 : 0;
}
