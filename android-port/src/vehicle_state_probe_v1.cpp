// Read-only resolver for the player vehicle's synchronized physics state.
//
// usage: a9tas_vehicle_state_probe_v1 PID LIB_BASE_HEX VEHICLE_OWNER_HEX

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uintptr_t kPlayerPhysicsInterfaceVtableRvas[] = {
    0x7EE9B78, 0x80C8098, 0x80C9978, 0x80CAF40, 0x80E74A0,
    0x80E8DF0, 0x80EA3B8, 0x815DC08, 0x815F3E8,
};
constexpr std::size_t kOwnerScanSize = 0x3000;
constexpr std::uintptr_t kCachedLinearVelocityOffset = 0xF50;
constexpr std::uintptr_t kCachedAngularVelocityOffset = 0xF5C;

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

struct InterfaceCandidate {
    std::uintptr_t interface{};
    std::uintptr_t vtable{};
    std::uintptr_t vtable_rva{};
};

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
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

bool ParseUnsigned(const char* value, int base, std::uint64_t* output) {
    if (!value || !*value || *value == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, base);
    if (errno != 0 || end == value || *end != '\0') return false;
    *output = parsed;
    return true;
}

bool FiniteVector(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i]) || std::fabs(values[i]) > 1000000.0f)
            return false;
    }
    return true;
}

std::uintptr_t MatchVtableRva(std::uintptr_t value, std::uintptr_t base) {
    for (const std::uintptr_t rva : kPlayerPhysicsInterfaceVtableRvas) {
        if (value == base + rva) return rva;
    }
    return 0;
}

bool ReadMaps(pid_t pid, std::vector<Mapping>* maps) {
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

bool CandidateHasValidVelocity(int mem, const InterfaceCandidate& candidate,
                               std::uintptr_t* physics_base_out) {
    std::int64_t linear_adjustment = 0;
    std::int64_t angular_adjustment = 0;
    if (!ReadExact(mem, candidate.vtable - 0x188, &linear_adjustment,
                   sizeof(linear_adjustment)) ||
        !ReadExact(mem, candidate.vtable - 0x190, &angular_adjustment,
                   sizeof(angular_adjustment)))
        return false;
    const auto linear_base = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(candidate.interface) + linear_adjustment);
    const auto angular_base = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(candidate.interface) + angular_adjustment);
    if (linear_base != angular_base || linear_base == 0) return false;
    float linear[3]{};
    float angular[3]{};
    if (!ReadExact(mem, linear_base + kCachedLinearVelocityOffset, linear,
                   sizeof(linear)) ||
        !ReadExact(mem, linear_base + kCachedAngularVelocityOffset, angular,
                   sizeof(angular)) ||
        !FiniteVector(linear, 3) || !FiniteVector(angular, 3))
        return false;
    *physics_base_out = linear_base;
    return true;
}

