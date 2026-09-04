// Read-only live object-graph gate driven by an offline-resolved build profile.
// It performs no ptrace, target write, hook installation, input, or game resume.

#include "vehicle_state_resolver_v1.h"

#include <cerrno>
#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <vector>
#include <unistd.h>

namespace {

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
    if (!text || !*text || !output) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *output = static_cast<std::uint64_t>(value);
    return true;
}

bool ValidSha256(const char* value) {
    if (!value || std::strlen(value) != 64) return false;
    bool nonzero = false;
    for (std::size_t index = 0; index < 64; ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

bool ParseRva(const char* text, std::uintptr_t* output) {
    std::uint64_t value = 0;
    if (!ParseUnsigned(text, 16, &value) || value == 0 ||
        value > static_cast<std::uint64_t>(UINTPTR_MAX))
        return false;
    *output = static_cast<std::uintptr_t>(value);
    return true;
}

struct GraphProfile {
    std::uintptr_t main_vtable{};
    std::uintptr_t embedded_time_source_vtable{};
    std::uintptr_t physics_context_vtable{};
    std::uintptr_t physics_implementation_vtable{};
    std::uintptr_t adjusted_setter_vtable{};
    std::uintptr_t nitro_service_vtable{};
    std::uintptr_t nitro_dispatch{};
    std::uintptr_t adjusted_brake{};
    std::uintptr_t adjusted_steering{};
    std::uintptr_t lifecycle_phase_gate{};
    std::uintptr_t lifecycle_shared_enter{};
    std::uintptr_t lifecycle_derived_enter{};
    std::vector<std::uintptr_t> lifecycle_vtables;
};

using Mapping = a9tas::vehicle_state_v1::Mapping;

bool MappingContains(const std::vector<Mapping>& maps,
                     std::uintptr_t address, std::size_t size,
                     bool require_writable) {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size) return false;
    for (const Mapping& mapping : maps) {
        if (mapping.begin <= address && address + size <= mapping.end &&
            mapping.perms[0] == 'r' &&
            (!require_writable || mapping.perms[1] == 'w'))
            return true;
    }
    return false;
}

bool ValidateMainObject(int mem, const std::vector<Mapping>& maps,
                        std::uintptr_t base, std::uintptr_t object,
                        const GraphProfile& profile,
                        std::uintptr_t* physics_context) {
    constexpr std::uintptr_t kEmbeddedTimeSourceOffset = 0xB0;
    constexpr std::uintptr_t kFixedDeltaOffset = 0x150;
    constexpr std::uintptr_t kEnabledOffset = 0x158;
    constexpr std::uintptr_t kTimeScaleManagerOffset = 0xF8;
    constexpr std::uintptr_t kPhaseSchedulerOffset = 0x178;
    constexpr std::uintptr_t kPhysicsContextOffset = 0x1A98;
    constexpr std::uintptr_t kTimeScaleValueOffset = 0x2D8;
    constexpr std::size_t kObjectSize = 0x1C50;
    if ((object & 7u) != 0 ||
        !MappingContains(maps, object, kObjectSize, true))
        return false;
    std::uintptr_t main_vptr = 0, embedded_vptr = 0;
    std::uintptr_t manager = 0, scheduler = 0, context = 0, context_vptr = 0;
    std::uint8_t enabled = 0;
    std::int64_t accumulator = 0;
    std::uint32_t scale_bits = 0;
    if (!a9tas::vehicle_state_v1::ReadExact(mem, object, &main_vptr,
                                             sizeof(main_vptr)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kEmbeddedTimeSourceOffset, &embedded_vptr,
            sizeof(embedded_vptr)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kTimeScaleManagerOffset, &manager,
            sizeof(manager)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kPhaseSchedulerOffset, &scheduler,
            sizeof(scheduler)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kEnabledOffset, &enabled, sizeof(enabled)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kFixedDeltaOffset, &accumulator,
            sizeof(accumulator)) ||
        main_vptr != base + profile.main_vtable ||
        embedded_vptr != base + profile.embedded_time_source_vtable ||
        enabled > 1 || manager == 0 || scheduler == 0 ||
        !MappingContains(maps, scheduler, sizeof(std::uintptr_t), false) ||
        !MappingContains(maps, manager + kTimeScaleValueOffset,
                         sizeof(scale_bits), false) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, manager + kTimeScaleValueOffset, &scale_bits,
            sizeof(scale_bits)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kPhysicsContextOffset, &context,
            sizeof(context)) || context == 0 ||
        !a9tas::vehicle_state_v1::ReadExact(mem, context, &context_vptr,
                                             sizeof(context_vptr)) ||
        context_vptr != base + profile.physics_context_vtable)
        return false;
    float scale = 0.0f;
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    if (!std::isfinite(scale) || scale < 0.0f || scale > 64.0f ||
        accumulator == INT64_MIN)
        return false;
    *physics_context = context;
    return true;
}

