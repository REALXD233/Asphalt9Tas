// Offline-only shadow-vptr transaction planner for the active physics interval
// provider. It performs no process access and no writes.

#include "physics_interval_getter_shadow_transaction_v2.h"

#include <cstring>
#include <limits>

namespace a9tas::physics_interval_shadow_v2 {
namespace {

Plan Reject(Status status) noexcept {
    Plan plan{};
    plan.status = status;
    return plan;
}

bool Add(std::uintptr_t value, std::uintptr_t amount,
         std::uintptr_t* result) noexcept {
    if (value > std::numeric_limits<std::uintptr_t>::max() - amount)
        return false;
    *result = value + amount;
    return true;
}

}  // namespace

Plan BuildPlan(const Observation& observation) noexcept {
    if (observation.library_base == 0 || observation.step_options == 0 ||
        observation.payload_wrapper == 0 ||
        observation.payload_shadow_storage == 0 ||
        observation.payload_continue_storage == 0)
        return Reject(Status::kInvalidAddress);
    std::uintptr_t expected_vptr = 0, expected_first = 0,
                   expected_second = 0, expected_getter = 0;
    if (!Add(observation.library_base, kStepOptionsVtableRva,
             &expected_vptr) ||
        !Add(observation.library_base, kFirstSlotRva, &expected_first) ||
        !Add(observation.library_base, kSecondSlotRva, &expected_second) ||
        !Add(observation.library_base, kGetterThunkRva, &expected_getter))
        return Reject(Status::kInvalidAddress);
    if (observation.observed_vptr != expected_vptr)
        return Reject(Status::kInvalidOriginalVptr);
    if (static_cast<std::intptr_t>(observation.original_words[0]) !=
            kOwnerAdjustment ||
        static_cast<std::intptr_t>(observation.original_words[1]) !=
            kOwnerAdjustment ||
        static_cast<std::intptr_t>(observation.original_words[2]) !=
            kOwnerAdjustment ||
        observation.original_words[3] != 0)
        return Reject(Status::kInvalidOwnerAdjustment);
    if (observation.original_words[kNegativeWords] != expected_first ||
        observation.original_words[kNegativeWords + 1] != expected_second ||
        observation.original_words[kGetterWord] != expected_getter)
        return Reject(Status::kInvalidGetterSlot);
    // Slot 2 is the final callable entry of this sub-vtable. The next word is
    // the signed adjustment metadata of the following sub-vtable, not a fourth
    // function pointer.
    if (static_cast<std::intptr_t>(
            observation.original_words[kNegativeWords + 3]) !=
        kNextVtableAdjustment)
        return Reject(Status::kInvalidTableBoundary);
    std::uintptr_t shadow_vptr = 0;
    if (!Add(observation.payload_shadow_storage,
             kNegativeWords * sizeof(std::uintptr_t), &shadow_vptr) ||
        observation.payload_shadow_storage % alignof(std::uintptr_t) != 0 ||
        observation.payload_continue_storage % alignof(std::uintptr_t) != 0)
        return Reject(Status::kInvalidPayloadRange);

    Plan plan{};
    plan.status = Status::kReady;
    plan.step_options = observation.step_options;
    plan.original_vptr = expected_vptr;
    plan.shadow_storage = observation.payload_shadow_storage;
    plan.shadow_vptr = shadow_vptr;
    plan.continue_storage = observation.payload_continue_storage;
    plan.original_getter = expected_getter;
    plan.wrapper = observation.payload_wrapper;
    std::memcpy(plan.shadow_words, observation.original_words,
                sizeof(plan.shadow_words));
    plan.shadow_words[kGetterWord] = observation.payload_wrapper;
    plan.copy_shadow_first = true;
    plan.publish_continue_second = true;
    plan.swap_object_vptr_last = true;
    plan.restore_original_vptr_first = true;
    return plan;
}

bool VerifyArmed(const Plan& plan, std::uintptr_t object_vptr,
                 std::uintptr_t continuation,
                 const std::uintptr_t shadow_words[kShadowWords]) noexcept {
    return plan.status == Status::kReady && object_vptr == plan.shadow_vptr &&
           continuation == plan.original_getter && shadow_words != nullptr &&
           std::memcmp(plan.shadow_words, shadow_words,
                       sizeof(plan.shadow_words)) == 0;
}

bool VerifyRestored(const Plan& plan, std::uintptr_t object_vptr) noexcept {
    return plan.status == Status::kReady &&
           object_vptr == plan.original_vptr;
}

}  // namespace a9tas::physics_interval_shadow_v2

