#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_VALUES";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-control-values-probe";
constexpr const char* kDumpRequest =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-control-values-dump";
constexpr const char* kDumpOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-control-values.bin";

constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

// 0x366d4b0 performs exactly two semantic operations:
//   FourFloats v = ReadFourFloats(source);       // 0x366f8a8
//   Consume(a, b, &v, d);                        // 0x4bff63c
// Replacing the whole wrapper avoids relocating its PC-relative BL while
// preserving the original data flow. This probe observes only; it never
// changes the four values.
constexpr std::uintptr_t kWrapperOffset = 0x366d4b0;
constexpr std::uintptr_t kReadFourFloatsOffset = 0x366f8a8;
constexpr std::uintptr_t kConsumeOffset = 0x4bff63c;
constexpr std::uint8_t kWrapperSignature[16] = {
    0xff, 0xc3, 0x00, 0xd1, 0xf5, 0x53, 0x01, 0xa9,
    0xf3, 0x7b, 0x02, 0xa9, 0xf5, 0x03, 0x00, 0xaa,
};

struct FourFloats {
    float value[4];
};
static_assert(sizeof(FourFloats) == 16);

using ReadFourFloatsFn = FourFloats (*)(const void* source);
using ConsumeFn = void (*)(void* a, void* b, const FourFloats* values, void* d);

ReadFourFloatsFn g_read_four_floats = nullptr;
ConsumeFn g_consume = nullptr;
std::atomic<std::uintptr_t> g_guest_base{0};
std::atomic<std::uint64_t> g_call_count{0};
std::atomic<std::uintptr_t> g_latest_caller{0};
std::atomic<std::uintptr_t> g_latest_source{0};
std::atomic<std::uint32_t> g_latest_bits[4]{};
std::atomic<bool> g_installed{false};
std::uintptr_t g_trampoline = 0;
std::uint8_t g_original[16]{};

constexpr std::size_t kRingCapacity = 4096;

struct SampleSlot {
    std::atomic<std::uint64_t> commit{};
    std::uint64_t caller_offset{};
    std::uint64_t source{};
    std::uint32_t value_bits[4]{};
};

struct DumpRecord {
    std::uint64_t sequence{};
    std::uint64_t caller_offset{};
    std::uint64_t source{};
    std::uint32_t value_bits[4]{};
};

struct DumpHeader {
    char magic[8];
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint64_t total_calls;
    std::uint32_t record_count;
    std::uint32_t reserved;
};

SampleSlot g_ring[kRingCapacity]{};
DumpRecord g_dump_records[kRingCapacity]{};

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
        unsigned long long start = 0;
        unsigned long long end = 0;
        unsigned long long offset = 0;
        char permissions[5]{};
        char path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &start, &end,
            permissions, &offset, path);
        if (fields == 5 && offset == 0 &&
            std::strstr(path, "libAsphalt9.so") != nullptr) {
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
        g_guest_base.store(static_cast<std::uintptr_t>(info->dlpi_addr),
                           std::memory_order_release);
        return 1;
    }
    return 0;
}

bool ReadBuildId(const char* path, std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;

    Elf64_Ehdr header{};
    const bool valid_header =
        std::fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
        header.e_ident[EI_CLASS] == ELFCLASS64 &&
        header.e_machine == EM_AARCH64 &&
        header.e_phentsize == sizeof(Elf64_Phdr);
    if (!valid_header) {
        std::fclose(file);
        return false;
    }

    bool found = false;
    for (std::uint16_t index = 0; index < header.e_phnum && !found; ++index) {
        Elf64_Phdr program{};
        if (std::fseek(file, static_cast<long>(header.e_phoff) +
                                static_cast<long>(index) * sizeof(program),
                       SEEK_SET) != 0 ||
            std::fread(&program, sizeof(program), 1, file) != 1) {
            break;
        }
        if (program.p_type != PT_NOTE || program.p_filesz > 1024 * 1024) {
            continue;
        }

        std::uint64_t cursor = program.p_offset;
        const std::uint64_t end = program.p_offset + program.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1) {
                break;
            }
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
                found = std::fread(output, 20, 1, file) == 1;
                break;
            }
            cursor += desc_size;
        }
    }
    std::fclose(file);
    return found;
}