bool ValidateImplementation(int mem, const std::vector<Mapping>& maps,
                            std::uintptr_t base, std::uintptr_t object,
                            const GraphProfile& profile,
                            std::uintptr_t* nitro_service,
                            std::uintptr_t* setter_object) {
    constexpr std::uintptr_t kNitroServiceOffset = 0xCB8;
    constexpr std::uintptr_t kAdjustedSetterOffset = 0x2A80;
    constexpr std::uintptr_t kBrakeSlot = 0x2A8;
    constexpr std::uintptr_t kSteeringSlot = 0x2B0;
    constexpr std::uintptr_t kNitroDispatchSlot = 0x108;
    if ((object & 7u) != 0 ||
        !MappingContains(maps, object, kAdjustedSetterOffset + 8, true))
        return false;
    std::uintptr_t object_vptr = 0, service = 0, service_vptr = 0;
    std::uintptr_t dispatch = 0, setter_vptr = 0, brake = 0, steering = 0;
    const std::uintptr_t setter = object + kAdjustedSetterOffset;
    if (!a9tas::vehicle_state_v1::ReadExact(mem, object, &object_vptr,
                                             sizeof(object_vptr)) ||
        object_vptr != base + profile.physics_implementation_vtable ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kNitroServiceOffset, &service, sizeof(service)) ||
        service == 0 ||
        !a9tas::vehicle_state_v1::ReadExact(mem, service, &service_vptr,
                                             sizeof(service_vptr)) ||
        service_vptr != base + profile.nitro_service_vtable ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, service_vptr + kNitroDispatchSlot, &dispatch,
            sizeof(dispatch)) || dispatch != base + profile.nitro_dispatch ||
        !a9tas::vehicle_state_v1::ReadExact(mem, setter, &setter_vptr,
                                             sizeof(setter_vptr)) ||
        setter_vptr != base + profile.adjusted_setter_vtable ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, setter_vptr + kBrakeSlot, &brake, sizeof(brake)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, setter_vptr + kSteeringSlot, &steering, sizeof(steering)) ||
        brake != base + profile.adjusted_brake ||
        steering != base + profile.adjusted_steering)
        return false;
    *nitro_service = service;
    *setter_object = setter;
    return true;
}

