#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_ACTION_SUB_V3";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-action-subscriber-probe-v3";
constexpr const char* kDumpOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-action-subscriber-v3.bin";

constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

// Intrusive-list dispatcher. Its first four instructions are a relocatable
// stack-only prologue, so they are safe to copy into a trampoline.
constexpr std::uintptr_t kActionDispatchOffset = 0x35f79cc;
constexpr std::uint8_t kActionDispatchSignature[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf7, 0x5b, 0x03, 0xa9,
    0xf5, 0x53, 0x04, 0xa9, 0xf3, 0x7b, 0x05, 0xa9,
};

constexpr std::size_t kRingCapacity = 512;

struct SnapshotSlot {
    std::atomic<std::uint64_t> commit{};
    std::uint64_t call_sequence{};
    std::uint64_t caller_offset{};
    std::uint64_t container{};
    std::uint64_t head{};
    std::uint64_t count{};
    std::uint64_t object{};
    std::uint64_t live_owner{};
    std::uint64_t removed{};
    std::uint64_t vtable{};
    std::uint64_t callback{};
};

struct DumpRecord {
    std::uint64_t event_sequence{};
    std::uint64_t call_sequence{};
    std::uint64_t caller_offset{};
    std::uint64_t container{};
    std::uint64_t head{};
    std::uint64_t count{};
    std::uint64_t object{};
    std::uint64_t live_owner{};
    std::uint64_t removed{};
    std::uint64_t vtable{};
    std::uint64_t callback{};
    std::uint8_t callback_bytes[0x100];
};

struct DumpHeader {
    char magic[8];
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint64_t guest_base;
    std::uint64_t total_calls;
    std::uint64_t total_events;
    std::uint32_t record_count;
    std::uint32_t object_bytes;
    std::uint32_t vtable_bytes;
    std::uint32_t reserved;
    std::uint64_t copied_object;
    std::uint64_t copied_vtable;
};

static_assert(sizeof(DumpRecord) == 344);

using DispatchFn = void (*)(void* container);

std::atomic<std::uintptr_t> g_guest_base{0};
std::atomic<std::uintptr_t> g_trampoline{0};
std::atomic<std::uint64_t> g_call_count{0};
std::atomic<std::uint64_t> g_event_count{0};
std::atomic<std::uint64_t> g_last_topology_key{~std::uint64_t{0}};
SnapshotSlot g_ring[kRingCapacity]{};
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

std::uint64_t ToOffset(std::uintptr_t address, std::uintptr_t base) {
    return address >= base ? static_cast<std::uint64_t>(address - base)
                           : static_cast<std::uint64_t>(address);
}

void RecordTopology(void* container_pointer, std::uint64_t call_sequence,
                    std::uintptr_t caller) {
    const auto container =
        reinterpret_cast<std::uintptr_t>(container_pointer);
    const auto head = *reinterpret_cast<const std::uintptr_t*>(container);
    const auto count = *reinterpret_cast<const std::uint64_t*>(container + 0x10);
    const auto object = head != 0 ? head - 0x10 : 0;
    std::uintptr_t live_owner = 0;
    std::uint8_t removed = 1;
    std::uintptr_t vtable = 0;
    std::uintptr_t callback = 0;
    if (count != 0 && object != 0) {
        // Mirror the eligibility reads in 0x35fc578 without changing its
        // in-flight counter at object+0x30. The original dispatcher accesses
        // the vtable only when owner!=0 and removed==0.
        live_owner =
            *reinterpret_cast<const std::uintptr_t*>(object + 0x20);
        removed = *reinterpret_cast<const std::uint8_t*>(object + 0x28);
        if (live_owner != 0 && removed == 0) {
            vtable = *reinterpret_cast<const std::uintptr_t*>(object);
            callback =
                *reinterpret_cast<const std::uintptr_t*>(vtable + 0x40);
        }
    }

    // Keep the first 64 calls, then only topology changes and sparse samples.
    // This captures late race-state transitions without imposing per-call I/O.
    const std::uint64_t topology_key =
        static_cast<std::uint64_t>(object) ^
        (static_cast<std::uint64_t>(callback) << 1) ^ (count << 33) ^
        (static_cast<std::uint64_t>(removed) << 17);
    const std::uint64_t old_key =
        g_last_topology_key.exchange(topology_key, std::memory_order_relaxed);
    if (call_sequence >= 64 && topology_key == old_key &&
        call_sequence % 3000 != 0) {
        return;
    }

    const std::uint64_t event =
        g_event_count.fetch_add(1, std::memory_order_relaxed);
    SnapshotSlot& slot = g_ring[event % kRingCapacity];
    slot.commit.store(0, std::memory_order_relaxed);
    slot.call_sequence = call_sequence;
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    slot.caller_offset = ToOffset(caller, base);
    slot.container = container;
    slot.head = head;
    slot.count = count;
    slot.object = object;
    slot.live_owner = live_owner;
    slot.removed = removed;
    slot.vtable = vtable;
    slot.callback = callback;
    slot.commit.store(event + 1, std::memory_order_release);
}

extern "C" __attribute__((noinline)) void ActionSubscriberProbe(
    void* container) {
    const std::uint64_t call_sequence =
        g_call_count.fetch_add(1, std::memory_order_relaxed);
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    RecordTopology(container, call_sequence, caller);
    const auto trampoline = g_trampoline.load(std::memory_order_acquire);
    reinterpret_cast<DispatchFn>(trampoline)(container);
}

