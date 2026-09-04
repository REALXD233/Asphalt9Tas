#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

namespace a9tas::vehicle_state_v1 {

constexpr std::uintptr_t kPhysicsInterfaceVtableRvas[] = {
    0x7EE9B78, 0x80C8098, 0x80C9978, 0x80CAF40, 0x80E74A0,
    0x80E8DF0, 0x80EA3B8, 0x815DC08, 0x815F3E8,
};
constexpr std::uintptr_t kLinearOffset = 0xF50;
constexpr std::uintptr_t kAngularOffset = 0xF5C;
constexpr std::uintptr_t kPositionGetterRva = 0x4D10D38;
constexpr std::uintptr_t kRotationGetterRva = 0x4D10D4C;
constexpr std::uintptr_t kWrapperSlot40Rva = 0x36A9940;
constexpr std::uintptr_t kWrapperSlot48Rva = 0x36A9954;
constexpr std::uintptr_t kWrapperSlot58Rva = 0x36A8DC8;
constexpr std::uintptr_t kWrapperSlot60Rva = 0x36A8A3C;
constexpr std::uintptr_t kWrapperSlot68Rva = 0x36A8A50;
constexpr std::uintptr_t kWrapperSlot88Rva = 0x36A8A78;
constexpr std::uintptr_t kWrapperSlot90Rva = 0x36A9C20;
constexpr std::uintptr_t kWrapperSlot98Rva = 0x36A8C14;
constexpr std::uintptr_t kWrapperSlotA0Rva = 0x36A8C28;
constexpr std::uintptr_t kDelegateSlot40Rva = 0x36AE8C0;
constexpr std::uintptr_t kDelegateSlot48Rva = 0x36AE8EC;
constexpr std::uintptr_t kDelegateSlot58Rva = 0x36ACCD4;
constexpr std::uintptr_t kDelegateSlot60Rva = 0x36AC558;
constexpr std::uintptr_t kDelegateSlot68Rva = 0x36AC570;
constexpr std::uintptr_t kDelegateSlot88Rva = 0x36AC590;
constexpr std::uintptr_t kDelegateSlot90Rva = 0x36AEA20;
constexpr std::uintptr_t kDelegateSlot98Rva = 0x36AC980;
constexpr std::uintptr_t kDelegateSlotA0Rva = 0x36AC9E0;
constexpr std::uintptr_t kVehicleSourceVtableRva = 0x7EF2F48;
constexpr std::uintptr_t kVehicleSourceUpdateSlot = 0x58;
constexpr std::uintptr_t kVehicleSourceUpdateRva = 0x36AE400;
constexpr std::uintptr_t kCarPhysicsBodySourceVtableRva = 0x7EECD88;
constexpr std::uintptr_t kCarPhysicsBodySourceUpdateRva = 0x36999E0;
constexpr std::uintptr_t kPhysicsVelocityInterfaceOffset = 0x18;
constexpr std::uintptr_t kPhysicsBackendVtableRva = 0x9D5ED40;
constexpr std::uintptr_t kNativePhysicsBodyVtableRva = 0x9EDC6A0;
constexpr std::uintptr_t kPhysicsSetPoseSlot = 0x58;
constexpr std::uintptr_t kPhysicsSetPositionSlot = 0x60;
constexpr std::uintptr_t kPhysicsSetRotationSlot = 0x68;
constexpr std::uintptr_t kPhysicsSetLinearSlot = 0x98;
constexpr std::uintptr_t kPhysicsSetAngularSlot = 0xA0;
constexpr std::uintptr_t kPhysicsLinearGetterSlot = 0xA8;
constexpr std::uintptr_t kPhysicsAngularGetterSlot = 0xB0;
constexpr std::uintptr_t kPhysicsSetPoseRva = 0x4CC58CC;
constexpr std::uintptr_t kPhysicsSetPositionRva = 0x4CC5A18;
constexpr std::uintptr_t kPhysicsSetRotationRva = 0x4CC5AAC;
constexpr std::uintptr_t kPhysicsSetLinearRva = 0x4CC5C18;
constexpr std::uintptr_t kPhysicsSetAngularRva = 0x4CC5C34;
constexpr std::uintptr_t kPhysicsGetLinearRva = 0x4CC5C50;
constexpr std::uintptr_t kPhysicsGetAngularRva = 0x4CC5C64;

// Semantic addresses resolved from an exact game ELF.  Keeping them in a
// value object lets the established object-graph scanner serve additional
// channel builds without compiling package names or build-specific branches
// into the resolver.  The legacy overloads below use ReferenceProfile().
struct Profile {
    std::uintptr_t physics_interface_vtable_rvas[9]{};
    std::uintptr_t position_getter_rva{};
    std::uintptr_t rotation_getter_rva{};
    std::uintptr_t wrapper_rvas[9]{};
    std::uintptr_t delegate_rvas[9]{};
    std::uintptr_t vehicle_source_vtable_rva{};
    std::uintptr_t vehicle_source_update_rva{};
    std::uintptr_t car_physics_body_source_vtable_rva{};
    std::uintptr_t car_physics_body_source_update_rva{};
    std::uintptr_t physics_backend_vtable_rva{};
    std::uintptr_t native_physics_body_vtable_rva{};
    std::uintptr_t physics_set_pose_rva{};
    std::uintptr_t physics_set_position_rva{};
    std::uintptr_t physics_set_rotation_rva{};
    std::uintptr_t physics_set_linear_rva{};
    std::uintptr_t physics_set_angular_rva{};
    std::uintptr_t physics_get_linear_rva{};
    std::uintptr_t physics_get_angular_rva{};
};

inline constexpr Profile ReferenceProfile() {
    return Profile{
        {kPhysicsInterfaceVtableRvas[0], kPhysicsInterfaceVtableRvas[1],
         kPhysicsInterfaceVtableRvas[2], kPhysicsInterfaceVtableRvas[3],
         kPhysicsInterfaceVtableRvas[4], kPhysicsInterfaceVtableRvas[5],
         kPhysicsInterfaceVtableRvas[6], kPhysicsInterfaceVtableRvas[7],
         kPhysicsInterfaceVtableRvas[8]},
        kPositionGetterRva,
        kRotationGetterRva,
        {kWrapperSlot40Rva, kWrapperSlot48Rva, kWrapperSlot58Rva,
         kWrapperSlot60Rva, kWrapperSlot68Rva, kWrapperSlot88Rva,
         kWrapperSlot90Rva, kWrapperSlot98Rva, kWrapperSlotA0Rva},
        {kDelegateSlot40Rva, kDelegateSlot48Rva, kDelegateSlot58Rva,
         kDelegateSlot60Rva, kDelegateSlot68Rva, kDelegateSlot88Rva,
         kDelegateSlot90Rva, kDelegateSlot98Rva, kDelegateSlotA0Rva},
        kVehicleSourceVtableRva,
        kVehicleSourceUpdateRva,
        kCarPhysicsBodySourceVtableRva,
        kCarPhysicsBodySourceUpdateRva,
        kPhysicsBackendVtableRva,
        kNativePhysicsBodyVtableRva,
        kPhysicsSetPoseRva,
        kPhysicsSetPositionRva,
        kPhysicsSetRotationRva,
        kPhysicsSetLinearRva,
        kPhysicsSetAngularRva,
        kPhysicsGetLinearRva,
        kPhysicsGetAngularRva,
    };
}

struct Layout {
    std::uintptr_t interface{};
    std::uintptr_t interface_vtable{};
    std::uintptr_t physics_base{};
    std::uintptr_t position_address{};
    std::uintptr_t rotation_address{};
    std::uintptr_t linear_address{};
    std::uintptr_t angular_address{};
};

struct Snapshot {
    float position[3]{};
    float rotation[4]{};
    float linear[3]{};
    float angular[3]{};
};

struct BackendLayout {
    std::uint32_t failure_stage{};
    std::uintptr_t interface{};
    std::uintptr_t interface_vtable{};
    std::uintptr_t base{};
    std::uintptr_t base_vtable{};
    std::uintptr_t wrapper_slots[9]{};
    std::uintptr_t delegate_interface{};
    std::uintptr_t delegate_interface_vtable{};
    std::uintptr_t delegate{};
    std::uintptr_t delegate_vtable{};
    std::uintptr_t delegate_slots[9]{};
    std::uintptr_t linear_source_base{};
    std::uintptr_t angular_source_base{};
    std::uintptr_t linear_source_address{};
    std::uintptr_t angular_source_address{};
    std::uintptr_t source_vtable{};
    std::uintptr_t source_update{};
    std::uintptr_t physics_velocity_interface{};
    std::uintptr_t physics_velocity_vtable{};
    std::uintptr_t physics_set_pose{};
    std::uintptr_t physics_set_position{};
    std::uintptr_t physics_set_rotation{};
    std::uintptr_t physics_set_linear{};
    std::uintptr_t physics_set_angular{};
    std::uintptr_t physics_linear_getter{};
    std::uintptr_t physics_angular_getter{};
    std::uintptr_t native_body{};
    std::uintptr_t native_body_vtable{};
    std::uintptr_t native_pose_address{};
    std::uintptr_t native_pose_commit_tail_address{};
    std::uintptr_t native_linear_address{};
    std::uintptr_t native_angular_address{};
};

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

inline bool ReadExact(int fd, std::uintptr_t address, void* output,
                      std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, cursor + done, size - done,
                                static_cast<off_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

inline bool Finite(const float* values, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index]) ||
            std::fabs(values[index]) > 1000000.0f)
            return false;
    }
    return true;
}

