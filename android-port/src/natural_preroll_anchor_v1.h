#pragma once

#include <cstdint>

namespace a9tas::natural_preroll_v1 {

constexpr char kMagic[8] = {'A', '9', 'N', 'P', 'A', '1', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;

enum Flag : std::uint32_t {
    kTargetVerified = 1u << 0,
    kIdentityVerified = 1u << 1,
    kAnchorCertified = 1u << 2,
    kSourceHashesBound = 1u << 3,
    kFrame0Bound = 1u << 4,
};

constexpr std::uint32_t kUnboundFlags =
    kTargetVerified | kIdentityVerified | kAnchorCertified | kFrame0Bound;
constexpr std::uint32_t kBoundFlags = kUnboundFlags | kSourceHashesBound;

#pragma pack(push, 1)
struct AnchorV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t fixed_interval_us;
    std::uint32_t frame_count;
    std::uint32_t reserved0;
    std::uint8_t build_id[20];
    std::uint8_t reserved1[4];
    std::uint8_t recording_sha256[32];
    std::uint8_t report_sha256[32];
    std::uint64_t source_pid;
    std::uint64_t source_library_base;
    std::uint64_t source_main_object;
    std::uint64_t source_final_owner;
    std::uint64_t source_physics_context;
    std::uint64_t source_native_body;
    std::int32_t cycle_tid;
    std::int32_t commit_tid;
    std::uint64_t events[7];
    std::uint64_t completion_before;
    std::uint64_t completion_after;
    std::uint16_t callback_flags;
    std::uint8_t reserved2[6];
    std::uint64_t c98_pair_after;
    std::uint64_t c9c_pair_after;
    std::uint8_t anchor_transform[64];
    std::uint8_t anchor_linear[12];
    std::uint8_t frame0_transform[64];
    std::uint8_t frame0_linear[12];
};
#pragma pack(pop)

static_assert(sizeof(AnchorV1) == 424, "A9NPA1 ABI");

}  // namespace a9tas::natural_preroll_v1
