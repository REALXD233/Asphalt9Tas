#pragma once

// Hash-pinned, read-only resolver for the passive A9PHY2 getter payload.
// It reads only payload-owned locator words and validates their mapped ranges.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fc1_payload_elf_resolver_v1.h"
#include "physics_interval_getter_payload_v2.h"

namespace a9tas::physics_interval_getter_elf_v2 {

inline constexpr char kPayloadBasename[] =
    "liba9tas_physics_interval_getter_v2_passive.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0xf4, 0x9b, 0x78, 0xef, 0xf5, 0x1b, 0x60, 0x64,
    0x36, 0xf2, 0x91, 0xc8, 0x37, 0xab, 0x50, 0xc2,
    0x97, 0x28, 0x6a, 0xe3, 0xf4, 0x3a, 0x10, 0x35,
    0x2d, 0xc2, 0xaa, 0x77, 0xca, 0x8a, 0x62, 0xdf,
};

inline constexpr std::uintptr_t kWrapperLocatorRva = 0x51E0;
inline constexpr std::uintptr_t kControlLocatorRva = 0x51E8;
inline constexpr std::uintptr_t kEvidenceLocatorRva = 0x51F0;
inline constexpr std::uintptr_t kIntervalsLocatorRva = 0x51F8;
inline constexpr std::uintptr_t kEventsLocatorRva = 0x5200;
inline constexpr std::uintptr_t kShadowLocatorRva = 0x5208;
inline constexpr std::uintptr_t kContinueLocatorRva = 0x5210;
inline constexpr std::uintptr_t kControlSizeRva = 0x5218;
inline constexpr std::uintptr_t kEvidenceSizeRva = 0x5220;
inline constexpr std::uintptr_t kEventSizeRva = 0x5228;

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uint64_t offset{};
    char perms[5]{};
    std::string path;
};

struct Layout {
    std::uintptr_t load_bias{};
    std::uintptr_t wrapper{};
    std::uintptr_t control{};
    std::uintptr_t evidence{};
    std::uintptr_t intervals{};
    std::uintptr_t events{};
    std::uintptr_t shadow{};
    std::uintptr_t continuation{};
    std::uint8_t file_sha256[32]{};
    char mapped_path[1024]{};
};

namespace detail {

using a9tas::fc1_payload_elf_v1::detail::HashFile;
using a9tas::fc1_payload_elf_v1::detail::ReadAt;

inline bool EndsWith(const std::string& path) {
    const std::size_t length = sizeof(kPayloadBasename) - 1;
    if (path.size() < length) return false;
    const std::size_t offset = path.size() - length;
    return path.compare(offset, length, kPayloadBasename) == 0 &&
           (offset == 0 || path[offset - 1] == '/');
}

inline bool ReadMappings(pid_t pid, std::vector<Mapping>* output,
                         std::string* selected_path,
                         std::uintptr_t* load_bias) {
    char maps_path[64]{};
    std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                  static_cast<int>(pid));
    FILE* file = std::fopen(maps_path, "re");
    if (!file) return false;
    std::vector<Mapping> maps;
    char line[2048]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0, offset = 0;
        char perms[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &begin, &end,
            perms, &offset, path);
        if (fields < 4 || begin >= end) continue;
        std::string clean = fields == 5 ? std::string(path) : std::string();
        while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
        Mapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), offset, {}, clean};
        std::memcpy(mapping.perms, perms, 4);
        maps.push_back(std::move(mapping));
    }
    std::fclose(file);
    std::string selected;
    std::uintptr_t bias = UINTPTR_MAX;
    std::uint32_t selected_mappings = 0;
    for (const auto& mapping : maps) {
        if (!EndsWith(mapping.path)) continue;
        if (mapping.path.find(" (deleted)") != std::string::npos) return false;
        if (selected.empty()) selected = mapping.path;
        if (mapping.path != selected) return false;
        if (mapping.offset == 0) bias = std::min(bias, mapping.begin);
        ++selected_mappings;
    }
    if (selected.empty() || selected_mappings < 2 || bias == UINTPTR_MAX ||
        selected.size() >= 1024)
        return false;
    *output = std::move(maps);
    *selected_path = selected;
    *load_bias = bias;
    return true;
}

inline const Mapping* At(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
    if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
    const std::uintptr_t end = address + size;
    for (const auto& mapping : maps)
        if (address >= mapping.begin && end <= mapping.end) return &mapping;
    return nullptr;
}

