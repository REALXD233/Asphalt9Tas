#include <android/log.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_PAYLOAD";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

std::atomic<bool> g_loaded{false};
std::atomic<bool> g_build_verified{false};
std::atomic<std::uintptr_t> g_game_base{0};
std::atomic<std::uintptr_t> g_guest_game_base{0};
std::atomic<std::uint64_t> g_physics_dispatch_calls{0};
using PhysicsDispatchFn = void (*)(void*, const void*);
PhysicsDispatchFn g_original_physics_dispatch = nullptr;

struct CandidateSignature {
    const char* name;
    std::uintptr_t offset;
    const std::uint8_t* bytes;
    size_t size;
};

constexpr std::uint8_t kPhysicsCtor[] = {
    0xf4,0x0f,0x1e,0xf8,0xf3,0x7b,0x01,0xa9,0xf4,0x03,0x01,0xaa,0xf3,0x03,0x00,0xaa,
    0x38,0x00,0x00,0x94,0x68,0x42,0x02,0xb0,0x08,0xc1,0x20,0x91,0x60,0xa2,0x02,0x91,
};
constexpr std::uint8_t kPhysicsIntervalGet[] = {
    0x09,0x78,0x41,0xb9,0x09,0x01,0x00,0xb9,0xc0,0x03,0x5f,0xd6,
};
constexpr std::uint8_t kPhysicsDispatch[] = {
    0xf6,0x0f,0x1d,0xf8,0xf5,0x53,0x01,0xa9,0xf3,0x7b,0x02,0xa9,0x08,0x24,0x40,0xa9,
    0xf4,0x03,0x01,0xaa,0xf3,0x03,0x00,0xaa,0x2a,0x00,0x80,0x52,0x28,0x01,0x08,0xcb,
};
constexpr std::uint8_t kHidJni[] = {
    0xe0,0x03,0x02,0x2a,0x01,0x00,0x00,0x14,0xe8,0x0f,0x1d,0xfc,0xf5,0x53,0x01,0xa9,
    0xf3,0x7b,0x02,0xa9,0x88,0x43,0x02,0xd0,0x14,0xb5,0x44,0xf9,0x08,0x1c,0xa0,0x4e,
};
constexpr std::uint8_t kKeyboardJni[] = {
    0xe0,0x03,0x02,0x2a,0x01,0x00,0x00,0x14,0xe8,0x0f,0x1d,0xfc,0xf5,0x53,0x01,0xa9,
    0xf3,0x7b,0x02,0xa9,0x88,0x43,0x02,0xb0,0x14,0xe1,0x44,0xf9,0x08,0x1c,0xa0,0x4e,
};
constexpr std::uint8_t kReplayReader[] = {
    0x13,0x00,0x80,0x12,0xe8,0x0f,0x00,0xf9,0xe8,0x57,0x41,0xb9,0x3f,0x03,0x08,0x6b,
    0x02,0x19,0x00,0x54,0xe0,0xb3,0x40,0xf9,0x00,0xe4,0x00,0x6f,0x7f,0x23,0x00,0xb9,
};

constexpr CandidateSignature kCandidates[] = {
    {"physics_ctor", 0x38b6e60, kPhysicsCtor, sizeof(kPhysicsCtor)},
    {"physics_interval_get", 0x38b77c0, kPhysicsIntervalGet, sizeof(kPhysicsIntervalGet)},
    {"physics_dispatch", 0x38b7930, kPhysicsDispatch, sizeof(kPhysicsDispatch)},
    {"hid_jni", 0x5d4a69c, kHidJni, sizeof(kHidJni)},
    {"keyboard_jni", 0x5d4b368, kKeyboardJni, sizeof(kKeyboardJni)},
    {"replay_reader", 0x61b2a80, kReplayReader, sizeof(kReplayReader)},
};

constexpr std::uintptr_t kPhysicsVtableOffset = 0x8103830;
constexpr std::uintptr_t kPhysicsDispatchOffset = 0x38b7930;
constexpr std::uintptr_t kPhysicsVtableMethods[] = {
    0x38b6f94, 0x38b7088, 0x38b70a0, 0x38b7258, 0x38b76c8, 0x38b76e0,
    0x38b76f4, 0x38b77ac, 0x38b77b4, 0x38b77c0, 0x38b77cc, 0x38b7840,
    0x38b79d4, 0x38b77a4, 0x38b79fc, 0x38b7b54, 0x38b7b5c, 0x38b7a00,
    0x38b7a1c, 0x38b7a3c, 0x38b7a54,
};

