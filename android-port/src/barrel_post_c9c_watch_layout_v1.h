#pragma once

// Exact debug-register layouts for the orthogonal post-C9C Barrel schedule.
// Active window: DR0 walks the two RBX fields, DR1 watches angular aux, DR2
// watches F64, DR3 watches world. At F64 DR0/DR2 are disabled and DR1 is
// restored to callback flags before the stopped owner resumes.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace a9tas::barrel_post_c9c_watch_v1 {

constexpr std::uintptr_t kRbxFirstOffset = 0x1968;
constexpr std::uintptr_t kRbxSecondOffset = 0x196C;
constexpr std::uintptr_t kAngularAuxOffset = 0x0C;

constexpr unsigned long LocalWrite(std::uint32_t slot,
                                   std::uint32_t length_encoding) {
  return (1UL << (slot * 2)) | (1UL << (16 + slot * 4)) |
         (static_cast<unsigned long>(length_encoding) <<
          (18 + slot * 4));
}

constexpr unsigned long kParallelActiveDr7 =
    LocalWrite(0, 3) | LocalWrite(1, 3) |
    LocalWrite(2, 3) | LocalWrite(3, 3);
constexpr unsigned long kRbxDisabledDr7 =
    LocalWrite(1, 3) | LocalWrite(2, 3) | LocalWrite(3, 3);
constexpr unsigned long kPostF64Dr7 =
    LocalWrite(1, 1) | LocalWrite(3, 3);

struct Layout {
  std::uintptr_t dr0_rbx_first{};
  std::uintptr_t dr0_rbx_second{};
  std::uintptr_t dr1_angular_aux{};
  std::uintptr_t dr1_callback_flags{};
  std::uintptr_t dr2_f64{};
  std::uintptr_t dr3_world_commit{};
  unsigned long parallel_active_dr7{};
  unsigned long rbx_disabled_dr7{};
  unsigned long post_f64_dr7{};
};

inline bool Add(std::uintptr_t base, std::uintptr_t offset,
                std::uintptr_t* output) {
  if (output == nullptr ||
      base > std::numeric_limits<std::uintptr_t>::max() - offset)
    return false;
  *output = base + offset;
  return true;
}

inline bool Overlaps(std::uintptr_t left, std::size_t left_size,
                     std::uintptr_t right, std::size_t right_size) {
  if (left_size == 0 || right_size == 0 ||
      left > std::numeric_limits<std::uintptr_t>::max() - left_size ||
      right > std::numeric_limits<std::uintptr_t>::max() - right_size)
    return true;
  return left < right + right_size && right < left + left_size;
}

inline bool Prepare(std::uintptr_t rbx_owner,
                    std::uintptr_t native_angular,
                    std::uintptr_t callback_flags,
                    std::uintptr_t f64,
                    std::uintptr_t world_commit,
                    Layout* output) {
  if (rbx_owner == 0 || native_angular == 0 || callback_flags == 0 ||
      f64 == 0 || world_commit == 0 || output == nullptr)
    return false;
  Layout result{};
  if (!Add(rbx_owner, kRbxFirstOffset, &result.dr0_rbx_first) ||
      !Add(rbx_owner, kRbxSecondOffset, &result.dr0_rbx_second) ||
      !Add(native_angular, kAngularAuxOffset, &result.dr1_angular_aux))
    return false;
  result.dr1_callback_flags = callback_flags;
  result.dr2_f64 = f64;
  result.dr3_world_commit = world_commit;
  result.parallel_active_dr7 = kParallelActiveDr7;
  result.rbx_disabled_dr7 = kRbxDisabledDr7;
  result.post_f64_dr7 = kPostF64Dr7;

  if ((result.dr0_rbx_first & 3u) != 0 ||
      (result.dr0_rbx_second & 3u) != 0 ||
      (result.dr1_angular_aux & 3u) != 0 ||
      (result.dr1_callback_flags & 1u) != 0 ||
      (result.dr2_f64 & 3u) != 0 ||
      (result.dr3_world_commit & 3u) != 0)
    return false;
  const std::uintptr_t active_addresses[] = {
      result.dr0_rbx_first, result.dr0_rbx_second,
      result.dr1_angular_aux, result.dr2_f64, result.dr3_world_commit};
  const std::size_t active_sizes[] = {4, 4, 4, 4, 4};
  for (std::size_t left = 0; left < 5; ++left) {
    for (std::size_t right = left + 1; right < 5; ++right) {
      if (Overlaps(active_addresses[left], active_sizes[left],
                   active_addresses[right], active_sizes[right]))
        return false;
    }
  }
  if (Overlaps(result.dr1_callback_flags, 2,
               result.dr3_world_commit, 4))
    return false;
  *output = result;
  return true;
}

}  // namespace a9tas::barrel_post_c9c_watch_v1