void WriteAbsoluteJump(std::uint8_t output[16], std::uintptr_t target) {
    constexpr std::uint32_t load_x17_literal = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xd61f0220;
    std::memcpy(output, &load_x17_literal, sizeof(load_x17_literal));
    std::memcpy(output + 4, &branch_x17, sizeof(branch_x17));
    std::memcpy(output + 8, &target, sizeof(target));
}

bool WriteProcMem(std::uintptr_t address, const std::uint8_t bytes[16]) {
    const int descriptor = open("/proc/self/mem", O_RDWR);
    if (descriptor < 0) return false;
    const ssize_t written =
        pwrite(descriptor, bytes, 16, static_cast<off_t>(address));
    close(descriptor);
    return written == 16;
}

std::uintptr_t BuildTrampoline(std::uintptr_t target) {
    auto* stub = static_cast<std::uint8_t*>(
        mmap(nullptr, 32, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (stub == MAP_FAILED) return 0;
    std::memcpy(stub, reinterpret_cast<const void*>(target), 16);
    WriteAbsoluteJump(stub + 16, target + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(stub),
                            reinterpret_cast<char*>(stub + 32));
    if (mprotect(stub, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(stub, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(stub);
}

extern "C" __attribute__((noinline)) void ControlValuesProbe(
    void* a, void* b, const void* source, void* d) {
    const FourFloats values = g_read_four_floats(source);
    const std::uint64_t index =
        g_call_count.fetch_add(1, std::memory_order_relaxed);
    const std::uintptr_t caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    const std::uintptr_t caller_offset = caller >= base ? caller - base : caller;

    SampleSlot& slot = g_ring[index % kRingCapacity];
    slot.commit.store(0, std::memory_order_relaxed);
    slot.caller_offset = caller_offset;
    slot.source = reinterpret_cast<std::uintptr_t>(source);
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint32_t bits =
            std::bit_cast<std::uint32_t>(values.value[i]);
        slot.value_bits[i] = bits;
        g_latest_bits[i].store(bits, std::memory_order_relaxed);
    }
    slot.commit.store(index + 1, std::memory_order_release);
    g_latest_caller.store(caller_offset, std::memory_order_relaxed);
    g_latest_source.store(reinterpret_cast<std::uintptr_t>(source),
                          std::memory_order_relaxed);

    if (g_trampoline != 0) {
        using Fn = void (*)(void*, void*, const void*, void*);
        reinterpret_cast<Fn>(g_trampoline)(a, b, source, d);
    } else {
        g_consume(a, b, &values, d);
    }
}

bool InstallProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kWrapperOffset);
    if (std::memcmp(target, kWrapperSignature, sizeof(kWrapperSignature)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "wrapper signature mismatch; probe disabled");
        return false;
    }

    g_read_four_floats = reinterpret_cast<ReadFourFloatsFn>(
        guest_base + kReadFourFloatsOffset);
    g_consume = reinterpret_cast<ConsumeFn>(guest_base + kConsumeOffset);
    std::memcpy(g_original, target, sizeof(g_original));
    const std::uintptr_t trampoline = BuildTrampoline(
        reinterpret_cast<std::uintptr_t>(target));
    if (trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "wrapper trampoline failed; probe disabled");
        return false;
    }
    g_trampoline = trampoline;

    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch,
                      reinterpret_cast<std::uintptr_t>(&ControlValuesProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        std::memcpy(target, g_original, sizeof(g_original));
        __builtin___clear_cache(reinterpret_cast<char*>(target),
                                reinterpret_cast<char*>(target + 16));
        if (g_trampoline != 0) {
            munmap(reinterpret_cast<void*>(g_trampoline), 32);
            g_trampoline = 0;
        }
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "wrapper patch write failed; probe disabled");
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));

    std::uint8_t read_back[16]{};
    std::memcpy(read_back, target, sizeof(read_back));
    if (std::memcmp(read_back, patch, sizeof(patch)) != 0) {
        std::memcpy(target, g_original, sizeof(g_original));
        __builtin___clear_cache(reinterpret_cast<char*>(target),
                                reinterpret_cast<char*>(target + 16));
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "wrapper patch verification failed");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "control-values probe installed target=%p trampoline=%p", target,
                        reinterpret_cast<void*>(trampoline));
    return true;
}

