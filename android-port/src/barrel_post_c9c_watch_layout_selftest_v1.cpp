#include "barrel_post_c9c_watch_layout_v1.h"

#include <cstdio>

namespace watch = a9tas::barrel_post_c9c_watch_v1;

int main() {
  watch::Layout layout{};
  const bool prepared = watch::Prepare(
      0x10000000u, 0x20000160u, 0x30000002u, 0x40000004u,
      0x50000008u, &layout);
  const bool exact =
      prepared && layout.dr0_rbx_first == 0x10001968u &&
      layout.dr0_rbx_second == 0x1000196cu &&
      layout.dr1_angular_aux == 0x2000016Cu &&
      layout.dr1_callback_flags == 0x30000002u &&
      layout.dr2_f64 == 0x40000004u &&
      layout.dr3_world_commit == 0x50000008u &&
      layout.parallel_active_dr7 == watch::kParallelActiveDr7 &&
      layout.rbx_disabled_dr7 == watch::kRbxDisabledDr7 &&
      layout.post_f64_dr7 == watch::kPostF64Dr7 &&
      watch::kParallelActiveDr7 != watch::kPostF64Dr7;

  watch::Layout ignored{};
  const bool rejects_bad_alignment =
      !watch::Prepare(0x10000002u, 0x20000160u, 0x30000002u,
                      0x40000004u, 0x50000008u, &ignored);
  const bool rejects_active_overlap =
      !watch::Prepare(0x10000000u, 0x20000160u, 0x30000002u,
                      0x10001968u, 0x50000008u, &ignored);
  const bool rejects_post_f64_overlap =
      !watch::Prepare(0x10000000u, 0x20000160u, 0x50000008u,
                      0x40000004u, 0x50000008u, &ignored);

  const bool passed = exact && rejects_bad_alignment &&
                      rejects_active_overlap && rejects_post_f64_overlap;
  std::printf(
      "BARREL_POST_C9C_WATCH_LAYOUT_SELFTEST passed=%u "
      "dr0_rbx_first=0x%llx dr0_rbx_second=0x%llx "
      "dr1_angular=0x%llx dr1_callback_restored=1 "
      "dr0_dr2_disabled_post_f64=1 runtime=disabled\n",
      passed ? 1u : 0u,
      static_cast<unsigned long long>(layout.dr0_rbx_first),
      static_cast<unsigned long long>(layout.dr0_rbx_second),
      static_cast<unsigned long long>(layout.dr1_angular_aux));
  return passed ? 0 : 1;
}
