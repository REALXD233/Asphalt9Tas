#pragma once

// Passive NitroService wrapper protocol used only while recording.  It
// mirrors AluTasV2's EnableNitro detour semantics: count the real call in the
// current authoritative frame, then invoke the exact original function.

#include <cstddef>
#include <cstdint>

namespace a9tas::natural_action_recording_v1 {

inline constexpr char kControlMagic[8] = {'A','9','N','R','C','1',0,0};
inline constexpr char kEvidenceMagic[8] = {'A','9','N','R','E','1',0,0};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaximumFrames = 3600;
inline constexpr std::size_t kActivateSlotOffset = 0x108;
inline constexpr std::size_t kShadowVtableSize = 0x180;

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
  kInstalled = 1u << 1,
};

enum Status : std::int32_t {
  kPassive = 0,
  kCounted = 1,
  kOutOfWindow = -1,
  kUnexpectedService = -2,
  kUnexpectedVptr = -3,
  kFrameOverflow = -4,
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uint32_t session_id;
  std::uint32_t reserved0;
  std::uintptr_t expected_service;
  std::uintptr_t original_vptr;
  std::uintptr_t shadow_vptr;
  std::uintptr_t original_activate;
  std::uint64_t active_sequence;
  std::uint64_t reserved[7];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t clean_returns;
  std::uint64_t counted_calls;
  std::uint64_t out_of_window_calls;
  std::uint64_t overflow_calls;
  std::uint64_t failures;
  std::uint64_t last_sequence;
  std::uintptr_t last_service;
  std::uintptr_t observed_vptr;
  std::int32_t last_status;
  std::uint32_t last_tid;
  std::uint64_t reserved[2];
};

static_assert(sizeof(Control) == 128);
static_assert(sizeof(Evidence) == 128);
static_assert(kActivateSlotOffset + sizeof(std::uintptr_t) <=
              kShadowVtableSize);

}  // namespace a9tas::natural_action_recording_v1