bool DumpRing() {
    const std::uint64_t total = g_call_count.load(std::memory_order_acquire);
    const std::uint64_t first =
        total > kRingCapacity ? total - kRingCapacity : 0;
    std::uint32_t count = 0;

    for (std::uint64_t index = first; index < total; ++index) {
        SampleSlot& slot = g_ring[index % kRingCapacity];
        const std::uint64_t expected = index + 1;
        const std::uint64_t before =
            slot.commit.load(std::memory_order_acquire);
        if (before != expected) continue;

        DumpRecord record{};
        record.sequence = index;
        record.caller_offset = slot.caller_offset;
        record.source = slot.source;
        std::memcpy(record.value_bits, slot.value_bits,
                    sizeof(record.value_bits));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.commit.load(std::memory_order_relaxed) != expected) continue;
        g_dump_records[count++] = record;
    }

    FILE* file = std::fopen(kDumpOutput, "wb");
    if (file == nullptr) return false;
    DumpHeader header{{'A', '9', 'C', 'V', 'P', '1', '\0', '\0'}, {},
                      sizeof(DumpRecord), total, count, 0};
    std::memcpy(header.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
    const bool success =
        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        (count == 0 ||
         std::fwrite(g_dump_records, sizeof(DumpRecord), count, file) == count);
    std::fclose(file);
    return success;
}

void* StatusWorker(void*) {
    while (true) {
        sleep(5);
        const std::uint64_t calls =
            g_call_count.load(std::memory_order_relaxed);
        const std::uintptr_t caller =
            g_latest_caller.load(std::memory_order_relaxed);
        float values[4]{};
        for (std::size_t i = 0; i < 4; ++i) {
            values[i] = std::bit_cast<float>(
                g_latest_bits[i].load(std::memory_order_relaxed));
        }
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "calls=%llu caller=0x%zx values=%.6g,%.6g,%.6g,%.6g",
            static_cast<unsigned long long>(calls),
            static_cast<std::size_t>(caller), static_cast<double>(values[0]),
            static_cast<double>(values[1]), static_cast<double>(values[2]),
            static_cast<double>(values[3]));

        if (access(kDumpRequest, F_OK) == 0) {
            const bool dumped = DumpRing();
            unlink(kDumpRequest);
            __android_log_print(dumped ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                                kTag, "ring dump %s calls=%llu path=%s",
                                dumped ? "complete" : "failed",
                                static_cast<unsigned long long>(calls),
                                kDumpOutput);
        }
    }
}

void* VerificationWorker(void*) {
    // Existing Houdini evidence shows that installing during startup races the
    // translator. Keep the verified two-minute quiet period.
    sleep(120);

    GameMapping mapping{};
    if (!FindGameMapping(&mapping)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "game mapping unavailable; probe disabled");
        return nullptr;
    }
    dl_iterate_phdr(FindGuestGameModule, nullptr);
    const std::uintptr_t guest_base =
        g_guest_base.load(std::memory_order_acquire);
    if (guest_base == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "guest base unavailable; probe disabled");
        return nullptr;
    }

    std::uint8_t build_id[20]{};
    if (!ReadBuildId(mapping.path, build_id) ||
        std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "Build ID mismatch; probe disabled");
        return nullptr;
    }
    if (access(kEnableMarker, F_OK) != 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "probe disabled by marker policy");
        return nullptr;
    }
    if (!InstallProbe(guest_base)) return nullptr;

    pthread_t status_thread{};
    if (pthread_create(&status_thread, nullptr, StatusWorker, nullptr) == 0) {
        pthread_detach(status_thread);
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 candidate=control-values-v1");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, VerificationWorker, nullptr) == 0) {
        pthread_detach(worker);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 2;
}

