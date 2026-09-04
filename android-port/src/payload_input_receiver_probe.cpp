#include <android/log.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_INPUT_RX";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-input-receiver-probe";
constexpr const char* kOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-input-receivers.bin";
constexpr std::uintptr_t kHidTableOffset = 0xa5bc968;
constexpr std::uintptr_t kKeyboardTableOffset = 0xa5bc9c0;
constexpr std::size_t kElementSize = 0x50;
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

struct DumpHeader {
    char magic[8];
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint64_t guest_base;
    std::uint32_t record_count;
    std::uint32_t reserved;
};

struct ReceiverRecord {
    std::uint32_t kind;  // 1=HID, 2=keyboard
    std::uint32_t index;
    std::uint64_t descriptor;
    std::uint64_t handler;
    std::uint64_t device;
    std::uint64_t receiver;
    std::uint64_t receiver_vtable;
    std::uint64_t method_d8;
    std::uint64_t method_e0;
    std::uint8_t device_bytes[0x180];
    std::uint8_t receiver_bytes[0x200];
    std::uint8_t vtable_bytes[0x120];
};

static_assert(sizeof(ReceiverRecord) == 1248);

std::uintptr_t g_guest_base = 0;

bool FindGameMapping(GameMapping* mapping) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0, offset = 0;
        char permissions[5]{}, path[1024]{};
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
        g_guest_base = static_cast<std::uintptr_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

bool ReadBuildId(const char* path, std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    Elf64_Ehdr header{};
    const bool valid = std::fread(&header, sizeof(header), 1, file) == 1 &&
                       std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
                       header.e_ident[EI_CLASS] == ELFCLASS64 &&
                       header.e_machine == EM_AARCH64;
    if (!valid) {
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
        if (program.p_type != PT_NOTE || program.p_filesz > 1024 * 1024)
            continue;
        std::uint64_t cursor = program.p_offset;
        const std::uint64_t end = program.p_offset + program.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1)
                break;
            cursor += sizeof(note);
            const auto name_size = (note.n_namesz + 3u) & ~3u;
            const auto desc_size = (note.n_descsz + 3u) & ~3u;
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

bool RangeMapped(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0 || address + size < address) return false;
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[512]{};
    bool mapped = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, permissions) ==
                3 &&
            address >= start && address + size <= end &&
            permissions[0] == 'r') {
            mapped = true;
            break;
        }
    }
    std::fclose(maps);
    return mapped;
}

template <typename T>
bool SafeRead(T* value, std::uintptr_t address) {
    if (!RangeMapped(address, sizeof(T))) return false;
    std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

void CopyMapped(void* output, std::size_t size, std::uintptr_t address) {
    if (RangeMapped(address, size))
        std::memcpy(output, reinterpret_cast<const void*>(address), size);
}

std::uint32_t CollectTable(std::uintptr_t table_offset, std::uint32_t kind,
                           ReceiverRecord* output, std::uint32_t capacity) {
    std::uintptr_t begin = 0, end = 0;
    if (!SafeRead(&begin, g_guest_base + table_offset) ||
        !SafeRead(&end, g_guest_base + table_offset + 8) || begin == 0 ||
        end < begin || (end - begin) % kElementSize != 0)
        return 0;
    std::size_t count = (end - begin) / kElementSize;
    if (count > capacity) count = capacity;
    std::uint32_t written = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uintptr_t descriptor = begin + index * kElementSize;
        std::uintptr_t handler = 0, device = 0;
        SafeRead(&handler, descriptor + 8);
        SafeRead(&device, descriptor + 0x20);
        if (device == 0) continue;
        ReceiverRecord record{};
        record.kind = kind;
        record.index = static_cast<std::uint32_t>(index);
        record.descriptor = descriptor;
        record.handler = handler;
        record.device = device;
        const std::uintptr_t receiver_offset = kind == 1 ? 0x110 : 0x130;
        SafeRead(&record.receiver, device + receiver_offset);
        if (record.receiver != 0) {
            SafeRead(&record.receiver_vtable, record.receiver);
            if (record.receiver_vtable != 0) {
                SafeRead(&record.method_d8, record.receiver_vtable + 0xd8);
                SafeRead(&record.method_e0, record.receiver_vtable + 0xe0);
                CopyMapped(record.vtable_bytes, sizeof(record.vtable_bytes),
                           record.receiver_vtable);
            }
            CopyMapped(record.receiver_bytes, sizeof(record.receiver_bytes),
                       record.receiver);
        }
        CopyMapped(record.device_bytes, sizeof(record.device_bytes), device);
        output[written++] = record;
    }
    return written;
}

void* Worker(void*) {
    sleep(120);
    GameMapping mapping{};
    dl_iterate_phdr(FindGuestGameModule, nullptr);
    std::uint8_t build_id[20]{};
    if (!FindGameMapping(&mapping) || g_guest_base == 0 ||
        !ReadBuildId(mapping.path, build_id) ||
        std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0 ||
        access(kEnableMarker, F_OK) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "preconditions failed; no read performed");
        return nullptr;
    }

    ReceiverRecord records[16]{};
    std::uint32_t count = CollectTable(kHidTableOffset, 1, records, 8);
    count += CollectTable(kKeyboardTableOffset, 2, records + count, 16 - count);
    FILE* file = std::fopen(kOutput, "wb");
    if (file == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "output open failed");
        return nullptr;
    }
    DumpHeader header{{'A', '9', 'I', 'R', 'X', '1', '\0', '\0'}, {},
                      sizeof(ReceiverRecord), g_guest_base, count, 0};
    std::memcpy(header.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
    const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                    (count == 0 ||
                     std::fwrite(records, sizeof(ReceiverRecord), count, file) ==
                         count);
    std::fclose(file);
    __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "passive dump=%s records=%u path=%s",
                        ok ? "ok" : "failed", count, kOutput);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& r = records[i];
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "kind=%u idx=%u handler_off=0x%zx device=%p receiver=%p "
            "vtable_off=0x%zx d8_off=0x%zx e0_off=0x%zx",
            r.kind, r.index,
            r.handler >= g_guest_base ? r.handler - g_guest_base : r.handler,
            reinterpret_cast<void*>(r.device),
            reinterpret_cast<void*>(r.receiver),
            r.receiver_vtable >= g_guest_base
                ? r.receiver_vtable - g_guest_base
                : r.receiver_vtable,
            r.method_d8 >= g_guest_base ? r.method_d8 - g_guest_base
                                        : r.method_d8,
            r.method_e0 >= g_guest_base ? r.method_e0 - g_guest_base
                                        : r.method_e0);
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 patching=0 candidate=input-rx-v1");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 2;
}