extern "C" __attribute__((noinline)) void PhysicsDispatchProbe(void* context,
                                                                 const void* frame) {
    g_physics_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
    g_original_physics_dispatch(context, frame);
}

void WriteAbsoluteJump(std::uint8_t* destination, std::uintptr_t target) {
    // ldr x17, #8; br x17; .quad target
    constexpr std::uint32_t load_x17_literal = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xD61F0220;
    std::memcpy(destination, &load_x17_literal, sizeof(load_x17_literal));
    std::memcpy(destination + 4, &branch_x17, sizeof(branch_x17));
    std::memcpy(destination + 8, &target, sizeof(target));
}

bool InstallPhysicsDispatchProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kPhysicsDispatchOffset);
    if (std::memcmp(target, kPhysicsDispatch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime dispatch bytes mismatch; probe disabled");
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(
        mmap(nullptr, 32, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (trampoline == MAP_FAILED) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "trampoline mmap failed");
        return false;
    }
    std::memcpy(trampoline, target, 16);
    WriteAbsoluteJump(trampoline + 16, reinterpret_cast<std::uintptr_t>(target + 16));
    __builtin___clear_cache(reinterpret_cast<char*>(trampoline),
                            reinterpret_cast<char*>(trampoline + 32));
    if (mprotect(trampoline, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(trampoline, 32);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "trampoline mprotect failed");
        return false;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        munmap(trampoline, 32);
        return false;
    }
    void* page = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) &
        ~static_cast<std::uintptr_t>(page_size - 1));
    if (mprotect(page, static_cast<size_t>(page_size),
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(trampoline, 32);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "target mprotect failed");
        return false;
    }

    g_original_physics_dispatch = reinterpret_cast<PhysicsDispatchFn>(trampoline);
    WriteAbsoluteJump(target, reinterpret_cast<std::uintptr_t>(&PhysicsDispatchProbe));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    mprotect(page, static_cast<size_t>(page_size), PROT_READ);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "physics dispatch count probe installed target=%p trampoline=%p",
                        target, trampoline);
    return true;
}

void* ProbeReporter(void*) {
    std::uint64_t previous = 0;
    for (int sample = 1; sample <= 6; ++sample) {
        sleep(5);
        const std::uint64_t current =
            g_physics_dispatch_calls.load(std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "physics dispatch sample=%d total=%llu delta_5s=%llu",
                            sample, static_cast<unsigned long long>(current),
                            static_cast<unsigned long long>(current - previous));
        previous = current;
    }
    return nullptr;
}

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

bool FindGameMapping(GameMapping* mapping) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, path);
        if (fields == 5 && offset == 0 && std::strstr(path, "libAsphalt9.so") != nullptr) {
            char* clean = path;
            while (*clean == ' ') ++clean;
            mapping->base = static_cast<std::uintptr_t>(start);
            std::snprintf(mapping->path, sizeof(mapping->path), "%s", clean);
            found = true;
            break;
        }
    }
    std::fclose(maps);
    return found;
}

int FindGuestGameModule(dl_phdr_info* info, size_t, void*) {
    if (info != nullptr && info->dlpi_name != nullptr &&
        std::strstr(info->dlpi_name, "libAsphalt9.so") != nullptr) {
        g_guest_game_base.store(static_cast<std::uintptr_t>(info->dlpi_addr),
                                std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "guest game base=%p phnum=%u path=%s",
                            reinterpret_cast<void*>(info->dlpi_addr),
                            static_cast<unsigned>(info->dlpi_phnum), info->dlpi_name);
        return 1;
    }
    return 0;
}

bool ReadBuildId(const char* path, std::uint8_t out[20]) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;

    Elf64_Ehdr header{};
    const bool header_ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
                           std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
                           header.e_ident[EI_CLASS] == ELFCLASS64 &&
                           header.e_machine == EM_AARCH64 &&
                           header.e_phentsize == sizeof(Elf64_Phdr);
    if (!header_ok) {
        std::fclose(file);
        return false;
    }

    bool found = false;
    for (std::uint16_t index = 0; index < header.e_phnum && !found; ++index) {
        Elf64_Phdr program{};
        if (std::fseek(file, static_cast<long>(header.e_phoff) +
                            static_cast<long>(index) * sizeof(program), SEEK_SET) != 0 ||
            std::fread(&program, sizeof(program), 1, file) != 1) break;
        if (program.p_type != PT_NOTE || program.p_filesz > 1024 * 1024) continue;

        std::uint64_t cursor = program.p_offset;
        const std::uint64_t end = program.p_offset + program.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1) break;
            cursor += sizeof(note);
            const std::uint64_t name_size = (note.n_namesz + 3u) & ~3u;
            const std::uint64_t desc_size = (note.n_descsz + 3u) & ~3u;
            if (cursor + name_size + desc_size > end) break;

            char name[16]{};
            if (note.n_namesz < sizeof(name)) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                std::fread(name, 1, note.n_namesz, file);
            }
            cursor += name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_descsz == 20 &&
                std::strcmp(name, "GNU") == 0) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                found = std::fread(out, 20, 1, file) == 1;
                break;
            }
            cursor += desc_size;
        }
    }
    std::fclose(file);
    return found;
}

