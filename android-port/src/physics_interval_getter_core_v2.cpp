// BUILD-ONLY/OFFLINE source-faithful GetPhysicsInterval contract.
// No process access, hook installation, device access or game write exists here.

#include "physics_interval_getter_core_v2.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace a9tas::physics_interval_v2 {
namespace {

bool IntervalBitsValid(std::uint32_t bits) noexcept {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && value >= 0.001f && value <= 0.1f;
}

GetterDetourPlanV2 Reject(PlanStatus status) noexcept {
    return {status, 0, 0, 0, 0, false, false, false, false};
}

}  // namespace

bool ValidateMetadata(const RecordingMetadataV2& metadata) noexcept {
    return std::memcmp(metadata.magic, kMagic, sizeof(kMagic)) == 0 &&
           metadata.version == kVersion && metadata.size == sizeof(metadata) &&
           metadata.flags == kRequiredFlags &&
           std::memcmp(metadata.build_id, kSupportedBuildId,
                       sizeof(kSupportedBuildId)) == 0 &&
           metadata.context_vtable_rva == kPhysicsContextVtableRva &&
           metadata.step_options_vtable_rva == kStepOptionsVtableRva &&
           metadata.getter_thunk_rva == kGetterAdjustorThunkRva &&
           metadata.owner_adjustment == kOwnerAdjustment &&
           IntervalBitsValid(metadata.physics_interval_bits) &&
           metadata.reserved == 0;
}

GetterDetourPlanV2 BuildGetterDetourPlan(
    const RecordingMetadataV2& metadata,
    const ObservationV2& observation) noexcept {
    if (!ValidateMetadata(metadata))
        return Reject(PlanStatus::kInvalidMetadata);
    if (observation.library_base == 0 || observation.physics_context == 0 ||
        observation.library_base >
            std::numeric_limits<std::uintptr_t>::max() -
                kPhysicsContextVtableRva ||
        observation.context_vptr !=
            observation.library_base + kPhysicsContextVtableRva)
        return Reject(PlanStatus::kInvalidContextIdentity);
    if (observation.step_options == 0)
        return Reject(PlanStatus::kMissingStepOptions);
    if (observation.library_base >
            std::numeric_limits<std::uintptr_t>::max() -
                kStepOptionsVtableRva ||
        observation.step_options_vptr !=
            observation.library_base + kStepOptionsVtableRva)
        return Reject(PlanStatus::kInvalidStepOptionsIdentity);
    if (observation.library_base >
            std::numeric_limits<std::uintptr_t>::max() -
                kGetterAdjustorThunkRva ||
        observation.getter_target !=
            observation.library_base + kGetterAdjustorThunkRva)
        return Reject(PlanStatus::kInvalidGetterIdentity);
    if (observation.owner_adjustment != kOwnerAdjustment)
        return Reject(PlanStatus::kInvalidOwnerAdjustment);
    if (!IntervalBitsValid(observation.effective_interval_bits))
        return Reject(PlanStatus::kInvalidEffectiveInterval);
    if (observation.step_options <
        static_cast<std::uintptr_t>(-kOwnerAdjustment))
        return Reject(PlanStatus::kAddressOverflow);

    return {
        PlanStatus::kReadyGetterDetour,
        observation.getter_target,
        observation.step_options,
        observation.step_options -
            static_cast<std::uintptr_t>(-kOwnerAdjustment),
        observation.effective_interval_bits,
        true,
        true,
        true,
        false,
    };
}

DetourResultV2 ApplyOriginalSemantics(
    const RecordingMetadataV2& metadata,
    std::uint32_t original_output_bits,
    bool override_enabled) noexcept {
    if (!ValidateMetadata(metadata) || !IntervalBitsValid(original_output_bits))
        return {false, 0, 0};
    const std::uint32_t final_bits =
        override_enabled ? metadata.physics_interval_bits : original_output_bits;
    return {true, final_bits, final_bits};
}

}  // namespace a9tas::physics_interval_v2

#if defined(A9TAS_PHYSICS_INTERVAL_GETTER_SELFTEST)
namespace {
using namespace a9tas::physics_interval_v2;

RecordingMetadataV2 Metadata() {
    RecordingMetadataV2 metadata{};
    std::memcpy(metadata.magic, kMagic, sizeof(kMagic));
    metadata.version = kVersion;
    metadata.size = sizeof(metadata);
    metadata.flags = kRequiredFlags;
    std::memcpy(metadata.build_id, kSupportedBuildId, sizeof(kSupportedBuildId));
    metadata.context_vtable_rva = kPhysicsContextVtableRva;
    metadata.step_options_vtable_rva = kStepOptionsVtableRva;
    metadata.getter_thunk_rva = kGetterAdjustorThunkRva;
    metadata.owner_adjustment = kOwnerAdjustment;
    const float interval = 1.0f / 60.0f;
    std::memcpy(&metadata.physics_interval_bits, &interval, sizeof(interval));
    return metadata;
}

bool SelfTest() {
    const auto metadata = Metadata();
    const std::uintptr_t base = 0x10000000u;
    const float observed = 1.0f / 120.0f;
    std::uint32_t observed_bits = 0;
    std::memcpy(&observed_bits, &observed, sizeof(observed));
    ObservationV2 observation{
        base,
        0x20000000u,
        base + kPhysicsContextVtableRva,
        0x30002A78u,
        base + kStepOptionsVtableRva,
        base + kGetterAdjustorThunkRva,
        kOwnerAdjustment,
        observed_bits,
    };
    const auto plan = BuildGetterDetourPlan(metadata, observation);
    if (plan.status != PlanStatus::kReadyGetterDetour ||
        plan.hook_target != base + kGetterAdjustorThunkRva ||
        plan.owner != 0x30000000u || !plan.call_original_first ||
        !plan.override_after_original || !plan.capture_final_output ||
        plan.inline_field_write_allowed)
        return false;
    const auto record = ApplyOriginalSemantics(metadata, observed_bits, false);
    if (!record.valid || record.final_output_bits != observed_bits ||
        record.metadata_after_call_bits != observed_bits)
        return false;
    const auto replay = ApplyOriginalSemantics(metadata, observed_bits, true);
    if (!replay.valid ||
        replay.final_output_bits != metadata.physics_interval_bits ||
        replay.metadata_after_call_bits != metadata.physics_interval_bits)
        return false;
    observation.getter_target++;
    return BuildGetterDetourPlan(metadata, observation).status ==
           PlanStatus::kInvalidGetterIdentity;
}
}  // namespace
#endif

#if !defined(A9TAS_PHYSICS_INTERVAL_GETTER_NO_MAIN)
int main() {
#if defined(A9TAS_PHYSICS_INTERVAL_GETTER_SELFTEST)
    const bool passed = SelfTest();
    std::printf("PHYSICS_INTERVAL_GETTER_CORE_V2_SELFTEST passed=%d "
                "runtime=disabled device_access=0 game_writes=0\n",
                passed ? 1 : 0);
    return passed ? 0 : 1;
#else
    std::puts("PHYSICS_INTERVAL_GETTER_CORE_V2_BUILD_ONLY "
              "runtime=disabled device_access=0 game_writes=0");
    return 0;
#endif
}
#endif