bool ValidateLifecycle(int mem, const std::vector<Mapping>& maps,
                       std::uintptr_t base, std::uintptr_t object,
                       std::uintptr_t vptr, const GraphProfile& profile,
                       std::uintptr_t* state_address) {
    constexpr std::uintptr_t kGateSlot = 0x1D8;
    constexpr std::uintptr_t kEnterSlot = 0x1E0;
    constexpr std::uintptr_t kStateOffset = 0x2D8;
    if ((object & 7u) != 0 ||
        !MappingContains(maps, object, kStateOffset + 9, true))
        return false;
    const std::uintptr_t rva = vptr - base;
    if (!std::binary_search(profile.lifecycle_vtables.begin(),
                            profile.lifecycle_vtables.end(), rva))
        return false;
    std::uintptr_t gate = 0, enter = 0;
    std::uint32_t state = 0;
    if (!a9tas::vehicle_state_v1::ReadExact(
            mem, vptr + kGateSlot, &gate, sizeof(gate)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, vptr + kEnterSlot, &enter, sizeof(enter)) ||
        !a9tas::vehicle_state_v1::ReadExact(
            mem, object + kStateOffset, &state, sizeof(state)) ||
        gate != base + profile.lifecycle_phase_gate ||
        (enter != base + profile.lifecycle_shared_enter &&
         enter != base + profile.lifecycle_derived_enter) || state != 2)
        return false;
    *state_address = object + kStateOffset;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    constexpr int kProfileRvaCount = 42;
    constexpr int kGraphFixedRvaCount = 12;
    constexpr int kFirstProfileArgument = 4;
    if (argc < kFirstProfileArgument + kProfileRvaCount +
                   kGraphFixedRvaCount + 1) {
        std::fprintf(stderr,
                     "usage: %s PID BASE_HEX PROFILE_SHA256 "
                     "42_VEHICLE_RVAS 12_GRAPH_RVAS LIFECYCLE_COUNT "
                     "LIFECYCLE_VTABLE_RVAS\n",
                     argv[0]);
        return 2;
    }

    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) || pid_value == 0 ||
        pid_value > static_cast<std::uint64_t>(INT_MAX) ||
        !ParseUnsigned(argv[2], 16, &base_value) || base_value == 0 ||
        base_value > static_cast<std::uint64_t>(UINTPTR_MAX) ||
        !ValidSha256(argv[3])) {
        std::fprintf(stderr, "LIVE_PROFILE_GATE_FAIL stage=arguments\n");
        return 2;
    }

    a9tas::vehicle_state_v1::Profile profile{};
    int argument = kFirstProfileArgument;
    auto parse_one = [&](std::uintptr_t* value) {
        return ParseRva(argv[argument++], value);
    };
    for (auto& value : profile.physics_interface_vtable_rvas)
        if (!parse_one(&value)) return 2;
    if (!parse_one(&profile.position_getter_rva) ||
        !parse_one(&profile.rotation_getter_rva))
        return 2;
    for (auto& value : profile.wrapper_rvas)
        if (!parse_one(&value)) return 2;
    for (auto& value : profile.delegate_rvas)
        if (!parse_one(&value)) return 2;
    if (!parse_one(&profile.vehicle_source_vtable_rva) ||
        !parse_one(&profile.vehicle_source_update_rva) ||
        !parse_one(&profile.car_physics_body_source_vtable_rva) ||
        !parse_one(&profile.car_physics_body_source_update_rva) ||
        !parse_one(&profile.physics_backend_vtable_rva) ||
        !parse_one(&profile.native_physics_body_vtable_rva) ||
        !parse_one(&profile.physics_set_pose_rva) ||
        !parse_one(&profile.physics_set_position_rva) ||
        !parse_one(&profile.physics_set_rotation_rva) ||
        !parse_one(&profile.physics_set_linear_rva) ||
        !parse_one(&profile.physics_set_angular_rva) ||
        !parse_one(&profile.physics_get_linear_rva) ||
        !parse_one(&profile.physics_get_angular_rva)) {
        std::fprintf(stderr, "LIVE_PROFILE_GATE_FAIL stage=profile_parse\n");
        return 2;
    }

    GraphProfile graph{};
    if (!parse_one(&graph.main_vtable) ||
        !parse_one(&graph.embedded_time_source_vtable) ||
        !parse_one(&graph.physics_context_vtable) ||
        !parse_one(&graph.physics_implementation_vtable) ||
        !parse_one(&graph.adjusted_setter_vtable) ||
        !parse_one(&graph.nitro_service_vtable) ||
        !parse_one(&graph.nitro_dispatch) ||
        !parse_one(&graph.adjusted_brake) ||
        !parse_one(&graph.adjusted_steering) ||
        !parse_one(&graph.lifecycle_phase_gate) ||
        !parse_one(&graph.lifecycle_shared_enter) ||
        !parse_one(&graph.lifecycle_derived_enter)) {
        std::fprintf(stderr, "LIVE_PROFILE_GATE_FAIL stage=graph_parse\n");
        return 2;
    }
    std::uint64_t lifecycle_count = 0;
    if (argument >= argc ||
        !ParseUnsigned(argv[argument++], 10, &lifecycle_count) ||
        lifecycle_count == 0 || lifecycle_count > 4096 ||
        lifecycle_count != static_cast<std::uint64_t>(argc - argument)) {
        std::fprintf(stderr,
                     "LIVE_PROFILE_GATE_FAIL stage=lifecycle_count\n");
        return 2;
    }
    graph.lifecycle_vtables.reserve(
        static_cast<std::size_t>(lifecycle_count));
    while (argument < argc) {
        std::uintptr_t value = 0;
        if (!parse_one(&value)) return 2;
        graph.lifecycle_vtables.push_back(value);
    }
    if (!std::is_sorted(graph.lifecycle_vtables.begin(),
                        graph.lifecycle_vtables.end()) ||
        std::adjacent_find(graph.lifecycle_vtables.begin(),
                           graph.lifecycle_vtables.end()) !=
            graph.lifecycle_vtables.end()) {
        std::fprintf(stderr,
                     "LIVE_PROFILE_GATE_FAIL stage=lifecycle_order\n");
        return 2;
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const std::uintptr_t module_base =
        static_cast<std::uintptr_t>(base_value);
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::fprintf(stderr,
                     "LIVE_PROFILE_GATE_FAIL stage=open_mem errno=%d\n",
                     errno);
        return 3;
    }

    a9tas::vehicle_state_v1::Layout layout{};
    std::uint64_t scanned = 0;
    const bool layout_ok = a9tas::vehicle_state_v1::Resolve(
        pid, mem, module_base, profile, &layout, &scanned);
    a9tas::vehicle_state_v1::Snapshot snapshot{};
    const bool snapshot_ok = layout_ok &&
        a9tas::vehicle_state_v1::ReadSnapshot(mem, layout, &snapshot);
    a9tas::vehicle_state_v1::BackendLayout backend{};
    const bool backend_ok = layout_ok &&
        a9tas::vehicle_state_v1::ResolveBackendLayout(
            mem, module_base, layout, profile, &backend);

    float source_linear[3]{};
    float source_angular[3]{};
    float native_linear[3]{};
    float native_angular[3]{};
    float native_pose[16]{};
    const bool values_ok = backend_ok &&
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

    std::vector<Mapping> maps;
    std::vector<std::uintptr_t> main_candidates;
    std::vector<std::uintptr_t> implementation_candidates;
    std::vector<std::uintptr_t> lifecycle_candidates;
    std::uint64_t graph_scanned = 0;
    const bool maps_ok = a9tas::vehicle_state_v1::ReadMaps(pid, &maps);
    std::vector<std::uint8_t> buffer(1u << 20);
    if (maps_ok) {
        for (const Mapping& mapping : maps) {
            if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w' ||
                mapping.perms[3] != 'p' || mapping.path == "[vvar]" ||
                mapping.path == "[vdso]")
                continue;
            for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
                const std::size_t size = static_cast<std::size_t>(
                    std::min<std::uintptr_t>(buffer.size(),
                                             mapping.end - cursor));
                if (!a9tas::vehicle_state_v1::ReadExact(
                        mem, cursor, buffer.data(), size))
                    break;
                graph_scanned += size;
                for (std::size_t offset = 0;
                     offset + sizeof(std::uintptr_t) <= size;
                     offset += sizeof(std::uintptr_t)) {
                    std::uintptr_t vptr = 0;
                    std::memcpy(&vptr, buffer.data() + offset,
                                sizeof(vptr));
                    const std::uintptr_t object = cursor + offset;
                    if (vptr == module_base + graph.main_vtable) {
                        std::uintptr_t context = 0;
                        if (ValidateMainObject(mem, maps, module_base, object,
                                               graph, &context))
                            main_candidates.push_back(object);
                    }
                    if (vptr == module_base +
                                    graph.physics_implementation_vtable) {
                        std::uintptr_t service = 0, setter = 0;
                        if (ValidateImplementation(mem, maps, module_base,
                                                   object, graph, &service,
                                                   &setter))
                            implementation_candidates.push_back(object);
                    }
                    if (vptr >= module_base) {
                        const std::uintptr_t rva = vptr - module_base;
                        if (std::binary_search(graph.lifecycle_vtables.begin(),
                                               graph.lifecycle_vtables.end(),
                                               rva)) {
                            std::uintptr_t state_address = 0;
                            if (ValidateLifecycle(mem, maps, module_base,
                                                  object, vptr, graph,
                                                  &state_address))
                                lifecycle_candidates.push_back(object);
                        }
                    }
                }
                cursor += size;
            }
        }
    }
    const auto normalize = [](std::vector<std::uintptr_t>* values) {
        std::sort(values->begin(), values->end());
        values->erase(std::unique(values->begin(), values->end()),
                      values->end());
    };
    normalize(&main_candidates);
    normalize(&implementation_candidates);
    normalize(&lifecycle_candidates);
    std::uintptr_t physics_context = 0, nitro_service = 0;
    std::uintptr_t setter_object = 0, lifecycle_state = 0;
    const bool graph_unique = main_candidates.size() == 1 &&
        implementation_candidates.size() == 1 &&
        lifecycle_candidates.size() == 1;
    const bool graph_ok = graph_unique &&
        ValidateMainObject(mem, maps, module_base, main_candidates[0], graph,
                           &physics_context) &&
        ValidateImplementation(mem, maps, module_base,
                               implementation_candidates[0], graph,
                               &nitro_service, &setter_object) &&
        ValidateLifecycle(mem, maps, module_base, lifecycle_candidates[0],
                          [&] {
                              std::uintptr_t value = 0;
                              return a9tas::vehicle_state_v1::ReadExact(
                                  mem, lifecycle_candidates[0], &value,
                                  sizeof(value)) ? value : 0;
                          }(),
                          graph, &lifecycle_state);
    close(mem);

    std::printf(
        "CHANNEL_AGNOSTIC_VEHICLE_LIVE_GATE_V1 passed=%u pid=%d "
        "profile_sha256=%s base=0x%" PRIxPTR " scanned=%" PRIu64
        " graph_scanned=%" PRIu64
        " layout=%u snapshot=%u backend=%u values=%u graph=%u "
        "main_candidates=%zu implementation_candidates=%zu "
        "lifecycle_candidates=%zu failure_stage=%u "
        "game_writes=0 ptrace_calls=0 hooks=0 auto_esc=0\n",
        layout_ok && snapshot_ok && backend_ok && values_ok && graph_ok
            ? 1u : 0u,
        static_cast<int>(pid), argv[3], module_base, scanned, graph_scanned,
        layout_ok ? 1u : 0u, snapshot_ok ? 1u : 0u,
        backend_ok ? 1u : 0u, values_ok ? 1u : 0u, graph_ok ? 1u : 0u,
        main_candidates.size(), implementation_candidates.size(),
        lifecycle_candidates.size(), backend.failure_stage);
    if (layout_ok) {
        std::printf(
            "OBJECTS interface=0x%" PRIxPTR " physics=0x%" PRIxPTR
            " source=0x%" PRIxPTR " native_body=0x%" PRIxPTR "\n",
            layout.interface, layout.physics_base,
            backend.linear_source_base, backend.native_body);
    }
    if (graph_ok) {
        std::printf(
            "GRAPH main=0x%" PRIxPTR " physics_context=0x%" PRIxPTR
            " implementation=0x%" PRIxPTR " nitro_service=0x%" PRIxPTR
            " setter=0x%" PRIxPTR " lifecycle=0x%" PRIxPTR
            " lifecycle_state=0x%" PRIxPTR "\n",
            main_candidates[0], physics_context,
            implementation_candidates[0], nitro_service, setter_object,
            lifecycle_candidates[0], lifecycle_state);
    }
    if (snapshot_ok && values_ok) {
        std::printf(
            "STATE position=(%.9g,%.9g,%.9g) "
            "rotation=(%.9g,%.9g,%.9g,%.9g) "
            "linear=(%.9g,%.9g,%.9g) angular=(%.9g,%.9g,%.9g)\n",
            snapshot.position[0], snapshot.position[1], snapshot.position[2],
            snapshot.rotation[0], snapshot.rotation[1], snapshot.rotation[2],
            snapshot.rotation[3], native_linear[0], native_linear[1],
            native_linear[2], native_angular[0], native_angular[1],
            native_angular[2]);
    }
    return layout_ok && snapshot_ok && backend_ok && values_ok && graph_ok
        ? 0 : 4;
}
