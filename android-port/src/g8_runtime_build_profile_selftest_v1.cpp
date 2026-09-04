#include "g8_runtime_build_profile_v1.h"

#include <cstdio>

int main() {
  namespace profile = a9tas::g8_runtime_build_profile_v1;
  const profile::Profile reference = profile::ReferenceProfile();
  if (!profile::Valid(reference)) return 1;
  auto changed = reference;
  changed.profile_sha256[0] = 0;
  for (auto& value : changed.profile_sha256) value = 0;
  if (profile::Valid(changed)) return 2;
  changed = reference;
  changed.hook_rvas[0] = changed.image_size;
  if (profile::Valid(changed)) return 3;
  changed = reference;
  changed.lifecycle_phase_gate_rva = 0;
  if (profile::Valid(changed)) return 4;
  changed = reference;
  changed.reserved[0] = 1;
  if (profile::Valid(changed)) return 5;
  std::printf(
      "G8_RUNTIME_BUILD_PROFILE_SELFTEST passed=1 size=%zu hooks=%u "
      "setters=%u channel_literals=0 game_writes=0\n",
      sizeof(reference), profile::kHookCount, profile::kSetterCount);
  return 0;
}