#if defined(A9TAS_PHYSICS_INTERVAL_SHADOW_SELFTEST)
#include <cstdio>

namespace {
using namespace a9tas::physics_interval_shadow_v2;

Observation ValidObservation() {
    Observation observation{};
    observation.library_base = 0x10000000u;
    observation.step_options = 0x30002A78u;
    observation.observed_vptr =
        observation.library_base + kStepOptionsVtableRva;
    observation.payload_wrapper = 0x50001000u;
    observation.payload_shadow_storage = 0x50002000u;
    observation.payload_continue_storage = 0x50003000u;
    observation.original_words[0] =
        static_cast<std::uintptr_t>(kOwnerAdjustment);
    observation.original_words[1] =
        static_cast<std::uintptr_t>(kOwnerAdjustment);
    observation.original_words[2] =
        static_cast<std::uintptr_t>(kOwnerAdjustment);
    observation.original_words[3] = 0;
    observation.original_words[4] = observation.library_base + 0x3693228;
    observation.original_words[5] = observation.library_base + 0x3693264;
    observation.original_words[6] =
        observation.library_base + kGetterThunkRva;
    observation.original_words[7] =
        static_cast<std::uintptr_t>(kNextVtableAdjustment);
    for (std::size_t index = 8; index < kShadowWords; ++index)
        observation.original_words[index] = observation.library_base + index;
    return observation;
}

bool SelfTest() {
    auto observation = ValidObservation();
    const Plan plan = BuildPlan(observation);
    if (plan.status != Status::kReady ||
        plan.shadow_vptr != observation.payload_shadow_storage + 32 ||
        plan.shadow_words[kGetterWord] != observation.payload_wrapper ||
        !plan.copy_shadow_first || !plan.publish_continue_second ||
        !plan.swap_object_vptr_last || !plan.restore_original_vptr_first ||
        !VerifyArmed(plan, plan.shadow_vptr, plan.original_getter,
                     plan.shadow_words) ||
        !VerifyRestored(plan, plan.original_vptr))
        return false;
    observation.original_words[0] = 0;
    if (BuildPlan(observation).status != Status::kInvalidOwnerAdjustment)
        return false;
    observation = ValidObservation();
    ++observation.original_words[kGetterWord];
    if (BuildPlan(observation).status != Status::kInvalidGetterSlot)
        return false;
    observation = ValidObservation();
    observation.original_words[kNegativeWords + 3] = 0;
    return BuildPlan(observation).status == Status::kInvalidTableBoundary;
}
}  // namespace

int main() {
    const bool passed = SelfTest();
    std::printf("PHYSICS_INTERVAL_SHADOW_TRANSACTION_V2_SELFTEST passed=%d "
                "runtime=disabled process_access=0 game_writes=0\n",
                passed ? 1 : 0);
    return passed ? 0 : 1;
}
#elif !defined(A9TAS_PHYSICS_INTERVAL_SHADOW_NO_MAIN)
#include <cstdio>
int main() {
    std::puts("PHYSICS_INTERVAL_SHADOW_TRANSACTION_V2_BUILD_ONLY "
              "runtime=disabled process_access=0 game_writes=0");
    return 0;
}
#endif
