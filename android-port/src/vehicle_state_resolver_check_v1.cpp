// Read-only runtime check for the exact resolver shared by scheduler replay.

#include "vehicle_state_resolver_v1.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PID LIB_BASE_HEX\n", argv[0]);
        return 2;
    }
    errno = 0;
    char* pid_end = nullptr;
    char* base_end = nullptr;
    const long pid_long = std::strtol(argv[1], &pid_end, 10);
    const unsigned long long base_value =
        std::strtoull(argv[2], &base_end, 16);
    if (errno != 0 || pid_end == argv[1] || *pid_end != '\0' ||
        base_end == argv[2] || *base_end != '\0' || pid_long <= 0 ||
        base_value == 0)
        return 2;
    const pid_t pid = static_cast<pid_t>(pid_long);
    const auto base = static_cast<std::uintptr_t>(base_value);
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 3;
    a9tas::vehicle_state_v1::Layout layout{};
    std::uint64_t scanned = 0;
    const bool resolved =
        a9tas::vehicle_state_v1::Resolve(pid, mem, base, &layout, &scanned);
    a9tas::vehicle_state_v1::Snapshot snapshot{};
    const bool snapshot_ok =
        resolved && a9tas::vehicle_state_v1::ReadSnapshot(mem, layout, &snapshot);
    a9tas::vehicle_state_v1::BackendLayout backend{};
    const bool backend_ok =
        resolved && a9tas::vehicle_state_v1::ResolveBackendLayout(
                        mem, base, layout, &backend);
    float source_linear[3]{};
    float source_angular[3]{};
    float native_linear[3]{};
    float native_angular[3]{};
    float native_pose[16]{};
    // Static candidates from the late VehicleStuntState stabilization chain.
    // These reads are diagnostic only; they are not part of the proven replay
    // ABI and are deliberately excluded from the process exit condition.
    constexpr std::uintptr_t kBarrelRbxCandidateOffset = 0x1968;
    constexpr std::uintptr_t kVehicleStuntStateOffset = 0x1D80;
    float layout_barrel_rbx_candidate[2]{};
    std::uint32_t layout_vehicle_stunt_state = 0;
    const bool layout_barrel_candidate_ok =
        resolved && layout.physics_base != 0 &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, layout.physics_base + kBarrelRbxCandidateOffset,
            layout_barrel_rbx_candidate,
            sizeof(layout_barrel_rbx_candidate)) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, layout.physics_base + kVehicleStuntStateOffset,
            &layout_vehicle_stunt_state,
            sizeof(layout_vehicle_stunt_state)) &&
        a9tas::vehicle_state_v1::Finite(layout_barrel_rbx_candidate, 2);
    const bool backend_state_ok =
        backend_ok &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.linear_source_address, source_linear,
            sizeof(source_linear)) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.angular_source_address, source_angular,
            sizeof(source_angular)) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.native_linear_address, native_linear,
            sizeof(native_linear)) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.native_angular_address, native_angular,
            sizeof(native_angular)) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.native_pose_address, native_pose,
            sizeof(native_pose)) &&
        a9tas::vehicle_state_v1::Finite(source_linear, 3) &&
        a9tas::vehicle_state_v1::Finite(source_angular, 3) &&
        a9tas::vehicle_state_v1::Finite(native_linear, 3) &&
        a9tas::vehicle_state_v1::Finite(native_angular, 3) &&
        a9tas::vehicle_state_v1::Finite(native_pose, 16);
    float source_barrel_rbx_candidate[2]{};
    std::uint32_t source_vehicle_stunt_state = 0;
    const bool source_barrel_candidate_ok =
        backend_ok && backend.angular_source_base != 0 &&
        backend.angular_source_base <=
            UINTPTR_MAX - kVehicleStuntStateOffset - sizeof(std::uint32_t) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.angular_source_base + kBarrelRbxCandidateOffset,
            source_barrel_rbx_candidate,
            sizeof(source_barrel_rbx_candidate)) &&
        a9tas::vehicle_state_v1::ReadExact(
            mem, backend.angular_source_base + kVehicleStuntStateOffset,
            &source_vehicle_stunt_state,
            sizeof(source_vehicle_stunt_state)) &&
        a9tas::vehicle_state_v1::Finite(source_barrel_rbx_candidate, 2);
    close(mem);
    std::printf(
        "VEHICLE_STATE_RESOLVER_CHECK_V1 resolved=%u snapshot=%u "
        "scanned=%" PRIu64 " interface=0x%" PRIxPTR
        " physics=0x%" PRIxPTR " position_addr=0x%" PRIxPTR
        " rotation_addr=0x%" PRIxPTR " linear_addr=0x%" PRIxPTR
        " angular_addr=0x%" PRIxPTR "\n",
        resolved ? 1u : 0u, snapshot_ok ? 1u : 0u, scanned,
        layout.interface, layout.physics_base, layout.position_address,
        layout.rotation_address, layout.linear_address,
        layout.angular_address);
    std::printf(
        "VEHICLE_DELEGATE resolved=%u failure_stage=%u interface=0x%" PRIxPTR
        " interface_vtable=0x%" PRIxPTR " base=0x%" PRIxPTR
        " base_vtable=0x%" PRIxPTR
        " delegate=0x%" PRIxPTR " delegate_vtable=0x%" PRIxPTR
        " wrapper40=0x%" PRIxPTR " wrapper48=0x%" PRIxPTR
        " delegate40=0x%" PRIxPTR " delegate48=0x%" PRIxPTR "\n",
        backend_ok ? 1u : 0u, backend.failure_stage, backend.interface,
        backend.interface_vtable,
        backend.base, backend.base_vtable, backend.delegate,
        backend.delegate_vtable, backend.wrapper_slots[0],
        backend.wrapper_slots[1], backend.delegate_slots[0],
        backend.delegate_slots[1]);
    std::printf(
        "VEHICLE_SOURCE resolved=%u linear_base=0x%" PRIxPTR
        " angular_base=0x%" PRIxPTR " linear_addr=0x%" PRIxPTR
        " angular_addr=0x%" PRIxPTR "\n",
        backend_state_ok ? 1u : 0u, backend.linear_source_base,
        backend.angular_source_base, backend.linear_source_address,
        backend.angular_source_address);
    std::printf(
        "VEHICLE_SOURCE_PRODUCER vtable=0x%" PRIxPTR
        " update=0x%" PRIxPTR " physics_interface=0x%" PRIxPTR
        " physics_vtable=0x%" PRIxPTR " set_pose=0x%" PRIxPTR
        " set_position=0x%" PRIxPTR " set_rotation=0x%" PRIxPTR
        " set_linear=0x%" PRIxPTR " set_angular=0x%" PRIxPTR
        " linear_getter=0x%" PRIxPTR " angular_getter=0x%" PRIxPTR "\n",
        backend.source_vtable, backend.source_update,
        backend.physics_velocity_interface, backend.physics_velocity_vtable,
        backend.physics_set_pose, backend.physics_set_position,
        backend.physics_set_rotation, backend.physics_set_linear,
        backend.physics_set_angular,
        backend.physics_linear_getter, backend.physics_angular_getter);
    std::printf(
        "PHYSICS_NATIVE_BODY body=0x%" PRIxPTR
        " vtable=0x%" PRIxPTR " pose_addr=0x%" PRIxPTR
        " pose_commit_tail=0x%" PRIxPTR " linear_addr=0x%" PRIxPTR
        " angular_addr=0x%" PRIxPTR "\n",
        backend.native_body, backend.native_body_vtable,
        backend.native_pose_address,
        backend.native_pose_commit_tail_address,
        backend.native_linear_address,
        backend.native_angular_address);
    std::printf(
        "BARREL_RBX_LAYOUT_CANDIDATE valid=%u base=0x%" PRIxPTR
        " values_addr=0x%" PRIxPTR " state_addr=0x%" PRIxPTR
        " state=%" PRIu32 " values=(%.9g,%.9g)\n",
        layout_barrel_candidate_ok ? 1u : 0u, layout.physics_base,
        layout.physics_base == 0
            ? 0
            : layout.physics_base + kBarrelRbxCandidateOffset,
        layout.physics_base == 0
            ? 0
            : layout.physics_base + kVehicleStuntStateOffset,
        layout_vehicle_stunt_state, layout_barrel_rbx_candidate[0],
        layout_barrel_rbx_candidate[1]);
    std::printf(
        "BARREL_RBX_SOURCE_CANDIDATE valid=%u base=0x%" PRIxPTR
        " values_addr=0x%" PRIxPTR " state_addr=0x%" PRIxPTR
        " state=%" PRIu32 " values=(%.9g,%.9g)\n",
        source_barrel_candidate_ok ? 1u : 0u,
        backend.angular_source_base,
        backend.angular_source_base == 0
            ? 0
            : backend.angular_source_base + kBarrelRbxCandidateOffset,
        backend.angular_source_base == 0
            ? 0
            : backend.angular_source_base + kVehicleStuntStateOffset,
        source_vehicle_stunt_state, source_barrel_rbx_candidate[0],
        source_barrel_rbx_candidate[1]);
    if (snapshot_ok) {
        std::printf(
            "STATE position=(%.9g,%.9g,%.9g) "
            "rotation=(%.9g,%.9g,%.9g,%.9g) "
            "linear=(%.9g,%.9g,%.9g) angular=(%.9g,%.9g,%.9g)\n",
            snapshot.position[0], snapshot.position[1], snapshot.position[2],
            snapshot.rotation[0], snapshot.rotation[1], snapshot.rotation[2],
            snapshot.rotation[3], snapshot.linear[0], snapshot.linear[1],
            snapshot.linear[2], snapshot.angular[0], snapshot.angular[1],
            snapshot.angular[2]);
    }
    if (backend_state_ok) {
        std::printf(
            "VEHICLE_SOURCE_VALUES linear=(%.9g,%.9g,%.9g) "
            "angular=(%.9g,%.9g,%.9g)\n"
            "PHYSICS_NATIVE_VALUES linear=(%.9g,%.9g,%.9g) "
            "angular=(%.9g,%.9g,%.9g)\n",
            source_linear[0], source_linear[1], source_linear[2],
            source_angular[0], source_angular[1], source_angular[2],
            native_linear[0], native_linear[1], native_linear[2],
            native_angular[0], native_angular[1], native_angular[2]);
        std::printf(
            "PHYSICS_NATIVE_POSE "
            "basis0=(%.9g,%.9g,%.9g,%.9g) "
            "basis1=(%.9g,%.9g,%.9g,%.9g) "
            "basis2=(%.9g,%.9g,%.9g,%.9g) "
            "origin=(%.9g,%.9g,%.9g,%.9g)\n",
            native_pose[0], native_pose[1], native_pose[2], native_pose[3],
            native_pose[4], native_pose[5], native_pose[6], native_pose[7],
            native_pose[8], native_pose[9], native_pose[10], native_pose[11],
            native_pose[12], native_pose[13], native_pose[14], native_pose[15]);
    }
    return snapshot_ok && backend_state_ok ? 0 : 4;
}