inline bool ReadSnapshot(int mem, const Layout& layout, Snapshot* output) {
    if (!ReadExact(mem, layout.position_address, output->position,
                   sizeof(output->position)) ||
        !ReadExact(mem, layout.rotation_address, output->rotation,
                   sizeof(output->rotation)) ||
        !ReadExact(mem, layout.linear_address, output->linear,
                   sizeof(output->linear)) ||
        !ReadExact(mem, layout.angular_address, output->angular,
                   sizeof(output->angular)) ||
        !Finite(output->position, 3) || !Finite(output->rotation, 4) ||
        !Finite(output->linear, 3) || !Finite(output->angular, 3))
        return false;
    const double norm =
        static_cast<double>(output->rotation[0]) * output->rotation[0] +
        static_cast<double>(output->rotation[1]) * output->rotation[1] +
        static_cast<double>(output->rotation[2]) * output->rotation[2] +
        static_cast<double>(output->rotation[3]) * output->rotation[3];
    return norm >= 0.25 && norm <= 2.25;
}

// Resolve the vehicle-state delegate used by CarPhysicsState's velocity
// readback. Earlier notes incorrectly inferred setter identities solely from
// matching vtable slot numbers in an unrelated PhysicsBackendBody class. This
// resolver deliberately uses neutral slot names, follows the actual +0xA0
// delegate chain, and exposes only the proven linear/angular source fields.
// It never invokes a vfunc or writes gameplay data.
inline bool ResolveBackendLayout(int mem, std::uintptr_t module_base,
                                 const Layout& layout,
                                 const Profile& profile,
                                 BackendLayout* output) {
    BackendLayout backend{};
    const auto fail = [&](std::uint32_t stage) {
        backend.failure_stage = stage;
        *output = backend;
        return false;
    };
    if (!ReadExact(mem, layout.physics_base + 0x30, &backend.interface,
                   sizeof(backend.interface)) ||
        backend.interface == 0 ||
        !ReadExact(mem, backend.interface, &backend.interface_vtable,
                   sizeof(backend.interface_vtable)))
        return fail(1);
    std::int64_t adjustment = 0;
    if (backend.interface_vtable < 0x230 ||
        !ReadExact(mem, backend.interface_vtable - 0x230, &adjustment,
                   sizeof(adjustment)))
        return fail(2);
    const auto signed_base =
        static_cast<std::intptr_t>(backend.interface) + adjustment;
    if (signed_base <= 0) return fail(3);
    backend.base = static_cast<std::uintptr_t>(signed_base);
    constexpr std::uintptr_t kSlots[] = {
        0x40, 0x48, 0x58, 0x60, 0x68, 0x88, 0x90, 0x98, 0xA0,
    };
    if (!ReadExact(mem, backend.base, &backend.base_vtable,
                   sizeof(backend.base_vtable)))
        return fail(4);
    for (std::size_t index = 0; index < std::size(kSlots); ++index) {
        if (!ReadExact(mem, backend.base_vtable + kSlots[index],
                       &backend.wrapper_slots[index],
                       sizeof(backend.wrapper_slots[index])) ||
            backend.wrapper_slots[index] !=
                module_base + profile.wrapper_rvas[index])
            return fail(5);
    }

    if (!ReadExact(mem, backend.base + 0xA0, &backend.delegate_interface,
                   sizeof(backend.delegate_interface)) ||
        backend.delegate_interface == 0 ||
        !ReadExact(mem, backend.delegate_interface,
                   &backend.delegate_interface_vtable,
                   sizeof(backend.delegate_interface_vtable)) ||
        backend.delegate_interface_vtable < 0x230)
        return fail(6);
    std::int64_t delegate_adjustment = 0;
    if (!ReadExact(mem, backend.delegate_interface_vtable - 0x230,
                   &delegate_adjustment, sizeof(delegate_adjustment)))
        return fail(7);
    const auto signed_delegate =
        static_cast<std::intptr_t>(backend.delegate_interface) +
        delegate_adjustment;
    if (signed_delegate <= 0) return fail(8);
    backend.delegate = static_cast<std::uintptr_t>(signed_delegate);
    if (!ReadExact(mem, backend.delegate, &backend.delegate_vtable,
                   sizeof(backend.delegate_vtable)))
        return fail(9);
    for (std::size_t index = 0; index < std::size(kSlots); ++index) {
        if (!ReadExact(mem, backend.delegate_vtable + kSlots[index],
                       &backend.delegate_slots[index],
                       sizeof(backend.delegate_slots[index])))
            return fail(10);
    }

    std::int64_t linear_adjustment = 0;
    std::int64_t angular_adjustment = 0;
    if (backend.delegate_vtable < 0x68 ||
        !ReadExact(mem, backend.delegate_vtable - 0x60,
                   &linear_adjustment, sizeof(linear_adjustment)) ||
        !ReadExact(mem, backend.delegate_vtable - 0x68,
                   &angular_adjustment, sizeof(angular_adjustment)))
        return fail(11);
    const auto signed_linear_source =
        static_cast<std::intptr_t>(backend.delegate) + linear_adjustment;
    const auto signed_angular_source =
        static_cast<std::intptr_t>(backend.delegate) + angular_adjustment;
    if (signed_linear_source <= 0 || signed_angular_source <= 0)
        return fail(12);
    backend.linear_source_base =
        static_cast<std::uintptr_t>(signed_linear_source);
    backend.angular_source_base =
        static_cast<std::uintptr_t>(signed_angular_source);
    if (backend.linear_source_base != backend.angular_source_base ||
        !ReadExact(mem, backend.linear_source_base, &backend.source_vtable,
                   sizeof(backend.source_vtable)))
        return fail(13);
    std::uintptr_t linear_source_offset = 0;
    std::uintptr_t angular_source_offset = 0;
    std::uintptr_t expected_source_update = 0;
    if (backend.source_vtable ==
        module_base + profile.vehicle_source_vtable_rva) {
        linear_source_offset = 0x224;
        angular_source_offset = 0x23C;
        expected_source_update =
            module_base + profile.vehicle_source_update_rva;
    } else if (backend.source_vtable ==
               module_base + profile.car_physics_body_source_vtable_rva) {
        linear_source_offset = 0x2CC;
        angular_source_offset = 0x2E4;
        expected_source_update =
            module_base + profile.car_physics_body_source_update_rva;
    } else {
        return fail(14);
    }
    if (backend.linear_source_base >
            UINTPTR_MAX - linear_source_offset - 12 ||
        backend.angular_source_base >
            UINTPTR_MAX - angular_source_offset - 12)
        return fail(15);
    backend.linear_source_address =
        backend.linear_source_base + linear_source_offset;
    backend.angular_source_address =
        backend.angular_source_base + angular_source_offset;
    if (!ReadExact(mem, backend.source_vtable + kVehicleSourceUpdateSlot,
                   &backend.source_update, sizeof(backend.source_update)) ||
        backend.source_update != expected_source_update ||
        !ReadExact(mem,
                   backend.linear_source_base +
                       kPhysicsVelocityInterfaceOffset,
                   &backend.physics_velocity_interface,
                   sizeof(backend.physics_velocity_interface)) ||
        backend.physics_velocity_interface == 0 ||
        !ReadExact(mem, backend.physics_velocity_interface,
                   &backend.physics_velocity_vtable,
                   sizeof(backend.physics_velocity_vtable)) ||
        backend.physics_velocity_vtable !=
            module_base + profile.physics_backend_vtable_rva ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable + kPhysicsSetPoseSlot,
                   &backend.physics_set_pose,
                   sizeof(backend.physics_set_pose)) ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable + kPhysicsSetPositionSlot,
                   &backend.physics_set_position,
                   sizeof(backend.physics_set_position)) ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable + kPhysicsSetRotationSlot,
                   &backend.physics_set_rotation,
                   sizeof(backend.physics_set_rotation)) ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable + kPhysicsSetLinearSlot,
                   &backend.physics_set_linear,
                   sizeof(backend.physics_set_linear)) ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable + kPhysicsSetAngularSlot,
                   &backend.physics_set_angular,
                   sizeof(backend.physics_set_angular)) ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable +
                       kPhysicsLinearGetterSlot,
                   &backend.physics_linear_getter,
                   sizeof(backend.physics_linear_getter)) ||
        !ReadExact(mem,
                   backend.physics_velocity_vtable +
                       kPhysicsAngularGetterSlot,
                   &backend.physics_angular_getter,
                   sizeof(backend.physics_angular_getter)) ||
        backend.physics_set_pose !=
            module_base + profile.physics_set_pose_rva ||
        backend.physics_set_position !=
            module_base + profile.physics_set_position_rva ||
        backend.physics_set_rotation !=
            module_base + profile.physics_set_rotation_rva ||
        backend.physics_set_linear !=
            module_base + profile.physics_set_linear_rva ||
        backend.physics_set_angular !=
            module_base + profile.physics_set_angular_rva ||
        backend.physics_linear_getter !=
            module_base + profile.physics_get_linear_rva ||
        backend.physics_angular_getter !=
            module_base + profile.physics_get_angular_rva ||
        !ReadExact(mem, backend.physics_velocity_interface + 0x90,
                   &backend.native_body, sizeof(backend.native_body)) ||
        backend.native_body == 0 ||
        backend.native_body > UINTPTR_MAX - 0x16C)
        return fail(16);
    if (!ReadExact(mem, backend.native_body, &backend.native_body_vtable,
                   sizeof(backend.native_body_vtable)) ||
        backend.native_body_vtable !=
            module_base + profile.native_physics_body_vtable_rva)
        return fail(17);
    backend.native_pose_address = backend.native_body + 0x10;
    // The worker's final 16-byte pose store spans body+0x30..+0x4F.
    // A passive 4-byte HWBP at +0x40 traps after that complete store.
    backend.native_pose_commit_tail_address = backend.native_body + 0x40;
    backend.native_linear_address = backend.native_body + 0x150;
    backend.native_angular_address = backend.native_body + 0x160;
    *output = backend;
    return true;
}

