#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::physics_interval_payload_v2 {

inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint32_t kCapacity = 4096;
inline constexpr char kControlMagic[8] = {
    'A', '9', 'P', 'G', 'C', '2', '\0', '\0',
};
inline constexpr char kEvidenceMagic[8] = {
    'A', '9', 'P', 'G', 'E', '2', '\0', '\0',
};

enum class Mode : std::uint32_t {
    kIdle = 0,
    kRecord = 1,
    kReplay = 2,
};

enum EventFlag : std::uint32_t {
    kOriginalCalled = 1u << 0,
    kOriginalValid = 1u << 1,
    kOverrideRequested = 1u << 2,
    kOverrideApplied = 1u << 3,
    kFinalCaptured = 1u << 4,
    kObjectMatched = 1u << 5,
};

enum Status : std::int32_t {
    kStatusPassive = 0,
    kStatusArmed = 1,
    kStatusComplete = 2,
    kStatusFault = -1,
};

struct alignas(64) Control {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t mode;
    std::uint32_t enabled;
    std::uint32_t limit;
    std::uint32_t cursor;
    std::uint32_t completed;
    std::uint32_t active_calls;
    std::uint64_t expected_object;
    std::uint64_t expected_vptr;
    std::uint64_t original_getter;
};

struct Event {
    std::uint64_t sequence;
    std::uint64_t object;
    std::uint64_t output;
    std::uint64_t monotonic_ns;
    std::uint32_t tid;
    std::uint32_t original_bits;
    std::uint32_t requested_bits;
    std::uint32_t final_bits;
    std::uint32_t flags;
    std::uint32_t commit_sequence;
    std::uint64_t reserved;
};

struct alignas(64) Evidence {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::int32_t status;
    std::uint32_t reserved32;
    std::uint64_t calls;
    std::uint64_t record_calls;
    std::uint64_t replay_calls;
    std::uint64_t overrides;
    std::uint64_t semantic_errors;
    std::uint64_t object_mismatches;
    std::uint64_t first_tid;
    std::uint64_t last_tid;
    std::uint64_t tid_changes;
    std::uint64_t reserved[3];
};

static_assert(sizeof(Control) == 64, "physics interval control ABI");
static_assert(sizeof(Event) == 64, "physics interval event ABI");
static_assert(sizeof(Evidence) == 128, "physics interval evidence ABI");

}  // namespace a9tas::physics_interval_payload_v2
