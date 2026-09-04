#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::physics_interval_shadow_v2 {

inline constexpr std::uintptr_t kStepOptionsVtableRva = 0x7EED420;
inline constexpr std::uintptr_t kFirstSlotRva = 0x3693228;
inline constexpr std::uintptr_t kSecondSlotRva = 0x3693264;
inline constexpr std::uintptr_t kGetterThunkRva = 0x3695740;
inline constexpr std::intptr_t kOwnerAdjustment = -0x2A78;
inline constexpr std::intptr_t kNextVtableAdjustment = -0x2A80;
inline constexpr std::size_t kNegativeWords = 4;
inline constexpr std::size_t kGetterWord = kNegativeWords + 2;
inline constexpr std::size_t kShadowWords = 16;

struct Observation {
    std::uintptr_t library_base;
    std::uintptr_t step_options;
    std::uintptr_t observed_vptr;
    std::uintptr_t payload_wrapper;
    std::uintptr_t payload_shadow_storage;
    std::uintptr_t payload_continue_storage;
    std::uintptr_t original_words[kShadowWords];
};

enum class Status : std::uint32_t {
    kReady = 0,
    kInvalidAddress = 1,
    kInvalidOriginalVptr = 2,
    kInvalidOwnerAdjustment = 3,
    kInvalidGetterSlot = 4,
    kInvalidPayloadRange = 5,
    kInvalidTableBoundary = 6,
};

struct Plan {
    Status status;
    std::uintptr_t step_options;
    std::uintptr_t original_vptr;
    std::uintptr_t shadow_storage;
    std::uintptr_t shadow_vptr;
    std::uintptr_t continue_storage;
    std::uintptr_t original_getter;
    std::uintptr_t wrapper;
    std::uintptr_t shadow_words[kShadowWords];
    bool copy_shadow_first;
    bool publish_continue_second;
    bool swap_object_vptr_last;
    bool restore_original_vptr_first;
};

Plan BuildPlan(const Observation& observation) noexcept;
bool VerifyArmed(const Plan& plan, std::uintptr_t object_vptr,
                 std::uintptr_t continuation,
                 const std::uintptr_t shadow_words[kShadowWords]) noexcept;
bool VerifyRestored(const Plan& plan, std::uintptr_t object_vptr) noexcept;

}  // namespace a9tas::physics_interval_shadow_v2
