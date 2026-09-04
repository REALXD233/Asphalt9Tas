#pragma once

// Ordered payload-owned storage staging.  The control flags are deliberately
// published last.  The backend is supplied by a future guarded controller;
// this core itself has no process or device access.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "final_writer_replay_protocol_v1.h"
#include "final_writer_transaction_core_v1.h"

namespace a9tas::final_writer_storage_transaction_v1 {

using namespace a9tas::final_writer_replay_v1;

struct Layout {
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t targets{};
  std::uintptr_t audits{};
  std::uintptr_t evidence{};
};

struct Backend {
  void* context{};
  bool (*write_exact_verified)(void*, std::uintptr_t, const void*, std::size_t){};
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kOverlappingStorage = -2,
  kDisableControlWriteFailed = -3,
  kEvidenceWriteFailed = -4,
  kAuditClearFailed = -5,
  kTargetWriteFailed = -6,
  kShadowWriteFailed = -7,
  kPublishControlWriteFailed = -8,
  kPublishRollbackFailed = -9,
};

inline bool Range(std::uintptr_t begin, std::size_t size,
                  std::uintptr_t* end) {
  if (begin == 0 || size == 0 || end == nullptr || begin > UINTPTR_MAX - size)
    return false;
  *end = begin + size;
  return true;
}

inline bool ValidateLayout(const Layout& layout, std::uint32_t frame_count) {
  if (frame_count < 2 || frame_count > kMaximumFrames ||
      (layout.shadow & 63u) != 0 || (layout.control & 63u) != 0 ||
      (layout.targets & 63u) != 0 || (layout.audits & 63u) != 0 ||
      (layout.evidence & 63u) != 0)
    return false;
  struct Span { std::uintptr_t begin, end; } spans[5]{};
  const std::size_t sizes[5] = {
      kPrimaryShadowSize, sizeof(Control), sizeof(FrameTarget) * frame_count,
      sizeof(FrameAudit) * frame_count, sizeof(Evidence),
  };
  const std::uintptr_t begins[5] = {
      layout.shadow, layout.control, layout.targets, layout.audits,
      layout.evidence,
  };
  for (std::size_t index = 0; index < 5; ++index) {
    spans[index].begin = begins[index];
    if (!Range(begins[index], sizes[index], &spans[index].end)) return false;
  }
  for (std::size_t left = 0; left < 5; ++left)
    for (std::size_t right = left + 1; right < 5; ++right)
      if (spans[left].begin < spans[right].end &&
          spans[right].begin < spans[left].end)
        return false;
  return true;
}

inline Result Stage(const Backend& backend, const Layout& layout,
                    const final_writer_transaction_core_v1::Prepared& prepared,
                    const FrameTarget* targets, std::uint32_t frame_count) {
  if (backend.write_exact_verified == nullptr || targets == nullptr ||
      !ValidateLayout(layout, frame_count) ||
      prepared.unpublished_control.flags != 0 ||
      prepared.unpublished_control.frame_count != frame_count ||
      prepared.published_control.flags !=
          (kControlConfigured | kControlTargetsLoaded) ||
      prepared.published_control.frame_count != frame_count ||
      prepared.shadow_vptr != layout.shadow + kPrimaryPrefixSize)
    return Result::kInvalidArgument;
  const auto write = [&](std::uintptr_t address, const void* data,
                         std::size_t size) {
    return backend.write_exact_verified(backend.context, address, data, size);
  };

  // Disable first.  Even if a stale shadow were unexpectedly present, the
  // wrapper must fail closed before any large storage mutation begins.
  if (!write(layout.control, &prepared.unpublished_control, sizeof(Control)))
    return Result::kDisableControlWriteFailed;
  if (!write(layout.evidence, &prepared.fresh_evidence, sizeof(Evidence)))
    return Result::kEvidenceWriteFailed;
  std::vector<FrameAudit> zero_audits(frame_count);
  if (!write(layout.audits, zero_audits.data(),
             zero_audits.size() * sizeof(FrameAudit)))
    return Result::kAuditClearFailed;
  if (!write(layout.targets, targets,
             static_cast<std::size_t>(frame_count) * sizeof(FrameTarget)))
    return Result::kTargetWriteFailed;
  if (!write(layout.shadow, prepared.shadow.data(), prepared.shadow.size()))
    return Result::kShadowWriteFailed;

  // Publish only after all referenced bytes have been write-read verified.
  if (!write(layout.control, &prepared.published_control, sizeof(Control))) {
    if (!write(layout.control, &prepared.unpublished_control, sizeof(Control)))
      return Result::kPublishRollbackFailed;
    return Result::kPublishControlWriteFailed;
  }
  return Result::kOk;
}

}  // namespace a9tas::final_writer_storage_transaction_v1