bool ScanGlobalInterfaces(int mem, pid_t pid, std::uintptr_t base,
                          std::vector<InterfaceCandidate>* candidates,
                          std::uint64_t* scanned_bytes) {
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return false;
    constexpr std::size_t kChunkSize = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunkSize);
    for (const Mapping& mapping : maps) {
        if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w') continue;
        for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
            const std::size_t size = static_cast<std::size_t>(
                std::min<std::uintptr_t>(kChunkSize, mapping.end - cursor));
            if (!ReadExact(mem, cursor, buffer.data(), size)) break;
            *scanned_bytes += size;
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= size;
                 offset += sizeof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                const std::uintptr_t rva = MatchVtableRva(value, base);
                if (rva == 0) continue;
                const std::uintptr_t address = cursor + offset;
                bool duplicate = false;
                for (const auto& existing : *candidates)
                    duplicate = duplicate || existing.interface == address;
                if (!duplicate)
                    candidates->push_back({address, value, rva});
                if (candidates->size() > 256) return false;
            }
            cursor += size;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX VEHICLE_OWNER_HEX\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 16, &owner_value) || pid_value == 0 ||
        base_value == 0 || owner_value == 0) {
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto owner = static_cast<std::uintptr_t>(owner_value);
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem");
        return 3;
    }

    std::uintptr_t interface = 0;
    std::uintptr_t interface_offset = 0;
    std::uintptr_t expected_vtable = 0;
    std::uintptr_t matched_vtable_rva = 0;
    bool interface_is_indirect = false;
    for (std::uintptr_t offset = 0; offset < kOwnerScanSize;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t candidate = 0;
        if (!ReadExact(mem, owner + offset, &candidate, sizeof(candidate)))
            continue;
        std::uintptr_t candidate_interface = owner + offset;
        std::uintptr_t candidate_vtable = candidate;
        bool candidate_is_indirect = false;
        std::uintptr_t candidate_rva = MatchVtableRva(candidate_vtable, base);
        if (candidate_rva == 0 && candidate != 0 &&
            ReadExact(mem, candidate, &candidate_vtable,
                      sizeof(candidate_vtable))) {
            candidate_rva = MatchVtableRva(candidate_vtable, base);
            if (candidate_rva != 0) {
                candidate_interface = candidate;
                candidate_is_indirect = true;
            }
        }
        if (candidate_rva != 0) {
            if (interface != 0 && interface != candidate_interface) {
                std::fprintf(stderr,
                             "ambiguous player physics interface: 0x%" PRIxPTR
                             " and 0x%" PRIxPTR "\n",
                             interface, candidate_interface);
                close(mem);
                return 4;
            }
            interface = candidate_interface;
            interface_offset = offset;
            expected_vtable = candidate_vtable;
            matched_vtable_rva = candidate_rva;
            interface_is_indirect = candidate_is_indirect;
        }
    }
    if (interface == 0) {
        std::vector<InterfaceCandidate> global_candidates;
        std::uint64_t scanned_bytes = 0;
        if (!ScanGlobalInterfaces(mem, pid, base, &global_candidates,
                                  &scanned_bytes)) {
            std::fprintf(stderr, "global interface scan failed\n");
            close(mem);
            return 4;
        }
        std::size_t valid_count = 0;
        for (const auto& candidate : global_candidates) {
            std::uintptr_t physics_base = 0;
            const bool valid = CandidateHasValidVelocity(
                mem, candidate, &physics_base);
            std::printf(
                "GLOBAL_CANDIDATE interface=0x%" PRIxPTR
                " vtable_rva=0x%" PRIxPTR " physics_base=0x%" PRIxPTR
                " valid=%u\n",
                candidate.interface, candidate.vtable_rva, physics_base,
                valid ? 1u : 0u);
            if (!valid) continue;
            ++valid_count;
            interface = candidate.interface;
            expected_vtable = candidate.vtable;
            matched_vtable_rva = candidate.vtable_rva;
            interface_is_indirect = true;
        }
        std::printf("GLOBAL_SCAN scanned_bytes=%" PRIu64
                    " candidates=%zu valid=%zu\n",
                    scanned_bytes, global_candidates.size(), valid_count);
        if (valid_count != 1) {
            std::fprintf(stderr,
                         "expected one valid global physics interface, got %zu\n",
                         valid_count);
            close(mem);
            return 4;
        }
    }

    std::int64_t adjust_position = 0;
    std::int64_t adjust_rotation = 0;
    std::int64_t adjust_linear = 0;
    std::int64_t adjust_angular = 0;
    if (!ReadExact(mem, expected_vtable - 0xE8, &adjust_position,
                   sizeof(adjust_position)) ||
        !ReadExact(mem, expected_vtable - 0xF0, &adjust_rotation,
                   sizeof(adjust_rotation)) ||
        !ReadExact(mem, expected_vtable - 0x188, &adjust_linear,
                   sizeof(adjust_linear)) ||
        !ReadExact(mem, expected_vtable - 0x190, &adjust_angular,
                   sizeof(adjust_angular))) {
        close(mem);
        return 5;
    }

    const auto adjusted_position = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(interface) + adjust_position);
    const auto adjusted_rotation = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(interface) + adjust_rotation);
    const auto adjusted_linear = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(interface) + adjust_linear);
    const auto adjusted_angular = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(interface) + adjust_angular);

    float linear[3]{};
    float angular[3]{};
    const bool vectors_ok =
        adjusted_linear == adjusted_angular &&
        ReadExact(mem, adjusted_linear + kCachedLinearVelocityOffset, linear,
                  sizeof(linear)) &&
        ReadExact(mem, adjusted_angular + kCachedAngularVelocityOffset, angular,
                  sizeof(angular)) &&
        FiniteVector(linear, 3) && FiniteVector(angular, 3);

    std::uintptr_t nested = 0;
    std::uintptr_t nested_vtable = 0;
    std::int64_t nested_adjustment = 0;
    std::uintptr_t nested_base = 0;
    std::uintptr_t position_vfunc = 0;
    std::uintptr_t rotation_vfunc = 0;
    std::int64_t position_storage_adjustment = 0;
    std::int64_t rotation_storage_adjustment = 0;
    std::uintptr_t position_address = 0;
    std::uintptr_t rotation_address = 0;
    float position[3]{};
    float rotation[4]{};
    bool nested_ok = adjusted_position == adjusted_rotation &&
                     adjusted_position == adjusted_linear &&
                     ReadExact(mem, adjusted_position + 0x20, &nested,
                               sizeof(nested)) &&
                     nested != 0 &&
                     ReadExact(mem, nested, &nested_vtable,
                               sizeof(nested_vtable)) &&
                     nested_vtable >= base + 0x1000 &&
                     ReadExact(mem, nested_vtable - 0x58, &nested_adjustment,
                               sizeof(nested_adjustment));
    if (nested_ok) {
        nested_base = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(nested) + nested_adjustment);
        nested_ok = ReadExact(mem, nested_base, &nested_vtable,
                              sizeof(nested_vtable)) &&
                    ReadExact(mem, nested_vtable + 0x28, &position_vfunc,
                              sizeof(position_vfunc)) &&
                    ReadExact(mem, nested_vtable + 0x30, &rotation_vfunc,
                              sizeof(rotation_vfunc)) &&
                    position_vfunc == base + 0x4D10D38 &&
                    rotation_vfunc == base + 0x4D10D4C &&
                    ReadExact(mem, nested_vtable - 0x40,
                              &position_storage_adjustment,
                              sizeof(position_storage_adjustment)) &&
                    ReadExact(mem, nested_vtable - 0x48,
                              &rotation_storage_adjustment,
                              sizeof(rotation_storage_adjustment));
    }
    if (nested_ok) {
        position_address = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(nested_base) +
            position_storage_adjustment + 0x8);
        rotation_address = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(nested_base) +
            rotation_storage_adjustment + 0x14);
        nested_ok = ReadExact(mem, position_address, position,
                              sizeof(position)) &&
                    ReadExact(mem, rotation_address, rotation,
                              sizeof(rotation)) &&
                    FiniteVector(position, 3) && FiniteVector(rotation, 4);
    }

    std::printf(
        "VEHICLE_STATE_PROBE_V1 pid=%d owner=0x%" PRIxPTR
        " interface=0x%" PRIxPTR " interface_offset=0x%" PRIxPTR
        " indirect=%u vtable_rva=0x%" PRIxPTR "\n",
        static_cast<int>(pid), owner, interface, interface_offset,
        interface_is_indirect ? 1u : 0u, matched_vtable_rva);
    std::printf(
        "ADJUST position=%" PRId64 " rotation=%" PRId64
        " linear=%" PRId64 " angular=%" PRId64
        " resolved_position=0x%" PRIxPTR
        " resolved_rotation=0x%" PRIxPTR
        " resolved_linear=0x%" PRIxPTR
        " resolved_angular=0x%" PRIxPTR "\n",
        adjust_position, adjust_rotation, adjust_linear, adjust_angular,
        adjusted_position, adjusted_rotation, adjusted_linear,
        adjusted_angular);
    std::printf(
        "VELOCITY valid=%u linear=(%.9g,%.9g,%.9g) "
        "angular=(%.9g,%.9g,%.9g)\n",
        vectors_ok ? 1u : 0u, linear[0], linear[1], linear[2], angular[0],
        angular[1], angular[2]);
    std::printf(
        "POSE_DELEGATE valid=%u nested=0x%" PRIxPTR
        " nested_adjustment=%" PRId64 " nested_base=0x%" PRIxPTR
        " vtable_rva=0x%" PRIxPTR " position_vfunc_rva=0x%" PRIxPTR
        " rotation_vfunc_rva=0x%" PRIxPTR "\n",
        nested_ok ? 1u : 0u, nested, nested_adjustment, nested_base,
        nested_vtable >= base ? nested_vtable - base : 0,
        position_vfunc >= base ? position_vfunc - base : 0,
        rotation_vfunc >= base ? rotation_vfunc - base : 0);
    std::printf(
        "POSE valid=%u position_address=0x%" PRIxPTR
        " rotation_address=0x%" PRIxPTR
        " position=(%.9g,%.9g,%.9g)"
        " rotation=(%.9g,%.9g,%.9g,%.9g)\n",
        nested_ok ? 1u : 0u, position_address, rotation_address, position[0],
        position[1], position[2], rotation[0], rotation[1], rotation[2],
        rotation[3]);

    close(mem);
    return vectors_ok && nested_ok ? 0 : 6;
}