inline std::uintptr_t MatchVtable(std::uintptr_t value,
                                  std::uintptr_t base,
                                  const Profile& profile) {
    for (const std::uintptr_t rva : profile.physics_interface_vtable_rvas)
        if (value == base + rva) return rva;
    return 0;
}

inline bool ReadMaps(pid_t pid, std::vector<Mapping>* maps) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0;
        unsigned long long end = 0;
        char perms[5]{};
        char path_buffer[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %*llx %*s %*s %1023[^\n]", &begin, &end,
            perms, path_buffer);
        if (fields < 3) continue;
        Mapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), {}, ""};
        std::memcpy(mapping.perms, perms, 4);
        if (fields == 4) mapping.path = path_buffer;
        maps->push_back(mapping);
    }
    std::fclose(file);
    return !maps->empty();
}

inline bool BuildLayout(int mem, std::uintptr_t base,
                        std::uintptr_t interface,
                        std::uintptr_t interface_vtable,
                        const Profile& profile, Layout* output) {
    std::int64_t position_adjustment = 0;
    std::int64_t rotation_adjustment = 0;
    std::int64_t linear_adjustment = 0;
    std::int64_t angular_adjustment = 0;
    if (!ReadExact(mem, interface_vtable - 0xE8, &position_adjustment,
                   sizeof(position_adjustment)) ||
        !ReadExact(mem, interface_vtable - 0xF0, &rotation_adjustment,
                   sizeof(rotation_adjustment)) ||
        !ReadExact(mem, interface_vtable - 0x188, &linear_adjustment,
                   sizeof(linear_adjustment)) ||
        !ReadExact(mem, interface_vtable - 0x190, &angular_adjustment,
                   sizeof(angular_adjustment)))
        return false;
    const auto physics_base = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(interface) + linear_adjustment);
    if (physics_base == 0 ||
        physics_base != static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(interface) +
                            angular_adjustment) ||
        physics_base != static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(interface) +
                            position_adjustment) ||
        physics_base != static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(interface) +
                            rotation_adjustment))
        return false;

    std::uintptr_t nested = 0;
    std::uintptr_t nested_vtable = 0;
    std::int64_t nested_adjustment = 0;
    if (!ReadExact(mem, physics_base + 0x20, &nested, sizeof(nested)) ||
        nested == 0 ||
        !ReadExact(mem, nested, &nested_vtable, sizeof(nested_vtable)) ||
        !ReadExact(mem, nested_vtable - 0x58, &nested_adjustment,
                   sizeof(nested_adjustment)))
        return false;
    const auto nested_base = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(nested) + nested_adjustment);
    if (!ReadExact(mem, nested_base, &nested_vtable, sizeof(nested_vtable)))
        return false;
    std::uintptr_t position_vfunc = 0;
    std::uintptr_t rotation_vfunc = 0;
    std::int64_t position_storage_adjustment = 0;
    std::int64_t rotation_storage_adjustment = 0;
    if (!ReadExact(mem, nested_vtable + 0x28, &position_vfunc,
                   sizeof(position_vfunc)) ||
        !ReadExact(mem, nested_vtable + 0x30, &rotation_vfunc,
                   sizeof(rotation_vfunc)) ||
        position_vfunc != base + profile.position_getter_rva ||
        rotation_vfunc != base + profile.rotation_getter_rva ||
        !ReadExact(mem, nested_vtable - 0x40,
                   &position_storage_adjustment,
                   sizeof(position_storage_adjustment)) ||
        !ReadExact(mem, nested_vtable - 0x48,
                   &rotation_storage_adjustment,
                   sizeof(rotation_storage_adjustment)))
        return false;

    Layout layout{};
    layout.interface = interface;
    layout.interface_vtable = interface_vtable;
    layout.physics_base = physics_base;
    layout.position_address = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(nested_base) + position_storage_adjustment +
        0x8);
    layout.rotation_address = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(nested_base) + rotation_storage_adjustment +
        0x14);
    layout.linear_address = physics_base + kLinearOffset;
    layout.angular_address = physics_base + kAngularOffset;
    Snapshot snapshot{};
    if (!ReadSnapshot(mem, layout, &snapshot)) return false;
    *output = layout;
    return true;
}

