#pragma once

#include "g4_g3_adapter_v1.h"
#include <cstddef>
#include <cstdint>

namespace a9tas::completed_frame_scope_v1 {

// Caller serializes access with the existing runtime lock. No TLS, allocation,
// syscalls or game access: NativeBridge does not guarantee a TLS resolver.
template <std::size_t Threads = 8, std::size_t Depth = 8>
struct Stack {
  using Witness = g4_g3_adapter_v1::CompletedFrameScopeV1;
  struct Slot {
    std::uint32_t tid{};
    std::uint32_t depth{};
    Witness witnesses[Depth]{};
  };
  Slot slots[Threads]{};

  bool Push(std::uint32_t tid, Witness witness) noexcept {
    if (tid == 0) return false;
    Slot* slot = nullptr;
    for (auto& candidate : slots) {
      if (candidate.tid == tid) { slot = &candidate; break; }
      if (candidate.tid == 0 && slot == nullptr) slot = &candidate;
    }
    if (slot == nullptr || slot->depth == UINT32_MAX) return false;
    slot->tid = tid;
    // Push even an unqualified witness: nested unrelated callbacks must mask
    // an outer qualified callback until they return.
    if (slot->depth < Depth) slot->witnesses[slot->depth] = witness;
    ++slot->depth;
    return true;
  }

  const Witness* Current(std::uint32_t tid) const noexcept {
    for (const auto& slot : slots)
      if (slot.tid == tid && slot.depth != 0 && slot.depth <= Depth) {
        const auto& witness = slot.witnesses[slot.depth - 1];
        return witness.tid == tid && witness.generation != 0 ? &witness : nullptr;
      }
    return nullptr;
  }

  void Pop(std::uint32_t tid) noexcept {
    for (auto& slot : slots)
      if (slot.tid == tid && slot.depth != 0) {
        --slot.depth;
        if (slot.depth < Depth) slot.witnesses[slot.depth] = {};
        if (slot.depth == 0) slot.tid = 0;
        return;
      }
  }
};
}  // namespace a9tas::completed_frame_scope_v1
