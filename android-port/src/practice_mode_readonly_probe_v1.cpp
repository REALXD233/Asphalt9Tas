// Read-only discovery probe for the dedicated A9 CN PracticeRace object.
//
// This program is deliberately read-only.  It locates the exact, build-
// profiled PracticeRace lifecycle object and emits observations for the APK's
// positive/negative practice-mode gate.  It never ptraces or writes the target
// process.
//
// usage:
//   a9tas_practice_mode_readonly_probe_v1 PID LIB_BASE_HEX \
//       VPTR0_RVA_HEX VPTR588_RVA_HEX VPTR6C0_RVA_HEX \
//       VPTR718_RVA_HEX VPTR748_RVA_HEX [MAX_CANDIDATES]
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::size_t kObjectExtent = 0x750;
constexpr std::size_t kVptr588Offset = 0x588;
constexpr std::size_t kVptr6c0Offset = 0x6c0;
constexpr std::size_t kVptr718Offset = 0x718;
constexpr std::size_t kVptr748Offset = 0x748;
constexpr std::size_t kChunkSize = 1u << 20;
constexpr std::size_t kOverlap = sizeof(std::uintptr_t) - 1;
constexpr unsigned kDefaultMaxCandidates = 32;
constexpr unsigned kMaximumMaxCandidates = 256;

struct Mapping {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    char perms[5]{};
    std::string path;
};

struct CandidateSnapshot {
    std::uintptr_t address{};
    std::uintptr_t vptr0{};
    std::uintptr_t vptr588{};
    std::uintptr_t vptr6c0{};
    std::uintptr_t vptr718{};
    std::uintptr_t vptr748{};
    std::uint64_t object_hash{};
    std::string mapping_path;
};

bool AddNoOverflow(std::uintptr_t left, std::uintptr_t right,
                   std::uintptr_t* result) {
    if (left > std::numeric_limits<std::uintptr_t>::max() - right) return false;
    *result = left + right;
    return true;
}

bool ParseUnsigned(const char* text, int base, std::uint64_t* value) {
    if (!text || !*text || !value || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ReadMaps(pid_t pid, std::vector<Mapping>* maps) {
    char maps_path[64]{};
    std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                  static_cast<int>(pid));
    FILE* file = std::fopen(maps_path, "re");
    if (!file) return false;

    char line[4096]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0;
        unsigned long long end = 0;
        char perms[5]{};
        char path_buffer[3072]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %*llx %*s %*s %3071[^\n]", &begin, &end,
            perms, path_buffer);
        if (fields < 3 || begin >= end) continue;
        Mapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), {}, {}};
        std::memcpy(mapping.perms, perms, 4);
        if (fields == 4) mapping.path = path_buffer;
        while (!mapping.path.empty() && mapping.path.front() == ' ') {
            mapping.path.erase(0, 1);
        }
        maps->push_back(std::move(mapping));
    }
    std::fclose(file);
    return !maps->empty();
}

bool IsPrivateWritableObjectMapping(const Mapping& mapping) {
    if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w' ||
        mapping.perms[3] != 'p') {
        return false;
    }
    if (mapping.perms[2] == 'x') return false;
    return mapping.path != "[vvar]" && mapping.path != "[vdso]";
}