inline bool WritableRange(const std::vector<Mapping>& maps,
                          std::uintptr_t address, std::size_t size) {
    if (size == 0 || address > UINTPTR_MAX - size) return false;
    const std::uintptr_t end = address + size;
    std::uintptr_t cursor = address;
    while (cursor < end) {
        const Mapping* mapping = At(maps, cursor, 1);
        if (!mapping || mapping->perms[0] != 'r' ||
            mapping->perms[1] != 'w')
            return false;
        cursor = std::min(end, mapping->end);
    }
    return true;
}

inline bool Add(std::uintptr_t base, std::uintptr_t rva,
                std::uintptr_t* output) {
    if (base > UINTPTR_MAX - rva) return false;
    *output = base + rva;
    return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output) {
    using namespace a9tas::physics_interval_payload_v2;
    if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
    std::vector<Mapping> maps;
    std::string path;
    std::uintptr_t bias = 0;
    if (!detail::ReadMappings(pid, &maps, &path, &bias)) return false;
    const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) return false;
    struct stat info {};
    std::uint8_t hash[32]{};
    const bool hash_ok =
        fstat(file, &info) == 0 && info.st_size >= 4096 &&
        info.st_size <= 8 * 1024 * 1024 && detail::HashFile(file, hash) &&
        std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0;
    close(file);
    if (!hash_ok) return false;

    const std::uintptr_t rvas[] = {
        kWrapperLocatorRva, kControlLocatorRva, kEvidenceLocatorRva,
        kIntervalsLocatorRva, kEventsLocatorRva, kShadowLocatorRva,
        kContinueLocatorRva, kControlSizeRva, kEvidenceSizeRva,
        kEventSizeRva,
    };
    std::uintptr_t locators[10]{};
    for (std::size_t index = 0; index < 10; ++index) {
        if (!detail::Add(bias, rvas[index], &locators[index])) return false;
        const Mapping* mapping = detail::At(maps, locators[index], 8);
        if (!mapping || mapping->path != path || mapping->perms[0] != 'r')
            return false;
    }
    std::uintptr_t values[7]{};
    std::uint64_t sizes[3]{};
    if (!detail::ReadAt(process_mem, locators[0], values, sizeof(values)) ||
        !detail::ReadAt(process_mem, locators[7], sizes, sizeof(sizes)) ||
        sizes[0] != sizeof(Control) || sizes[1] != sizeof(Evidence) ||
        sizes[2] != sizeof(Event))
        return false;

    const Mapping* wrapper_map = detail::At(maps, values[0], 16);
    if (!wrapper_map || wrapper_map->path != path ||
        wrapper_map->perms[0] != 'r' || wrapper_map->perms[1] == 'w')
        return false;
    if (!detail::WritableRange(maps, values[1], sizeof(Control)) ||
        !detail::WritableRange(maps, values[2], sizeof(Evidence)) ||
        !detail::WritableRange(maps, values[3], kCapacity * sizeof(std::uint32_t)) ||
        !detail::WritableRange(maps, values[4], kCapacity * sizeof(Event)) ||
        !detail::WritableRange(maps, values[5], 16 * sizeof(std::uintptr_t)) ||
        !detail::WritableRange(maps, values[6], sizeof(std::uintptr_t)))
        return false;

    Control control{};
    Evidence evidence{};
    if (!detail::ReadAt(process_mem, values[1], &control, sizeof(control)) ||
        !detail::ReadAt(process_mem, values[2], &evidence, sizeof(evidence)) ||
        std::memcmp(control.magic, kControlMagic, 8) != 0 ||
        control.version != kVersion || control.size != sizeof(Control) ||
        std::memcmp(evidence.magic, kEvidenceMagic, 8) != 0 ||
        evidence.version != kVersion || evidence.size != sizeof(Evidence))
        return false;

    Layout layout{};
    layout.load_bias = bias;
    layout.wrapper = values[0];
    layout.control = values[1];
    layout.evidence = values[2];
    layout.intervals = values[3];
    layout.events = values[4];
    layout.shadow = values[5];
    layout.continuation = values[6];
    std::memcpy(layout.file_sha256, hash, sizeof(hash));
    std::snprintf(layout.mapped_path, sizeof(layout.mapped_path), "%s",
                  path.c_str());
    *output = layout;
    return true;
}

}  // namespace a9tas::physics_interval_getter_elf_v2