inline bool Resolve(pid_t pid, int mem, std::uintptr_t base,
                    const Profile& profile, Layout* output,
                    std::uint64_t* scanned_bytes = nullptr) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    constexpr std::size_t kChunkSize = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunkSize);
    std::vector<Layout> valid;
    std::uint64_t scanned = 0;
    for (const Mapping& mapping : maps) {
        if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w') continue;
        for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
            const std::size_t size = static_cast<std::size_t>(
                std::min<std::uintptr_t>(kChunkSize, mapping.end - cursor));
            if (!ReadExact(mem, cursor, buffer.data(), size)) break;
            scanned += size;
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= size;
                 offset += sizeof(std::uintptr_t)) {
                std::uintptr_t vtable = 0;
                std::memcpy(&vtable, buffer.data() + offset, sizeof(vtable));
                if (MatchVtable(vtable, base, profile) == 0) continue;
                Layout layout{};
                if (BuildLayout(mem, base, cursor + offset, vtable, profile,
                                &layout))
                    valid.push_back(layout);
                if (valid.size() > 1) return false;
            }
            cursor += size;
        }
    }
    if (scanned_bytes) *scanned_bytes = scanned;
    if (valid.size() != 1) return false;
    *output = valid.front();
    return true;
}

inline bool ResolveBackendLayout(int mem, std::uintptr_t module_base,
                                 const Layout& layout,
                                 BackendLayout* output) {
    return ResolveBackendLayout(mem, module_base, layout, ReferenceProfile(),
                                output);
}

inline std::uintptr_t MatchVtable(std::uintptr_t value,
                                  std::uintptr_t base) {
    return MatchVtable(value, base, ReferenceProfile());
}

inline bool BuildLayout(int mem, std::uintptr_t base,
                        std::uintptr_t interface,
                        std::uintptr_t interface_vtable, Layout* output) {
    return BuildLayout(mem, base, interface, interface_vtable,
                       ReferenceProfile(), output);
}

inline bool Resolve(pid_t pid, int mem, std::uintptr_t base, Layout* output,
                    std::uint64_t* scanned_bytes = nullptr) {
    return Resolve(pid, mem, base, ReferenceProfile(), output, scanned_bytes);
}

}  // namespace a9tas::vehicle_state_v1