bool ReadExact(int fd, std::uintptr_t address, void* output, std::size_t size) {
    auto* cursor = static_cast<std::uint8_t*>(output);
    std::size_t completed = 0;
    while (completed < size) {
        const ssize_t count = pread(
            fd, cursor + completed, size - completed,
            static_cast<off_t>(address + completed));
        if (count <= 0) return false;
        completed += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint64_t Fnv1a64(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename T>
T LoadUnaligned(const std::uint8_t* bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

bool MatchesObject(const std::uint8_t* object, std::uintptr_t vptr0,
                   std::uintptr_t vptr588, std::uintptr_t vptr6c0,
                   std::uintptr_t vptr718, std::uintptr_t vptr748) {
    return LoadUnaligned<std::uintptr_t>(object, 0) == vptr0 &&
           LoadUnaligned<std::uintptr_t>(object, kVptr588Offset) == vptr588 &&
           LoadUnaligned<std::uintptr_t>(object, kVptr6c0Offset) == vptr6c0 &&
           LoadUnaligned<std::uintptr_t>(object, kVptr718Offset) == vptr718 &&
           LoadUnaligned<std::uintptr_t>(object, kVptr748Offset) == vptr748;
}

int SelfTest() {
    std::vector<std::uint8_t> object(kObjectExtent, 0x5a);
    constexpr std::uintptr_t kVptr0 = 0x1111222233334444ULL;
    constexpr std::uintptr_t kVptr588 = 0x5555666677778888ULL;
    constexpr std::uintptr_t kVptr6c0 = 0x9999aaaabbbbccccULL;
    constexpr std::uintptr_t kVptr718 = 0x123456789abcdef0ULL;
    constexpr std::uintptr_t kVptr748 = 0xfedcba9876543210ULL;
    std::memcpy(object.data(), &kVptr0, sizeof(kVptr0));
    std::memcpy(object.data() + kVptr588Offset, &kVptr588, sizeof(kVptr588));
    std::memcpy(object.data() + kVptr6c0Offset, &kVptr6c0, sizeof(kVptr6c0));
    std::memcpy(object.data() + kVptr718Offset, &kVptr718, sizeof(kVptr718));
    std::memcpy(object.data() + kVptr748Offset, &kVptr748, sizeof(kVptr748));
    const bool positive = MatchesObject(object.data(), kVptr0, kVptr588,
                                        kVptr6c0, kVptr718, kVptr748);
    object[kVptr718Offset] ^= 1;
    const bool negative = !MatchesObject(object.data(), kVptr0, kVptr588,
                                         kVptr6c0, kVptr718, kVptr748);
    const bool passed = positive && negative;
    std::printf("A9PRACTICE_PROBE_SELFTEST_V1 passed=%u\n", passed ? 1u : 0u);
    return passed ? 0 : 1;
}

int FindPointerReferences(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: %s --find-pointer PID VALUE_HEX lib|rw MAX_RESULTS\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t pointer_value = 0;
    std::uint64_t limit_value = 0;
    if (!ParseUnsigned(argv[2], 10, &pid_value) || pid_value == 0 ||
        pid_value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
        !ParseUnsigned(argv[3], 16, &pointer_value) || pointer_value == 0 ||
        (std::strcmp(argv[4], "lib") != 0 && std::strcmp(argv[4], "rw") != 0) ||
        !ParseUnsigned(argv[5], 10, &limit_value) || limit_value == 0 ||
        limit_value > kMaximumMaxCandidates) {
        std::fprintf(stderr, "invalid find-pointer argument\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const std::uintptr_t pointer = static_cast<std::uintptr_t>(pointer_value);
    const bool library_only = std::strcmp(argv[4], "lib") == 0;
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return 3;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int memory = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (memory < 0) return 4;
    std::vector<std::uintptr_t> references;
    std::vector<std::string> paths;
    std::vector<std::uint8_t> chunk(kChunkSize);
    std::uint64_t scanned_bytes = 0;
    std::uint64_t unreadable_bytes = 0;
    for (const auto& mapping : maps) {
        const bool selected = library_only ?
                (mapping.perms[0] == 'r' &&
                 mapping.path.find("libAsphalt9.so") != std::string::npos) :
                IsPrivateWritableObjectMapping(mapping);
        if (!selected) continue;
        for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(chunk.size(), mapping.end - cursor));
            const ssize_t got = pread(memory, chunk.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                unreadable_bytes += want;
                cursor += want;
                continue;
            }
            scanned_bytes += static_cast<std::uint64_t>(got);
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= static_cast<std::size_t>(got);
                 offset += alignof(std::uintptr_t)) {
                if (LoadUnaligned<std::uintptr_t>(chunk.data(), offset) == pointer) {
                    references.push_back(cursor + offset);
                    paths.push_back(mapping.path.empty() ? "anonymous" : mapping.path);
                    if (references.size() >= limit_value) break;
                }
            }
            if (references.size() >= limit_value) break;
            cursor += static_cast<std::uintptr_t>(got);
        }
        if (references.size() >= limit_value) break;
    }
    close(memory);
    std::printf(
        "A9POINTER_DISCOVERY_V1 passed=1 pid=%d value=0x%" PRIxPTR
        " scope=%s references=%zu capped=%u scanned_bytes=%" PRIu64
        " unreadable_bytes=%" PRIu64 "\n",
        static_cast<int>(pid), pointer, argv[4], references.size(),
        references.size() >= limit_value ? 1u : 0u, scanned_bytes,
        unreadable_bytes);
    for (std::size_t index = 0; index < references.size(); ++index) {
        std::printf("A9POINTER_REFERENCE_V1 index=%zu address=0x%" PRIxPTR
                    " mapping=%s\n", index, references[index],
                    paths[index].c_str());
    }
    return 0;
}

int InventoryLibraryPointers(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s --inventory PID LIB_BEGIN_HEX LIB_END_HEX\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t begin_value = 0;
    std::uint64_t end_value = 0;
    if (!ParseUnsigned(argv[2], 10, &pid_value) || pid_value == 0 ||
        pid_value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
        !ParseUnsigned(argv[3], 16, &begin_value) ||
        !ParseUnsigned(argv[4], 16, &end_value) || begin_value == 0 ||
        begin_value >= end_value) {
        std::fprintf(stderr, "invalid inventory argument\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const std::uintptr_t library_begin = static_cast<std::uintptr_t>(begin_value);
    const std::uintptr_t library_end = static_cast<std::uintptr_t>(end_value);
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) return 3;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int memory = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (memory < 0) return 4;
    struct CountAndFirst {
        std::uint64_t count{};
        std::uintptr_t first{};
    };
    std::unordered_map<std::uintptr_t, CountAndFirst> inventory;
    std::vector<std::uint8_t> chunk(kChunkSize);
    std::uint64_t scanned_bytes = 0;
    std::uint64_t unreadable_bytes = 0;
    for (const auto& mapping : maps) {
        if (!IsPrivateWritableObjectMapping(mapping)) continue;
        for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uintptr_t>(chunk.size(), mapping.end - cursor));
            const ssize_t got = pread(memory, chunk.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                unreadable_bytes += want;
                cursor += want;
                continue;
            }
            scanned_bytes += static_cast<std::uint64_t>(got);
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= static_cast<std::size_t>(got);
                 offset += alignof(std::uintptr_t)) {
                const std::uintptr_t value =
                    LoadUnaligned<std::uintptr_t>(chunk.data(), offset);
                if (value < library_begin || value >= library_end) continue;
                auto& entry = inventory[value];
                if (entry.count == 0) entry.first = cursor + offset;
                ++entry.count;
            }
            cursor += static_cast<std::uintptr_t>(got);
        }
    }
    close(memory);
    std::vector<std::uintptr_t> values;
    values.reserve(inventory.size());
    for (const auto& item : inventory) values.push_back(item.first);
    std::sort(values.begin(), values.end());
    std::printf(
        "A9LIB_POINTER_INVENTORY_V1 passed=1 pid=%d lib_begin=0x%" PRIxPTR
        " lib_end=0x%" PRIxPTR " unique=%zu scanned_bytes=%" PRIu64
        " unreadable_bytes=%" PRIu64 "\n",
        static_cast<int>(pid), library_begin, library_end, values.size(),
        scanned_bytes, unreadable_bytes);
    for (const auto value : values) {
        const auto& entry = inventory[value];
        std::printf("A9LIB_POINTER_V1 value=0x%" PRIxPTR
                    " count=%" PRIu64 " first=0x%" PRIxPTR "\n",
                    value, entry.count, entry.first);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0) return SelfTest();
    if (argc >= 2 && std::strcmp(argv[1], "--find-pointer") == 0)
        return FindPointerReferences(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--inventory") == 0)
        return InventoryLibraryPointers(argc, argv);
    if (argc < 8 || argc > 9) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX VPTR0_RVA_HEX "
                     "VPTR588_RVA_HEX VPTR6C0_RVA_HEX "
                     "VPTR718_RVA_HEX VPTR748_RVA_HEX [MAX_CANDIDATES]\n",
                     argv[0]);
        return 2;
    }

    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t vptr0_rva_value = 0;
    std::uint64_t vptr588_rva_value = 0;
    std::uint64_t vptr6c0_rva_value = 0;
    std::uint64_t vptr718_rva_value = 0;
    std::uint64_t vptr748_rva_value = 0;
    std::uint64_t max_candidates_value = kDefaultMaxCandidates;
    if (!ParseUnsigned(argv[1], 10, &pid_value) || pid_value == 0 ||
        pid_value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
        !ParseUnsigned(argv[2], 16, &base_value) || base_value == 0 ||
        !ParseUnsigned(argv[3], 16, &vptr0_rva_value) ||
        !ParseUnsigned(argv[4], 16, &vptr588_rva_value) ||
        !ParseUnsigned(argv[5], 16, &vptr6c0_rva_value) ||
        !ParseUnsigned(argv[6], 16, &vptr718_rva_value) ||
        !ParseUnsigned(argv[7], 16, &vptr748_rva_value) ||
        (argc == 9 && (!ParseUnsigned(argv[8], 10, &max_candidates_value) ||
                       max_candidates_value == 0 ||
                       max_candidates_value > kMaximumMaxCandidates))) {
        std::fprintf(stderr, "invalid argument\n");
        return 2;
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const std::uintptr_t library_base = static_cast<std::uintptr_t>(base_value);
    std::uintptr_t vptr0 = 0;
    std::uintptr_t vptr588 = 0;
    std::uintptr_t vptr6c0 = 0;
    std::uintptr_t vptr718 = 0;
    std::uintptr_t vptr748 = 0;
    if (!AddNoOverflow(library_base, static_cast<std::uintptr_t>(vptr0_rva_value),
                       &vptr0) ||
        !AddNoOverflow(library_base,
                       static_cast<std::uintptr_t>(vptr588_rva_value),
                       &vptr588) ||
        !AddNoOverflow(library_base,
                       static_cast<std::uintptr_t>(vptr6c0_rva_value),
                       &vptr6c0) ||
        !AddNoOverflow(library_base,
                       static_cast<std::uintptr_t>(vptr718_rva_value),
                       &vptr718) ||
        !AddNoOverflow(library_base,
                       static_cast<std::uintptr_t>(vptr748_rva_value),
                       &vptr748)) {
        std::fprintf(stderr, "address overflow\n");
        return 2;
    }

    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps)) {
        std::perror("read maps");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int memory = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (memory < 0) {
        std::perror("open mem");
        return 4;
    }

    std::vector<CandidateSnapshot> candidates;
    std::vector<std::uint8_t> chunk(kChunkSize + kOverlap);
    std::vector<std::uint8_t> object(kObjectExtent);
    std::uint64_t scanned_bytes = 0;
    std::uint64_t unreadable_bytes = 0;

    for (const auto& mapping : maps) {
        if (!IsPrivateWritableObjectMapping(mapping)) continue;
        for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
            const std::size_t remaining = static_cast<std::size_t>(mapping.end - cursor);
            const std::size_t want = std::min(kChunkSize, remaining);
            const ssize_t got = pread(memory, chunk.data(), want,
                                      static_cast<off_t>(cursor));
            if (got <= 0) {
                unreadable_bytes += want;
                cursor += want;
                continue;
            }
            scanned_bytes += static_cast<std::uint64_t>(got);
            const std::size_t size = static_cast<std::size_t>(got);
            for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= size;
                 offset += alignof(std::uintptr_t)) {
                if (LoadUnaligned<std::uintptr_t>(chunk.data(), offset) != vptr0) {
                    continue;
                }
                const std::uintptr_t address = cursor + offset;
                std::uintptr_t object_end = 0;
                if (!AddNoOverflow(address, kObjectExtent, &object_end) ||
                    object_end > mapping.end ||
                    !ReadExact(memory, address, object.data(), object.size()) ||
                    !MatchesObject(object.data(), vptr0, vptr588, vptr6c0,
                                   vptr718, vptr748)) {
                    continue;
                }
                CandidateSnapshot snapshot{};
                snapshot.address = address;
                snapshot.vptr0 = LoadUnaligned<std::uintptr_t>(object.data(), 0);
                snapshot.vptr588 = LoadUnaligned<std::uintptr_t>(
                    object.data(), kVptr588Offset);
                snapshot.vptr6c0 = LoadUnaligned<std::uintptr_t>(
                    object.data(), kVptr6c0Offset);
                snapshot.vptr718 = LoadUnaligned<std::uintptr_t>(
                    object.data(), kVptr718Offset);
                snapshot.vptr748 = LoadUnaligned<std::uintptr_t>(
                    object.data(), kVptr748Offset);
                snapshot.object_hash = Fnv1a64(object.data(), object.size());
                snapshot.mapping_path = mapping.path.empty() ? "anonymous" : mapping.path;
                candidates.push_back(std::move(snapshot));
                if (candidates.size() >= max_candidates_value) break;
            }
            if (candidates.size() >= max_candidates_value) break;
            cursor += static_cast<std::uintptr_t>(got);
        }
        if (candidates.size() >= max_candidates_value) break;
    }
    close(memory);

    std::printf(
        "A9PRACTICE_DISCOVERY_V1 passed=1 pid=%d base=0x%" PRIxPTR
        " vptr0=0x%" PRIxPTR " vptr588=0x%" PRIxPTR
        " vptr6c0=0x%" PRIxPTR " vptr718=0x%" PRIxPTR
        " vptr748=0x%" PRIxPTR " candidates=%zu capped=%u"
        " scanned_bytes=%" PRIu64 " unreadable_bytes=%" PRIu64 "\n",
        static_cast<int>(pid), library_base, vptr0, vptr588, vptr6c0,
        vptr718, vptr748,
        candidates.size(), candidates.size() >= max_candidates_value ? 1u : 0u,
        scanned_bytes, unreadable_bytes);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        std::printf(
            "A9PRACTICE_CANDIDATE_V1 index=%zu address=0x%" PRIxPTR
            " vptr0=0x%" PRIxPTR " vptr588=0x%" PRIxPTR
            " vptr6c0=0x%" PRIxPTR " vptr718=0x%" PRIxPTR
            " vptr748=0x%" PRIxPTR
            " object_fnv1a64=0x%016" PRIx64 " mapping=%s\n",
            index, candidate.address, candidate.vptr0, candidate.vptr588,
            candidate.vptr6c0, candidate.vptr718, candidate.vptr748,
            candidate.object_hash,
            candidate.mapping_path.c_str());
    }
    return 0;
}
