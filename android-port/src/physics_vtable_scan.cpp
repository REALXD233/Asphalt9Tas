#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

constexpr std::uintptr_t kImageSize = 0xA5D8168;
constexpr std::uintptr_t kCarVisualVtables[] = {
    0x7EF6010, 0x7EF65A0, 0x7EF6DE0, 0x7EFC678,
    0x7EFCC48, 0x7EFDA80, 0x7EFE010,
};

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = pread64(fd, cursor + done, size - done,
                                  static_cast<off64_t>(address + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::vector<Mapping> ReadMaps(pid_t pid) {
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* file = std::fopen(path, "re");
    std::vector<Mapping> maps;
    if (file == nullptr) return maps;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        unsigned long long begin = 0, end = 0, offset = 0;
        char perms[5]{};
        char name[1400]{};
        const int count = std::sscanf(line,
            "%llx-%llx %4s %llx %*s %*s %1399[^\n]",
            &begin, &end, perms, &offset, name);
        if (count < 4) continue;
        Mapping map{};
        map.begin = static_cast<std::uintptr_t>(begin);
        map.end = static_cast<std::uintptr_t>(end);
        std::memcpy(map.perms, perms, sizeof(map.perms));
        if (count == 5) {
            const char* clean = name;
            while (*clean == ' ') ++clean;
            map.path = clean;
        }
        maps.push_back(std::move(map));
    }
    std::fclose(file);
    return maps;
}

bool IsMapped(const std::vector<Mapping>& maps, std::uintptr_t address,
              std::size_t size, bool require_read = true) {
    if (address == 0 || size == 0 || address + size < address) return false;
    for (const auto& map : maps) {
        if (address >= map.begin && address + size <= map.end) {
            return !require_read || map.perms[0] == 'r';
        }
    }
    return false;
}

const char* MapName(const std::vector<Mapping>& maps, std::uintptr_t address) {
    for (const auto& map : maps) {
        if (address >= map.begin && address < map.end) {
            return map.path.empty() ? "[anonymous]" : map.path.c_str();
        }
    }
    return "[unmapped]";
}

void PrintCodePointer(const char* label, std::uintptr_t value,
                      std::uintptr_t base) {
    if (value >= base && value < base + kImageSize) {
        std::printf(" %s=0x%" PRIxPTR "(lib+0x%" PRIxPTR ")",
                    label, value, value - base);
    } else {
        std::printf(" %s=0x%" PRIxPTR, label, value);
    }
}

void PrintVec3(int fd, const std::vector<Mapping>& maps, const char* label,
               std::uintptr_t address) {
    float values[3]{};
    if (IsMapped(maps, address, sizeof(values)) &&
        ReadExact(fd, address, values, sizeof(values))) {
        std::printf("  %s address=0x%" PRIxPTR " value=(%.9g,%.9g,%.9g)\n",
                    label, address, values[0], values[1], values[2]);
    } else {
        std::printf("  %s address=0x%" PRIxPTR " <unreadable>\n",
                    label, address);
    }
}

void PrintQuat(int fd, const std::vector<Mapping>& maps, const char* label,
               std::uintptr_t address) {
    float values[4]{};
    if (IsMapped(maps, address, sizeof(values)) &&
        ReadExact(fd, address, values, sizeof(values))) {
        std::printf("  %s address=0x%" PRIxPTR
                    " value=(%.9g,%.9g,%.9g,%.9g)\n",
                    label, address, values[0], values[1], values[2], values[3]);
    } else {
        std::printf("  %s address=0x%" PRIxPTR " <unreadable>\n",
                    label, address);
    }
}

void DescribeNestedGetter(int fd, const std::vector<Mapping>& maps,
                          const char* label, std::uintptr_t iface,
                          std::uintptr_t iface_vtable,
                          std::uintptr_t first_meta_offset,
                          std::uintptr_t nested_slot,
                          std::uintptr_t getter_meta_offset,
                          std::uintptr_t storage_offset, bool quaternion,
                          std::uintptr_t base) {
    std::int64_t first_adjustment = 0;
    if (!ReadExact(fd, iface_vtable - first_meta_offset, &first_adjustment,
                   sizeof(first_adjustment))) return;
    const auto owner = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(iface) + first_adjustment);
    std::uintptr_t nested_raw = 0;
    if (!ReadExact(fd, owner + 0x20, &nested_raw, sizeof(nested_raw)) ||
        !IsMapped(maps, nested_raw, sizeof(std::uintptr_t))) return;
    std::uintptr_t nested_raw_vtable = 0;
    if (!ReadExact(fd, nested_raw, &nested_raw_vtable,
                   sizeof(nested_raw_vtable)) ||
        !IsMapped(maps, nested_raw_vtable - 0x58, sizeof(std::int64_t))) return;
    std::int64_t nested_adjustment = 0;
    if (!ReadExact(fd, nested_raw_vtable - 0x58, &nested_adjustment,
                   sizeof(nested_adjustment))) return;
    const auto nested_iface = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(nested_raw) + nested_adjustment);
    std::uintptr_t nested_vtable = 0, target = 0;
    if (!ReadExact(fd, nested_iface, &nested_vtable, sizeof(nested_vtable)) ||
        !ReadExact(fd, nested_vtable + nested_slot, &target, sizeof(target))) return;
    std::printf("  %s owner_adjustment=%" PRId64
                " owner=0x%" PRIxPTR " nested_raw=0x%" PRIxPTR
                " nested_adjustment=%" PRId64 " nested_iface=0x%" PRIxPTR,
                label, first_adjustment, owner, nested_raw,
                nested_adjustment, nested_iface);
    PrintCodePointer("nested_vtable", nested_vtable, base);
    PrintCodePointer("target", target, base);
    std::printf("\n");
    std::int64_t storage_adjustment = 0;
    if (ReadExact(fd, nested_vtable - getter_meta_offset,
                  &storage_adjustment, sizeof(storage_adjustment))) {
        const auto storage = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(nested_iface) + storage_adjustment);
        if (quaternion) {
            PrintQuat(fd, maps, label, storage + storage_offset);
        } else {
            PrintVec3(fd, maps, label, storage + storage_offset);
        }
    }
}