void FormatBuildId(const std::uint8_t id[20], char text[41]) {
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 20; ++i) {
        text[i * 2] = hex[id[i] >> 4];
        text[i * 2 + 1] = hex[id[i] & 0xf];
    }
    text[40] = '\0';
}

void* VerificationWorker(void*) {
    GameMapping mapping{};
    for (int attempt = 0; attempt < 30000 && !FindGameMapping(&mapping); ++attempt) {
        usleep(1000);
    }
    if (mapping.base == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "game mapping wait timed out; hooks disabled");
        return nullptr;
    }
    g_game_base.store(mapping.base, std::memory_order_release);
    dl_iterate_phdr(FindGuestGameModule, nullptr);

    std::uint8_t build_id[20]{};
    char build_id_text[41]{};
    const bool read_ok = ReadBuildId(mapping.path, build_id);
    if (read_ok) FormatBuildId(build_id, build_id_text);
    const bool matches = read_ok && std::memcmp(build_id, kExpectedBuildId, 20) == 0;
    g_build_verified.store(matches, std::memory_order_release);
    __android_log_print(matches ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "game base=%p build_id=%s verified=%d hooks_enabled=0",
                        reinterpret_cast<void*>(mapping.base),
                        read_ok ? build_id_text : "unreadable", matches);
    if (!matches) return nullptr;

    FILE* game_file = std::fopen(mapping.path, "rb");
    for (const auto& candidate : kCandidates) {
        std::uint8_t actual[64]{};
        const bool read_signature = game_file != nullptr && candidate.size <= sizeof(actual) &&
            std::fseek(game_file, static_cast<long>(candidate.offset), SEEK_SET) == 0 &&
            std::fread(actual, candidate.size, 1, game_file) == 1;
        const bool signature_matches = read_signature &&
            std::memcmp(actual, candidate.bytes, candidate.size) == 0;
        __android_log_print(signature_matches ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                            kTag, "candidate=%s offset=0x%zx signature=%s",
                            candidate.name, static_cast<size_t>(candidate.offset),
                            signature_matches ? "match" : "mismatch");
    }
    if (game_file != nullptr) std::fclose(game_file);

    const std::uintptr_t guest_base = g_guest_game_base.load(std::memory_order_acquire);
    if (guest_base != 0) {
        auto* vtable = reinterpret_cast<const std::uintptr_t*>(guest_base + kPhysicsVtableOffset);
        size_t vtable_matches = 0;
        for (size_t i = 0; i < sizeof(kPhysicsVtableMethods) / sizeof(kPhysicsVtableMethods[0]); ++i) {
            if (vtable[i] == guest_base + kPhysicsVtableMethods[i]) ++vtable_matches;
        }
        __android_log_print(vtable_matches == 21 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                            kTag, "physics_vtable=%p entries_match=%zu/21 interval_slot=%p",
                            vtable, vtable_matches, reinterpret_cast<void*>(vtable[9]));
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "payload_probe=%p physics_dispatch=%p branch_distance=%lld",
                            reinterpret_cast<void*>(&VerificationWorker),
                            reinterpret_cast<void*>(guest_base + 0x38b7930),
                            static_cast<long long>(reinterpret_cast<std::uintptr_t>(&VerificationWorker)) -
                                static_cast<long long>(guest_base + 0x38b7930));

        if (InstallPhysicsDispatchProbe(guest_base)) {
            pthread_t reporter{};
            if (pthread_create(&reporter, nullptr, ProbeReporter, nullptr) == 0) {
                pthread_detach(reporter);
            }
        }
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    g_loaded.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded pid=%d abi=arm64 passive=1 protocol=1", getpid());
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, VerificationWorker, nullptr) == 0) {
        pthread_detach(worker);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "verification worker creation failed; hooks disabled");
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t a9tas_payload_protocol() {
    return 1;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t a9tas_game_base() {
    return g_game_base.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t a9tas_guest_game_base() {
    return g_guest_game_base.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) int a9tas_build_verified() {
    return g_build_verified.load(std::memory_order_acquire) ? 1 : 0;
}
