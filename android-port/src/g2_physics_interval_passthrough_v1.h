#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::g2_physics_interval_v1 {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kCapacity = 1024;
inline constexpr char kControlMagic[8] = {
    'A', '9', 'G', '2', 'C', '1', '\0', '\0',
};
inline constexpr char kEvidenceMagic[8] = {
    'A', '9', 'G', '2', 'E', '1', '\0', '\0',
};

enum class Command : std::uint32_t {
    kInstallAndArm = 1,
    kRestore = 2,
};

enum Status : std::int32_t {
    kStatusPassive = 0,
    kStatusArmed = 1,
    kStatusComplete = 2,
    kStatusRestored = 3,
    kStatusFault = -1,
};

enum EvidenceFlag : std::uint64_t {
    kGameIdentity = 1ULL << 0,
    kPrologueIdentity = 1ULL << 1,
    kOwnerIdentity = 1ULL << 2,
    kPatchPublished = 1ULL << 3,
    kPatchReadback = 1ULL << 4,
    kTargetLogicalRx = 1ULL << 5,
    kRestoreReadback = 1ULL << 6,
    kRestoredLogicalRx = 1ULL << 7,
    kOutputWriteAbsent = 1ULL << 8,
    kTargetPageIsolated = 1ULL << 9,
};

struct alignas(64) Control {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t enabled;
    std::uint32_t limit;
    std::uint32_t cursor;
    std::uint32_t completed;
    std::uint32_t active_calls;
    std::uint32_t generation;
    std::uint64_t expected_owner;
    std::uint64_t expected_owner_vptr;
    std::uint64_t game_base;
    std::uint64_t target_entry;
    std::uint64_t target_tail;
    std::uint64_t wrapper;
    std::uint64_t reserved[5];
};

struct alignas(64) Evidence {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::int32_t status;
    std::uint32_t first_error;
    std::uint64_t flags;
    std::uint64_t install_calls;
    std::uint64_t restore_calls;
    std::uint64_t wrapper_returns;
    std::uint64_t qualified_returns;
    std::uint64_t unqualified_returns;
    std::uint64_t valid_outputs;
    std::uint64_t semantic_errors;
    std::uint64_t identity_errors;
    std::uint64_t first_tid;
    std::uint64_t last_tid;
    std::uint64_t tid_changes;
    std::uint32_t first_bits;
    std::uint32_t last_bits;
    std::uint64_t reserved[7];
};

struct alignas(64) Event {
    std::uint64_t sequence;
    std::uint64_t owner;
    std::uint64_t output;
    std::uint64_t monotonic_ns;
    std::uint32_t tid;
    std::uint32_t output_bits;
    std::uint32_t owner_vptr_low;
    std::uint32_t commit_sequence;
    std::uint64_t reserved[2];
};

static_assert(sizeof(Control) == 128, "G2 control ABI");
static_assert(sizeof(Evidence) == 192, "G2 evidence ABI");
static_assert(sizeof(Event) == 64, "G2 event ABI");

}  // namespace a9tas::g2_physics_interval_v1
