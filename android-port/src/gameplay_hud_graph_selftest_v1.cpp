#include "gameplay_hud_graph_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hud = a9tas::gameplay_hud_graph_v1;
namespace active = a9tas::camera_active_state_resolver_v1;

namespace {

struct Arena {
  alignas(64) std::array<std::uint8_t, 0x3000> bytes{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
  auto* arena = static_cast<Arena*>(context);
  const std::uintptr_t begin =
      reinterpret_cast<std::uintptr_t>(arena->bytes.data());
  const std::uintptr_t end = begin + arena->bytes.size();
  if (address < begin || address > end || size > end - address) return false;
  std::memcpy(output, reinterpret_cast<const void*>(address), size);
  return true;
}

void Put(std::uintptr_t address, std::uintptr_t value) {
  std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

}  // namespace

int main() {
  constexpr std::uintptr_t kBase = 0x10000000;
  constexpr std::uintptr_t kEnd = 0xB0000000;
  Arena arena{};
  const std::uintptr_t begin =
      reinterpret_cast<std::uintptr_t>(arena.bytes.data());
  const active::Mapping mapping{
      begin, begin + arena.bytes.size(), true, true, false, true};
  const std::uintptr_t hud_object = begin + 0x100;
  const std::uintptr_t owner = begin + 0x1000;
  const std::uintptr_t screen = begin + 0x1800;
  Put(hud_object, kBase + hud::kGameplayHudVptrRvas[0]);
  Put(hud_object + hud::kGuiOwnerOffset, owner);
  Put(hud_object + hud::kGuiOwnerMirrorOffset, owner);
  Put(owner, kBase + 0x123000);
  Put(owner + hud::kRegisteredScreenOffset, screen);
  Put(screen, kBase + 0x456000);

  hud::Resolution result{};
  if (hud::Resolve({&arena, &Read}, &mapping, 1, kBase, kEnd, &result) !=
          hud::Result::kOk ||
      result.hud != hud_object || result.gui_owner != owner ||
      result.registered_screen != screen || result.structural_candidates != 1 ||
      result.matched_hud_vptr_rva != hud::kGameplayHudVptrRvas[0])
    return 1;

  Put(hud_object, kBase + hud::kGameplayHudVptrRvas[1]);
  if (hud::Resolve({&arena, &Read}, &mapping, 1, kBase, kEnd, &result) !=
          hud::Result::kOk ||
      result.matched_hud_vptr_rva != hud::kGameplayHudVptrRvas[1])
    return 4;

  Put(hud_object + hud::kGuiOwnerMirrorOffset, owner + 8);
  if (hud::Resolve({&arena, &Read}, &mapping, 1, kBase, kEnd, &result) !=
      hud::Result::kNotFound)
    return 2;
  Put(hud_object + hud::kGuiOwnerMirrorOffset, owner);
  const std::uintptr_t second = begin + 0x2000;
  Put(second, kBase + hud::kGameplayHudVptrRvas[1]);
  Put(second + hud::kGuiOwnerOffset, owner);
  Put(second + hud::kGuiOwnerMirrorOffset, owner);
  if (hud::Resolve({&arena, &Read}, &mapping, 1, kBase, kEnd, &result) !=
      hud::Result::kNotUnique)
    return 3;
  return 0;
}
