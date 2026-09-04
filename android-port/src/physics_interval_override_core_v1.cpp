// BUILD-ONLY/OFFLINE AluTasV2 physics-interval override planner.
//
// The core performs no process access and no write. It only accepts the exact
// supported PhysicsContext identity and the inline default step-options path.
// A non-null PhysicsContext+0x170 fails closed because that object owns the
// interval through a separate virtual getter.

#include "physics_interval_override_core_v1.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace a9tas::physics_interval_v1 {

namespace {

bool IntervalBitsValid(std::uint32_t bits) noexcept {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && value >= 0.001f && value <= 0.1f;
}

WritePlanV1 Reject(PlanStatus status) noexcept {
    return {status, 0, 0, 0, false, false};
}

}  // namespace

bool ValidateMetadata(const RecordingMetadataV1& metadata) noexcept {
    return std::memcmp(metadata.magic, kMagic, sizeof(kMagic)) == 0 &&
           metadata.version == kVersion && metadata.size == sizeof(metadata) &&
           metadata.flags == kRequiredFlags &&
           std::memcmp(metadata.build_id, kSupportedBuildId,
                       sizeof(kSupportedBuildId)) == 0 &&
           metadata.context_vtable_rva == kPhysicsContextVtableRva &&
           metadata.getter_rva == kPhysicsContextGetIntervalRva &&
           IntervalBitsValid(metadata.physics_interval_bits) &&
           metadata.reserved == 0;
}

WritePlanV1 BuildWritePlan(const RecordingMetadataV1& metadata,
                           const ObservationV1& observation) noexcept {
    if (!ValidateMetadata(metadata))
        return Reject(PlanStatus::kInvalidMetadata);
    if (observation.library_base == 0 || observation.physics_context == 0 ||
        observation.library_base >
            std::numeric_limits<std::uintptr_t>::max() -
                kPhysicsContextVtableRva ||
        observation.context_vptr !=
            observation.library_base + kPhysicsContextVtableRva)
        return Reject(PlanStatus::kInvalidIdentity);
    if (observation.step_options != 0)
        return Reject(PlanStatus::kAlternateStepOptionsActive);
    if (!IntervalBitsValid(observation.current_interval_bits))
        return Reject(PlanStatus::kInvalidCurrentInterval);
    if (observation.physics_context >
        std::numeric_limits<std::uintptr_t>::max() - kInlineIntervalOffset)
        return Reject(PlanStatus::kAddressOverflow);

    const bool exact = observation.current_interval_bits ==
                       metadata.physics_interval_bits;
    return {
        exact ? PlanStatus::kAlreadyExact : PlanStatus::kReady,
        observation.physics_context + kInlineIntervalOffset,
        observation.current_interval_bits,
        metadata.physics_interval_bits,
        !exact,
        !exact,
    };
}

bool VerifyApplied(const WritePlanV1& plan,
                   std::uint32_t observed_bits) noexcept {
    return (plan.status == PlanStatus::kReady ||
            plan.status == PlanStatus::kAlreadyExact) &&
           plan.address != 0 && observed_bits == plan.desired_bits;
}

bool VerifyRestored(const WritePlanV1& plan,
                    std::uint32_t observed_bits) noexcept {
    return (plan.status == PlanStatus::kReady ||
            plan.status == PlanStatus::kAlreadyExact) &&
           plan.address != 0 && observed_bits == plan.before_bits;
}

}  // namespace a9tas::physics_interval_v1

#if defined(A9TAS_PHYSICS_INTERVAL_OVERRIDE_SELFTEST)
namespace {

using namespace a9tas::physics_interval_v1;

RecordingMetadataV1 ValidMetadata() {
    RecordingMetadataV1 metadata{};
    std::memcpy(metadata.magic, kMagic, sizeof(kMagic));
    metadata.version = kVersion;
    metadata.size = sizeof(metadata);
    metadata.flags = kRequiredFlags;
    std::memcpy(metadata.build_id, kSupportedBuildId,
                sizeof(kSupportedBuildId));
    metadata.context_vtable_rva = kPhysicsContextVtableRva;
    metadata.getter_rva = kPhysicsContextGetIntervalRva;
    const float interval = 1.0f / 60.0f;
    std::memcpy(&metadata.physics_interval_bits, &interval, sizeof(interval));
    return metadata;
}

bool SelfTest() {
    const auto metadata = ValidMetadata();
    const float current = 1.0f / 30.0f;
    std::uint32_t current_bits = 0;
    std::memcpy(&current_bits, &current, sizeof(current));
    ObservationV1 observation{
        0x10000000u,
        0x20000000u,
        0x10000000u + kPhysicsContextVtableRva,
        0,
        current_bits,
    };
    const auto ready = BuildWritePlan(metadata, observation);
    if (ready.status != PlanStatus::kReady || !ready.write_required ||
        !ready.restore_required ||
        ready.address != observation.physics_context + kInlineIntervalOffset ||
        !VerifyApplied(ready, metadata.physics_interval_bits) ||
        !VerifyRestored(ready, current_bits))
        return false;

    observation.current_interval_bits = metadata.physics_interval_bits;
    const auto exact = BuildWritePlan(metadata, observation);
    if (exact.status != PlanStatus::kAlreadyExact || exact.write_required ||
        exact.restore_required ||
        !VerifyApplied(exact, metadata.physics_interval_bits))
        return false;

    observation.step_options = 0x30000000u;
    if (BuildWritePlan(metadata, observation).status !=
        PlanStatus::kAlternateStepOptionsActive)
        return false;
    observation.step_options = 0;
    ++observation.context_vptr;
    if (BuildWritePlan(metadata, observation).status !=
        PlanStatus::kInvalidIdentity)
        return false;

    auto invalid = metadata;
    invalid.reserved = 1;
    return BuildWritePlan(invalid, observation).status ==
           PlanStatus::kInvalidMetadata;
}

}  // namespace
#endif

#if !defined(A9TAS_PHYSICS_INTERVAL_OVERRIDE_NO_MAIN)
int main() {
#if defined(A9TAS_PHYSICS_INTERVAL_OVERRIDE_SELFTEST)
    const bool passed = SelfTest();
    std::printf("PHYSICS_INTERVAL_OVERRIDE_CORE_SELFTEST passed=%d "
                "device_access=0 game_writes=0\n",
                passed ? 1 : 0);
    return passed ? 0 : 1;
#else
    std::puts("PHYSICS_INTERVAL_OVERRIDE_CORE_BUILD_ONLY runtime=disabled "
              "device_access=0 game_writes=0");
    return 0;
#endif
}
#endif
