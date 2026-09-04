#include "barrel_successor_runtime_v1.h"

#include <cstdio>

int main() {
  a9tas::barrel_successor_runtime_v1::Runtime runtime{};
  const bool passive = !runtime.faulted() && !runtime.frame_active();
  std::printf(
      "BARREL_SUCCESSOR_RUNTIME_BUILD_ONLY passed=%u runtime=disabled "
      "collector=0 game_writes=0\n",
      passive ? 1u : 0u);
  return passive ? 0 : 1;
}