bool RangeMapped(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0 || address + size < address) return false;
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[512]{};
    bool mapped = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0;
        unsigned long long end = 0;
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

bool DumpSnapshots() {
    const std::uint64_t total_events =
        g_event_count.load(std::memory_order_acquire);
    const std::uint64_t first =
        total_events > kRingCapacity ? total_events - kRingCapacity : 0;
    std::uint32_t count = 0;
    for (std::uint64_t event = first; event < total_events; ++event) {
        SnapshotSlot& slot = g_ring[event % kRingCapacity];
        const std::uint64_t expected = event + 1;
        if (slot.commit.load(std::memory_order_acquire) != expected) continue;
        DumpRecord record{event,
                          slot.call_sequence,
                          slot.caller_offset,
                          slot.container,
                          slot.head,
                          slot.count,
                          slot.object,
                          slot.live_owner,
                          slot.removed,
                          slot.vtable,
                          slot.callback};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.commit.load(std::memory_order_relaxed) != expected) continue;
        g_dump_records[count++] = record;
    }

    std::uint8_t object_bytes[0x100]{};
    std::uint8_t vtable_bytes[0x100]{};
    std::uint32_t object_size = 0;
    std::uint32_t vtable_size = 0;
    std::uintptr_t copied_object = 0;
    std::uintptr_t copied_vtable = 0;
    for (std::uint32_t i = count; i > 0; --i) {
        const auto& record = g_dump_records[i - 1];
        if (record.object != 0 && record.vtable != 0) {
            copied_object = static_cast<std::uintptr_t>(record.object);
            copied_vtable = static_cast<std::uintptr_t>(record.vtable);
            if (RangeMapped(copied_object, sizeof(object_bytes))) {
                std::memcpy(object_bytes,
                            reinterpret_cast<const void*>(copied_object),
                            sizeof(object_bytes));
                object_size = sizeof(object_bytes);
            }
            if (RangeMapped(copied_vtable, sizeof(vtable_bytes))) {
                std::memcpy(vtable_bytes,
                            reinterpret_cast<const void*>(copied_vtable),
                            sizeof(vtable_bytes));
                vtable_size = sizeof(vtable_bytes);
            }
            break;
        }
    }

    FILE* file = std::fopen(kDumpOutput, "wb");
    if (file == nullptr) return false;
    DumpHeader header{{'A', '9', 'A', 'S', 'P', '3', '\0', '\0'}, {},
                      sizeof(DumpRecord),
                      g_guest_base.load(std::memory_order_relaxed),
                      g_call_count.load(std::memory_order_relaxed),
                      total_events,
                      count,
                      object_size,
                      vtable_size,
                      0,
                      copied_object,
                      copied_vtable};
    std::memcpy(header.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
    bool success = std::fwrite(&header, sizeof(header), 1, file) == 1;
    success = success &&
              (count == 0 ||
               std::fwrite(g_dump_records, sizeof(DumpRecord), count, file) ==
                   count);
    success = success &&
              (object_size == 0 ||
               std::fwrite(object_bytes, object_size, 1, file) == 1);
    success = success &&
              (vtable_size == 0 ||
               std::fwrite(vtable_bytes, vtable_size, 1, file) == 1);
    std::fclose(file);
    return success;
}

void* StatusWorker(void*) {
    std::uint64_t previous_events = ~std::uint64_t{0};
    while (true) {
        sleep(2);
        const std::uint64_t events =
            g_event_count.load(std::memory_order_acquire);
        if (events == previous_events) continue;
        previous_events = events;
        const bool dumped = DumpSnapshots();
        __android_log_print(
            dumped ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
            "calls=%llu events=%llu dump=%s path=%s",
            static_cast<unsigned long long>(
                g_call_count.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(events),
            dumped ? "ok" : "failed", kDumpOutput);
    }
}

bool InstallProbe(std::uintptr_t guest_base) {
    auto* target =
        reinterpret_cast<std::uint8_t*>(guest_base + kActionDispatchOffset);
    if (std::memcmp(target, kActionDispatchSignature,
                    sizeof(kActionDispatchSignature)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "dispatcher signature mismatch; probe disabled");
        return false;
    }
    const std::uintptr_t trampoline =
        BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "trampoline build failed; probe disabled");
        return false;
    }
    g_trampoline.store(trampoline, std::memory_order_release);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(
        patch, reinterpret_cast<std::uintptr_t>(&ActionSubscriberProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        g_trampoline.store(0, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "patch write failed; probe disabled");
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t read_back[16]{};
    std::memcpy(read_back, target, sizeof(read_back));
    if (std::memcmp(read_back, patch, sizeof(patch)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "patch verification failed");
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "probe installed target=%p trampoline=%p", target,
                        reinterpret_cast<void*>(trampoline));
    return true;
}

void* VerificationWorker(void*) {
    // Houdini translated-code installation races during early startup on this
    // exact LDPlayer configuration. Retain the verified quiet period.
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
    std::uint8_t build_id[20]{};
    if (guest_base == 0 || !ReadBuildId(mapping.path, build_id) ||
        std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "guest base or Build ID mismatch; probe disabled");
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
                        "loaded passive=1 candidate=action-subscriber-v3");
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
