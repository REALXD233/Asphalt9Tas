#include "final_writer_replay_elf_resolver_v1.h"

using a9tas::final_writer_replay_elf_v1::Layout;
using namespace a9tas::final_writer_replay_v1;

static_assert(sizeof(Layout::file_sha256) == 32);
static_assert(sizeof(FrameTarget) == 80);
static_assert(sizeof(FrameAudit) == 160);
static_assert(sizeof(Control) == 128);
static_assert(sizeof(Evidence) == 192);

int main() {
  Layout layout{};
  return a9tas::final_writer_replay_elf_v1::Resolve(0, -1, &layout) ? 1 : 0;
}