void DescribeCandidate(int fd, const std::vector<Mapping>& maps,
                       std::uintptr_t object, std::uintptr_t base,
                       std::uintptr_t top_vtable) {
    std::uintptr_t raw = 0;
    std::uintptr_t sync = 0;
    std::uintptr_t history = 0;
    const bool raw_ok = ReadExact(fd, object + 0x28, &raw, sizeof(raw));
    ReadExact(fd, object + 0x250, &sync, sizeof(sync));
    ReadExact(fd, object + 0x260, &history, sizeof(history));

    std::printf("CANDIDATE object=0x%" PRIxPTR
                " top_vtable=lib+0x%" PRIxPTR " map=%s",
                object, top_vtable - base, MapName(maps, object));
    PrintCodePointer("raw28", raw, base);
    PrintCodePointer("sync250", sync, base);
    PrintCodePointer("history260", history, base);
    std::printf("\n");

    if (!raw_ok || !IsMapped(maps, raw, sizeof(std::uintptr_t))) {
        std::printf("  interface: raw pointer is unreadable\n");
        return;
    }
    std::uintptr_t raw_vtable = 0;
    if (!ReadExact(fd, raw, &raw_vtable, sizeof(raw_vtable)) ||
        !IsMapped(maps, raw_vtable - 0x30, sizeof(std::int64_t))) {
        std::printf("  interface: raw vtable/metadata is unreadable\n");
        return;
    }

    std::int64_t adjustment = 0;
    if (!ReadExact(fd, raw_vtable - 0x30, &adjustment, sizeof(adjustment))) {
        std::printf("  interface: adjustment read failed\n");
        return;
    }
    const std::uintptr_t iface = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(raw) + adjustment);
    std::uintptr_t iface_vtable = 0;
    if (!IsMapped(maps, iface, sizeof(iface_vtable)) ||
        !ReadExact(fd, iface, &iface_vtable, sizeof(iface_vtable))) {
        std::printf("  interface: adjusted object unreadable raw_vtable=0x%" PRIxPTR
                    " adjustment=%" PRId64 " iface=0x%" PRIxPTR "\n",
                    raw_vtable, adjustment, iface);
        return;
    }

    std::printf("  interface raw=0x%" PRIxPTR, raw);
    PrintCodePointer("raw_vtable", raw_vtable, base);
    std::printf(" adjustment=%" PRId64 " iface=0x%" PRIxPTR,
                adjustment, iface);
    PrintCodePointer("iface_vtable", iface_vtable, base);
    std::printf("\n");

    struct Slot { const char* name; std::uintptr_t offset; };
    constexpr Slot slots[] = {
        {"position_B0", 0xB0},
        {"rotation_B8", 0xB8},
        {"linear_velocity_150", 0x150},
        {"angular_velocity_158", 0x158},
        {"state_68", 0x68},
        {"flag_190", 0x190},
        {"aux_248", 0x248},
    };
    std::printf("  slots");
    for (const auto& slot : slots) {
        std::uintptr_t target = 0;
        if (IsMapped(maps, iface_vtable + slot.offset, sizeof(target)) &&
            ReadExact(fd, iface_vtable + slot.offset, &target, sizeof(target))) {
            PrintCodePointer(slot.name, target, base);
        } else {
            std::printf(" %s=<unreadable>", slot.name);
        }
    }
    std::printf("\n");

    std::int64_t linear_adjustment = 0, angular_adjustment = 0;
    if (ReadExact(fd, iface_vtable - 0x188, &linear_adjustment,
                  sizeof(linear_adjustment))) {
        const auto state = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(iface) + linear_adjustment);
        PrintVec3(fd, maps, "linear_velocity", state + 0xF50);
    }
    if (ReadExact(fd, iface_vtable - 0x190, &angular_adjustment,
                  sizeof(angular_adjustment))) {
        const auto state = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(iface) + angular_adjustment);
        PrintVec3(fd, maps, "angular_velocity", state + 0xF5C);
    }
    DescribeNestedGetter(fd, maps, "position_chain", iface, iface_vtable,
                         0xE8, 0x28, 0x40, 0x8, false, base);
    DescribeNestedGetter(fd, maps, "rotation_chain", iface, iface_vtable,
                         0xF0, 0x30, 0x48, 0x14, true, base);

    // CarPhysicsState owner for this runtime vtable uses the same -0x188
    // top adjustment observed by the cached linear-velocity getter.
    std::int64_t state_adjustment = 0;
    if (ReadExact(fd, iface_vtable - 0x188, &state_adjustment,
                  sizeof(state_adjustment))) {
        const auto state = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(iface) + state_adjustment);
        std::uintptr_t body_raw = 0;
        if (ReadExact(fd, state + 0x30, &body_raw, sizeof(body_raw)) &&
            IsMapped(maps, body_raw, sizeof(std::uintptr_t))) {
            std::uintptr_t body_raw_vtable = 0;
            ReadExact(fd, body_raw, &body_raw_vtable, sizeof(body_raw_vtable));
            std::int64_t body_adjustment = 0;
            if (IsMapped(maps, body_raw_vtable - 0x30,
                         sizeof(body_adjustment)) &&
                ReadExact(fd, body_raw_vtable - 0x30, &body_adjustment,
                          sizeof(body_adjustment))) {
                const auto body_iface = static_cast<std::uintptr_t>(
                    static_cast<std::intptr_t>(body_raw) + body_adjustment);
                std::uintptr_t body_vtable = 0;
                ReadExact(fd, body_iface, &body_vtable, sizeof(body_vtable));
                std::printf("  rigidbody state=0x%" PRIxPTR
                            " raw=0x%" PRIxPTR " adjustment=%" PRId64
                            " iface=0x%" PRIxPTR,
                            state, body_raw, body_adjustment, body_iface);
                PrintCodePointer("vtable", body_vtable, base);
                constexpr Slot body_slots[] = {
                    {"linear_get_40", 0x40},
                    {"angular_get_48", 0x48},
                    {"delegate_slot_460", 0x460},
                    {"delegate_slot_478", 0x478},
                };
                for (const auto& slot : body_slots) {
                    std::uintptr_t target = 0;
                    if (ReadExact(fd, body_vtable + slot.offset, &target,
                                  sizeof(target))) {
                        PrintCodePointer(slot.name, target, base);
                    }
                }
                std::printf("\n");

                // Concrete rigid-body implementation is delegated through
                // body_iface+0xA0. Velocity thunks adjust that delegate using
                // vtable metadata at -0x230 before invoking slots +0x40/+0x48.
                std::uintptr_t delegate_raw = 0;
                if (ReadExact(fd, body_iface + 0xA0, &delegate_raw,
                              sizeof(delegate_raw)) &&
                    IsMapped(maps, delegate_raw, sizeof(std::uintptr_t))) {
                    std::uintptr_t delegate_raw_vtable = 0;
                    ReadExact(fd, delegate_raw, &delegate_raw_vtable,
                              sizeof(delegate_raw_vtable));
                    std::int64_t delegate_adjustment = 0;
                    if (IsMapped(maps, delegate_raw_vtable - 0x230,
                                 sizeof(delegate_adjustment)) &&
                        ReadExact(fd, delegate_raw_vtable - 0x230,
                                  &delegate_adjustment,
                                  sizeof(delegate_adjustment))) {
                        const auto delegate_iface = static_cast<std::uintptr_t>(
                            static_cast<std::intptr_t>(delegate_raw) +
                            delegate_adjustment);
                        std::uintptr_t delegate_vtable = 0;
                        ReadExact(fd, delegate_iface, &delegate_vtable,
                                  sizeof(delegate_vtable));
                        std::printf("  rigidbody_delegate raw=0x%" PRIxPTR
                                    " adjustment=%" PRId64
                                    " iface=0x%" PRIxPTR,
                                    delegate_raw, delegate_adjustment,
                                    delegate_iface);
                        PrintCodePointer("vtable", delegate_vtable, base);
                        constexpr Slot delegate_slots[] = {
                            {"linear_get_40", 0x40},
                            {"angular_get_48", 0x48},
                            {"slot_468", 0x468},
                            {"slot_480", 0x480},
                        };
                        for (const auto& slot : delegate_slots) {
                            std::uintptr_t target = 0;
                            if (ReadExact(fd, delegate_vtable + slot.offset,
                                          &target, sizeof(target))) {
                                PrintCodePointer(slot.name, target, base);
                            }
                        }
                        std::printf("\n");

                        // The concrete getters at slots +0x40/+0x48 use their
                        // own virtual-base metadata before reading the actual
                        // synchronized velocity storage. Report both adjustments
                        // and the resulting fields so CarPhysicsState frame caches
                        // cannot be mistaken for this deeper representation.
                        std::int64_t linear_storage_adjustment = 0;
                        if (ReadExact(fd, delegate_vtable - 0x60,
                                      &linear_storage_adjustment,
                                      sizeof(linear_storage_adjustment))) {
                            const auto storage = static_cast<std::uintptr_t>(
                                static_cast<std::intptr_t>(delegate_iface) +
                                linear_storage_adjustment);
                            std::printf("  rigidbody_storage linear_adjustment=%" PRId64
                                        " storage=0x%" PRIxPTR "\n",
                                        linear_storage_adjustment, storage);
                            PrintVec3(fd, maps, "synced_body_linear_velocity",
                                      storage + 0x2CC);
                        }
                        std::int64_t angular_storage_adjustment = 0;
                        if (ReadExact(fd, delegate_vtable - 0x68,
                                      &angular_storage_adjustment,
                                      sizeof(angular_storage_adjustment))) {
                            const auto storage = static_cast<std::uintptr_t>(
                                static_cast<std::intptr_t>(delegate_iface) +
                                angular_storage_adjustment);
                            std::printf("  rigidbody_storage angular_adjustment=%" PRId64
                                        " storage=0x%" PRIxPTR "\n",
                                        angular_storage_adjustment, storage);
                            PrintVec3(fd, maps, "synced_body_angular_velocity",
                                      storage + 0x2E4);

                            std::uintptr_t simulation_raw = 0;
                            if (ReadExact(fd, storage + 0x18, &simulation_raw,
                                          sizeof(simulation_raw)) &&
                                IsMapped(maps, simulation_raw,
                                         sizeof(std::uintptr_t))) {
                                std::uintptr_t simulation_vtable = 0;
                                ReadExact(fd, simulation_raw,
                                          &simulation_vtable,
                                          sizeof(simulation_vtable));
                                std::printf("  simulation_source raw=0x%" PRIxPTR,
                                            simulation_raw);
                                PrintCodePointer("vtable", simulation_vtable,
                                                 base);
                                constexpr Slot source_slots[] = {
                                    {"linear_source_A8", 0xA8},
                                    {"angular_source_B0", 0xB0},
                                };
                                for (const auto& slot : source_slots) {
                                    std::uintptr_t target = 0;
                                    if (ReadExact(fd,
                                                  simulation_vtable + slot.offset,
                                                  &target, sizeof(target))) {
                                        PrintCodePointer(slot.name, target, base);
                                    }
                                }
                                std::printf("\n");

                                std::uintptr_t backend_body = 0;
                                if (ReadExact(fd, simulation_raw + 0x90,
                                              &backend_body,
                                              sizeof(backend_body)) &&
                                    IsMapped(maps, backend_body, 0x180)) {
                                    std::printf("  backend_body address=0x%" PRIxPTR
                                                "\n", backend_body);
                                    PrintVec3(fd, maps,
                                              "backend_body_vec3_40",
                                              backend_body + 0x40);
                                    PrintVec3(fd, maps,
                                              "backend_body_linear_velocity",
                                              backend_body + 0x150);
                                    PrintVec3(fd, maps,
                                              "backend_body_angular_velocity",
                                              backend_body + 0x160);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PID LIB_BASE_HEX\n", argv[0]);
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    const std::uintptr_t base = static_cast<std::uintptr_t>(
        std::strtoull(argv[2], nullptr, 16));
    const auto maps = ReadMaps(pid);
    if (maps.empty()) {
        std::fprintf(stderr, "cannot read maps for pid %d\n", pid);
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    const int fd = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "open %s failed: %s\n", mem_path,
                     std::strerror(errno));
        return 4;
    }

    std::vector<std::uintptr_t> targets;
    for (const auto offset : kCarVisualVtables) targets.push_back(base + offset);
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::uint8_t> buffer(kChunk + sizeof(std::uintptr_t));
    std::size_t hits = 0;
    std::size_t scanned_maps = 0;
    std::uint64_t scanned_bytes = 0;

    for (const auto& map : maps) {
        // CarVisualComponent instances live in writable process memory. Skip
        // executable/file read-only regions and kernel pseudo mappings.
        if (map.perms[0] != 'r' || map.perms[1] != 'w') continue;
        if (map.path == "[vvar]" || map.path == "[vdso]") continue;
        ++scanned_maps;
        for (std::uintptr_t cursor = map.begin; cursor < map.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(kChunk, map.end - cursor));
            const ssize_t n = pread64(fd, buffer.data(), want,
                                      static_cast<off64_t>(cursor));
            if (n <= 0) {
                cursor += want;
                continue;
            }
            scanned_bytes += static_cast<std::uint64_t>(n);
            const std::size_t got = static_cast<std::size_t>(n);
            for (std::size_t i = 0; i + sizeof(std::uintptr_t) <= got;
                 i += alignof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                std::memcpy(&value, buffer.data() + i, sizeof(value));
                for (const auto target : targets) {
                    if (value == target) {
                        DescribeCandidate(fd, maps, cursor + i, base, value);
                        ++hits;
                        break;
                    }
                }
            }
            cursor += got;
        }
    }
    close(fd);
    std::printf("SUMMARY pid=%d base=0x%" PRIxPTR
                " maps=%zu bytes=%" PRIu64 " candidates=%zu\n",
                pid, base, scanned_maps, scanned_bytes, hits);
    return hits == 0 ? 5 : 0;
}
