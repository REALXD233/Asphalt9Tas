#include "g2_physics_interval_passthrough_v1.h"

#include <android/log.h>
#include <elf.h>
#include <jni.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace a9tas::g2_physics_interval_v1;

#ifndef A9TAS_G2_SOURCE_SHA256
#define A9TAS_G2_SOURCE_SHA256 UNSET
#endif
#ifndef A9TAS_G2_EXPECTED_PAYLOAD_PATH
#define A9TAS_G2_EXPECTED_PAYLOAD_PATH \
    /data/local/tmp/liba9tas_g2_physics_interval_passthrough_v1.so
#endif

#define A9TAS_G2_STRINGIFY_INNER(value) #value
#define A9TAS_G2_STRINGIFY(value) A9TAS_G2_STRINGIFY_INNER(value)

namespace {

constexpr const char* kTag = "A9TAS_G2_PI";
constexpr const char* kSourceSha256 =
    A9TAS_G2_STRINGIFY(A9TAS_G2_SOURCE_SHA256);
constexpr const char* kExpectedPayloadPath =
    A9TAS_G2_STRINGIFY(A9TAS_G2_EXPECTED_PAYLOAD_PATH);
constexpr const char* kGameBasename = "libAsphalt9.so";
constexpr const char* kGameBuildId =
    "e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b";
constexpr const char* kGameFileSha256 =
    "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0";
constexpr std::uintptr_t kTargetRva = 0x3695474;
constexpr std::size_t kPatchSize = 16;
constexpr int kLogicalCodeProtection = PROT_READ | PROT_EXEC;

constexpr std::uint8_t kExpectedPrologue[kPatchSize] = {
    0xff, 0xc3, 0x00, 0xd1, 0xf5, 0x53, 0x01, 0xa9,
    0xf3, 0x7b, 0x02, 0xa9, 0x35, 0x11, 0x91, 0x52,
};

constexpr std::uint32_t kInstallPassTag = 0x47324901;
constexpr std::uint32_t kRestorePassTag = 0x47324902;
constexpr std::uint32_t kFailureTag = 0x47324910;

enum Error : std::uint32_t {
    kErrorNone = 0,
    kErrorCommand = 1,
    kErrorControl = 2,
    kErrorGameIdentity = 3,
    kErrorTargetRange = 4,
    kErrorPrologue = 5,
    kErrorOwner = 6,
    kErrorProtection = 7,
    kErrorPatchReadback = 8,
    kErrorAlreadyInstalled = 9,
    kErrorNotInstalled = 10,
    kErrorActiveCall = 11,
    kErrorRestoreReadback = 12,
    kErrorOutput = 13,
};

struct MappingMetadata {
    std::uintptr_t start{};
    std::uintptr_t end{};
    int protection{};
    bool found{};
    bool query_ok{};
    bool private_mapping{};
};

struct GameImage {
    std::uintptr_t base{};
    const Elf64_Phdr* programs{};
    std::uint16_t program_count{};
    bool found{};
};

alignas(64) Control g_control{};
alignas(64) Evidence g_evidence{};
alignas(64) Event g_events[kCapacity]{};
std::atomic<std::int32_t> g_install_state{0};

extern "C" {
__attribute__((visibility("hidden")))
std::uintptr_t g_g2_physics_interval_tail_v1 = 0;
}

std::size_t AlignNote(std::size_t value) {
    return (value + 3U) & ~std::size_t{3U};
}

bool BuildIdMatches(std::uintptr_t base, const Elf64_Phdr* programs,
                    std::uint16_t count) {
    constexpr char kHex[] = "0123456789abcdef";
    char observed[65]{};
    for (std::uint16_t index = 0; index < count; ++index) {
        const Elf64_Phdr& program = programs[index];
        if (program.p_type != PT_NOTE || program.p_memsz < sizeof(Elf64_Nhdr))
            continue;
        const auto* cursor = reinterpret_cast<const std::uint8_t*>(
            base + program.p_vaddr);
        const auto* end = cursor + program.p_memsz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            std::memcpy(&note, cursor, sizeof(note));
            cursor += sizeof(note);
            const std::size_t name_size = AlignNote(note.n_namesz);
            const std::size_t desc_size = AlignNote(note.n_descsz);
            if (name_size > static_cast<std::size_t>(end - cursor) ||
                desc_size > static_cast<std::size_t>(end - cursor) - name_size)
                return false;
            const auto* name = cursor;
            const auto* description = cursor + name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(name, "GNU", 4) == 0 && note.n_descsz == 20) {
                for (std::size_t byte = 0; byte < note.n_descsz; ++byte) {
                    observed[byte * 2] = kHex[description[byte] >> 4];
                    observed[byte * 2 + 1] = kHex[description[byte] & 0xf];
                }
                observed[note.n_descsz * 2] = '\0';
                return std::strcmp(observed, kGameBuildId) == 0;
            }
            cursor = description + desc_size;
        }
    }
    return false;
}

int FindGameImage(dl_phdr_info* info, size_t, void* opaque) {
    if (info == nullptr || info->dlpi_name == nullptr || opaque == nullptr)
        return 0;
    const char* slash = std::strrchr(info->dlpi_name, '/');
    const char* basename = slash != nullptr ? slash + 1 : info->dlpi_name;
    if (std::strcmp(basename, kGameBasename) != 0) return 0;
    if (!BuildIdMatches(info->dlpi_addr, info->dlpi_phdr,
                        static_cast<std::uint16_t>(info->dlpi_phnum)))
        return 0;
    auto* image = static_cast<GameImage*>(opaque);
    if (image->found) {
        image->base = 0;
        return 1;
    }
    image->base = info->dlpi_addr;
    image->programs = info->dlpi_phdr;
    image->program_count = static_cast<std::uint16_t>(info->dlpi_phnum);
    image->found = true;
    return 0;
}

bool TargetInExecutableLoad(const GameImage& image, std::uintptr_t target) {
    if (!image.found || image.base == 0 || image.programs == nullptr ||
        target > UINTPTR_MAX - kPatchSize)
        return false;
    const std::uintptr_t target_end = target + kPatchSize;
    for (std::uint16_t index = 0; index < image.program_count; ++index) {
        const Elf64_Phdr& program = image.programs[index];
        if (program.p_type != PT_LOAD ||
            (program.p_flags & (PF_R | PF_W | PF_X)) != (PF_R | PF_X))
            continue;
        const std::uintptr_t begin = image.base + program.p_vaddr;
        const std::uintptr_t end = begin + program.p_memsz;
        if (target >= begin && target_end <= end) return true;
    }
    return false;
}

MappingMetadata QueryMapping(const void* address) {
    MappingMetadata metadata{};
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return metadata;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    char line[2048]{};
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
            continue;
        if (target < start || target >= end) continue;
        metadata.start = static_cast<std::uintptr_t>(start);
        metadata.end = static_cast<std::uintptr_t>(end);
        if (perms[0] == 'r') metadata.protection |= PROT_READ;
        if (perms[1] == 'w') metadata.protection |= PROT_WRITE;
        if (perms[2] == 'x') metadata.protection |= PROT_EXEC;
        metadata.private_mapping = perms[3] == 'p';
        metadata.found = true;
        break;
    }
    metadata.query_ok = std::ferror(maps) == 0;
    std::fclose(maps);
    return metadata;
}

bool MappingCovers(const MappingMetadata& mapping, const void* address,
                   std::size_t size) {
    if (!mapping.query_ok || !mapping.found || address == nullptr || size == 0)
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    return begin <= UINTPTR_MAX - size && begin >= mapping.start &&
           begin + size <= mapping.end;
}

bool PrivateGuestCodeView(const MappingMetadata& mapping, const void* address,
                          std::size_t size) {
    return MappingCovers(mapping, address, size) && mapping.private_mapping &&
           (mapping.protection == PROT_READ ||
            mapping.protection == (PROT_READ | PROT_EXEC));
}

bool PrivateWritableNoExec(const MappingMetadata& mapping, const void* address,
                           std::size_t size) {
    return MappingCovers(mapping, address, size) && mapping.private_mapping &&
           mapping.protection == (PROT_READ | PROT_WRITE);
}

bool PrivateReadableWritable(const MappingMetadata& mapping,
                             const void* address, std::size_t size) {
    return MappingCovers(mapping, address, size) && mapping.private_mapping &&
           (mapping.protection & (PROT_READ | PROT_WRITE)) ==
               (PROT_READ | PROT_WRITE) &&
           (mapping.protection & PROT_EXEC) == 0;
}

void AbsoluteJump(std::uint8_t patch_bytes[kPatchSize],
                  std::uintptr_t destination) {
    constexpr std::uint32_t kLoadX17 = 0x58000051;
    constexpr std::uint32_t kBranchX17 = 0xd61f0220;
    std::memcpy(patch_bytes, &kLoadX17, sizeof(kLoadX17));
    std::memcpy(patch_bytes + 4, &kBranchX17, sizeof(kBranchX17));
    std::memcpy(patch_bytes + 8, &destination, sizeof(destination));
}

bool ValidBits(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && value >= 0.001f && value <= 0.1f;
}

std::uint64_t MonotonicNs() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

void ResetEvidence() {
    std::memset(&g_evidence, 0, sizeof(g_evidence));
    std::memset(g_events, 0, sizeof(g_events));
    std::memcpy(g_evidence.magic, kEvidenceMagic, sizeof(kEvidenceMagic));
    g_evidence.version = kVersion;
    g_evidence.size = sizeof(g_evidence);
    g_evidence.status = kStatusPassive;
    g_evidence.flags = kOutputWriteAbsent;
}

void Initialize() {
    std::memset(&g_control, 0, sizeof(g_control));
    std::memcpy(g_control.magic, kControlMagic, sizeof(kControlMagic));
    g_control.version = kVersion;
    g_control.size = sizeof(g_control);
    ResetEvidence();
}

void Fault(Error error) {
    __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_evidence.semantic_errors, 1ULL,
                       __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_evidence.first_error, __ATOMIC_RELAXED) == 0)
        __atomic_store_n(&g_evidence.first_error,
                         static_cast<std::uint32_t>(error),
                         __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.status, kStatusFault, __ATOMIC_RELEASE);
}

bool ControlConfigured() {
    return std::memcmp(g_control.magic, kControlMagic,
                       sizeof(kControlMagic)) == 0 &&
           g_control.version == kVersion && g_control.size == sizeof(Control) &&
           g_control.enabled == 0 && g_control.limit > 0 &&
           g_control.limit <= kCapacity && g_control.cursor == 0 &&
           g_control.completed == 0 && g_control.active_calls == 0 &&
           g_control.expected_owner != 0 &&
           g_control.expected_owner_vptr != 0;
}

std::uint64_t EncodeReturn(std::uint32_t tag) {
    const auto tid = static_cast<std::uint32_t>(syscall(__NR_gettid));
    return (static_cast<std::uint64_t>(tid) << 32) | tag;
}

}  // namespace

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G2PhysicsIntervalOriginalTrampolineV1() {
    __asm__ volatile(
        "sub sp, sp, #0x30\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov w21, #0x8889\n"
        "adrp x17, :got:g_g2_physics_interval_tail_v1\n"
        "ldr x17, [x17, #:got_lo12:g_g2_physics_interval_tail_v1]\n"
        "ldr x17, [x17]\n"
        "br x17\n");
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
G2PhysicsIntervalAfterOriginalV1(void* owner, const std::uint32_t* output) {
    if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
        output == nullptr)
        return;
    __atomic_fetch_add(&g_control.active_calls, 1u, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&g_evidence.wrapper_returns, 1ULL,
                       __ATOMIC_RELAXED);

    if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
        owner == nullptr || g_control.target_tail == 0 ||
        g_control.wrapper == 0) {
        Fault(kErrorControl);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }

    if (reinterpret_cast<std::uintptr_t>(owner) != g_control.expected_owner) {
        __atomic_fetch_add(&g_evidence.unqualified_returns, 1ULL,
                           __ATOMIC_RELAXED);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }
    const std::uintptr_t owner_vptr = __atomic_load_n(
        reinterpret_cast<const std::uintptr_t*>(owner), __ATOMIC_RELAXED);
    if (owner_vptr != g_control.expected_owner_vptr) {
        __atomic_fetch_add(&g_evidence.identity_errors, 1ULL,
                           __ATOMIC_RELAXED);
        Fault(kErrorOwner);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }

    const std::uint32_t bits = __atomic_load_n(output, __ATOMIC_RELAXED);
    if (!ValidBits(bits)) {
        Fault(kErrorOutput);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }

    const std::uint32_t sequence = __atomic_fetch_add(
        &g_control.cursor, 1u, __ATOMIC_ACQ_REL);
    if (sequence >= g_control.limit || sequence >= kCapacity) {
        Fault(kErrorControl);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }
    const auto tid = static_cast<std::uint32_t>(syscall(__NR_gettid));
    const std::uint64_t previous_tid = __atomic_load_n(
        &g_evidence.last_tid, __ATOMIC_RELAXED);
    if (sequence == 0) {
        __atomic_store_n(&g_evidence.first_tid, tid, __ATOMIC_RELAXED);
        __atomic_store_n(&g_evidence.first_bits, bits, __ATOMIC_RELAXED);
    } else if (previous_tid != 0 && previous_tid != tid) {
        __atomic_fetch_add(&g_evidence.tid_changes, 1ULL,
                           __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_evidence.last_tid, tid, __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_bits, bits, __ATOMIC_RELAXED);

    Event* event = &g_events[sequence];
    event->sequence = sequence;
    event->owner = reinterpret_cast<std::uintptr_t>(owner);
    event->output = reinterpret_cast<std::uintptr_t>(output);
    event->monotonic_ns = MonotonicNs();
    event->tid = tid;
    event->output_bits = bits;
    event->owner_vptr_low = static_cast<std::uint32_t>(owner_vptr);
    __atomic_store_n(&event->commit_sequence, sequence + 1,
                     __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_evidence.qualified_returns, 1ULL,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_evidence.valid_outputs, 1ULL,
                       __ATOMIC_RELAXED);

    if (sequence + 1 >= g_control.limit) {
        __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_evidence.status, kStatusComplete,
                         __ATOMIC_RELEASE);
    }
    __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
G2PhysicsIntervalEntryV1() {
    __asm__ volatile(
        "sub sp, sp, #0x160\n"
        "stp x19, x20, [sp, #0x00]\n"
        "str x30, [sp, #0x10]\n"
        "mov x19, x0\n"
        "mov x20, x8\n"
        "bl G2PhysicsIntervalOriginalTrampolineV1\n"
        "stp x0, x1, [sp, #0x20]\n"
        "stp x2, x3, [sp, #0x30]\n"
        "stp x4, x5, [sp, #0x40]\n"
        "stp x6, x7, [sp, #0x50]\n"
        "stp x8, x9, [sp, #0x60]\n"
        "stp x10, x11, [sp, #0x70]\n"
        "stp x12, x13, [sp, #0x80]\n"
        "stp x14, x15, [sp, #0x90]\n"
        "stp x16, x17, [sp, #0xA0]\n"
        "str x18, [sp, #0xB0]\n"
        "stp q0, q1, [sp, #0xC0]\n"
        "stp q2, q3, [sp, #0xE0]\n"
        "stp q4, q5, [sp, #0x100]\n"
        "stp q6, q7, [sp, #0x120]\n"
        "mrs x9, nzcv\n"
        "mrs x10, fpcr\n"
        "stp x9, x10, [sp, #0x140]\n"
        "mrs x9, fpsr\n"
        "str x9, [sp, #0x150]\n"
        "mov x0, x19\n"
        "mov x1, x20\n"
        "bl G2PhysicsIntervalAfterOriginalV1\n"
        "ldr x9, [sp, #0x150]\n"
        "msr fpsr, x9\n"
        "ldp x9, x10, [sp, #0x140]\n"
        "msr nzcv, x9\n"
        "msr fpcr, x10\n"
        "ldp q6, q7, [sp, #0x120]\n"
        "ldp q4, q5, [sp, #0x100]\n"
        "ldp q2, q3, [sp, #0xE0]\n"
        "ldp q0, q1, [sp, #0xC0]\n"
        "ldr x18, [sp, #0xB0]\n"
        "ldp x16, x17, [sp, #0xA0]\n"
        "ldp x14, x15, [sp, #0x90]\n"
        "ldp x12, x13, [sp, #0x80]\n"
        "ldp x10, x11, [sp, #0x70]\n"
        "ldp x8, x9, [sp, #0x60]\n"
        "ldp x6, x7, [sp, #0x50]\n"
        "ldp x4, x5, [sp, #0x40]\n"
        "ldp x2, x3, [sp, #0x30]\n"
        "ldp x0, x1, [sp, #0x20]\n"
        "ldr x30, [sp, #0x10]\n"
        "ldp x19, x20, [sp, #0x00]\n"
        "add sp, sp, #0x160\n"
        "ret\n");
}

namespace {

bool InstallAndArm() {
    std::int32_t expected_state = 0;
    if (!g_install_state.compare_exchange_strong(
            expected_state, -2, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        Fault(kErrorAlreadyInstalled);
        return false;
    }
    if (!ControlConfigured()) {
        Fault(kErrorControl);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    ResetEvidence();
    __atomic_fetch_add(&g_evidence.install_calls, 1ULL,
                       __ATOMIC_RELAXED);

    GameImage image{};
    dl_iterate_phdr(FindGameImage, &image);
    if (!image.found || image.base == 0) {
        Fault(kErrorGameIdentity);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    g_evidence.flags |= kGameIdentity;
    const std::uintptr_t target = image.base + kTargetRva;
    if (!TargetInExecutableLoad(image, target)) {
        Fault(kErrorTargetRange);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    auto* const target_bytes = reinterpret_cast<std::uint8_t*>(target);
    if (std::memcmp(target_bytes, kExpectedPrologue, kPatchSize) != 0) {
        Fault(kErrorPrologue);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    g_evidence.flags |= kPrologueIdentity;

    const auto* owner = reinterpret_cast<const std::uintptr_t*>(
        g_control.expected_owner);
    const MappingMetadata owner_mapping = QueryMapping(owner);
    if (!PrivateReadableWritable(owner_mapping, owner, sizeof(*owner)) ||
        __atomic_load_n(owner, __ATOMIC_RELAXED) !=
            g_control.expected_owner_vptr) {
        Fault(kErrorOwner);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    g_evidence.flags |= kOwnerIdentity;

    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        Fault(kErrorProtection);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    const std::size_t page_size = static_cast<std::size_t>(page_size_value);
    const std::uintptr_t page = target & ~(page_size - 1);
    const MappingMetadata initial_mapping = QueryMapping(target_bytes);
    if (!PrivateGuestCodeView(initial_mapping, target_bytes, kPatchSize) ||
        !MappingCovers(initial_mapping, reinterpret_cast<void*>(page),
                       page_size)) {
        Fault(kErrorProtection);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    g_evidence.flags |= kTargetPageIsolated;

    std::uint8_t patch[kPatchSize]{};
    AbsoluteJump(patch,
                 reinterpret_cast<std::uintptr_t>(&G2PhysicsIntervalEntryV1));
    g_control.game_base = image.base;
    g_control.target_entry = target;
    g_control.target_tail = target + kPatchSize;
    g_control.wrapper =
        reinterpret_cast<std::uintptr_t>(&G2PhysicsIntervalEntryV1);
    g_g2_physics_interval_tail_v1 = target + kPatchSize;

    const bool made_writable =
        mprotect(reinterpret_cast<void*>(page), page_size,
                 PROT_READ | PROT_WRITE) == 0;
    const bool writable_readback =
        made_writable &&
        PrivateWritableNoExec(QueryMapping(reinterpret_cast<void*>(page)),
                              reinterpret_cast<void*>(page), page_size);
    if (!writable_readback) {
        if (made_writable) {
            (void)mprotect(reinterpret_cast<void*>(page), page_size,
                           kLogicalCodeProtection);
        }
        Fault(kErrorProtection);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    std::memcpy(target_bytes, patch, kPatchSize);
    __builtin___clear_cache(reinterpret_cast<char*>(target_bytes),
                            reinterpret_cast<char*>(target_bytes + kPatchSize));
    g_evidence.flags |= kPatchPublished;
    const bool patch_readback =
        std::memcmp(target_bytes, patch, kPatchSize) == 0;
    const bool rx_restored =
        mprotect(reinterpret_cast<void*>(page), page_size,
                 kLogicalCodeProtection) == 0 &&
        PrivateGuestCodeView(QueryMapping(reinterpret_cast<void*>(page)),
                             reinterpret_cast<void*>(page), page_size);
    if (!patch_readback || !rx_restored) {
        if (mprotect(reinterpret_cast<void*>(page), page_size,
                     PROT_READ | PROT_WRITE) == 0) {
            std::memcpy(target_bytes, kExpectedPrologue, kPatchSize);
            __builtin___clear_cache(
                reinterpret_cast<char*>(target_bytes),
                reinterpret_cast<char*>(target_bytes + kPatchSize));
            (void)mprotect(reinterpret_cast<void*>(page), page_size,
                           kLogicalCodeProtection);
        }
        Fault(patch_readback ? kErrorProtection : kErrorPatchReadback);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    g_evidence.flags |= kPatchReadback | kTargetLogicalRx;
    __atomic_store_n(&g_evidence.status, kStatusArmed,
                     __ATOMIC_RELEASE);
    g_install_state.store(1, std::memory_order_release);
    __atomic_store_n(&g_control.enabled, 1u, __ATOMIC_RELEASE);
    return true;
}

bool Restore() {
    std::int32_t expected_state = 1;
    if (!g_install_state.compare_exchange_strong(
            expected_state, -3, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        Fault(kErrorNotInstalled);
        return false;
    }
    __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_evidence.restore_calls, 1ULL,
                       __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_control.active_calls, __ATOMIC_ACQUIRE) != 0) {
        Fault(kErrorActiveCall);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }

    auto* const target = reinterpret_cast<std::uint8_t*>(
        g_control.target_entry);
    std::uint8_t patch[kPatchSize]{};
    AbsoluteJump(patch, g_control.wrapper);
    if (target == nullptr ||
        std::memcmp(target, patch, kPatchSize) != 0) {
        Fault(kErrorPatchReadback);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    const long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        Fault(kErrorProtection);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    const std::size_t page_size = static_cast<std::size_t>(page_size_value);
    const std::uintptr_t target_address =
        reinterpret_cast<std::uintptr_t>(target);
    const std::uintptr_t page = target_address & ~(page_size - 1);
    const bool made_writable =
        mprotect(reinterpret_cast<void*>(page), page_size,
                 PROT_READ | PROT_WRITE) == 0;
    const bool writable_readback =
        made_writable &&
        PrivateWritableNoExec(QueryMapping(reinterpret_cast<void*>(page)),
                              reinterpret_cast<void*>(page), page_size);
    if (!writable_readback) {
        if (made_writable) {
            (void)mprotect(reinterpret_cast<void*>(page), page_size,
                           kLogicalCodeProtection);
        }
        Fault(kErrorProtection);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    std::memcpy(target, kExpectedPrologue, kPatchSize);
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + kPatchSize));
    const bool bytes_restored =
        std::memcmp(target, kExpectedPrologue, kPatchSize) == 0;
    const bool rx_restored =
        mprotect(reinterpret_cast<void*>(page), page_size,
                 kLogicalCodeProtection) == 0 &&
        PrivateGuestCodeView(QueryMapping(reinterpret_cast<void*>(page)),
                             reinterpret_cast<void*>(page), page_size);
    if (!bytes_restored || !rx_restored) {
        Fault(bytes_restored ? kErrorProtection : kErrorRestoreReadback);
        g_install_state.store(-1, std::memory_order_release);
        return false;
    }
    g_evidence.flags |= kRestoreReadback | kRestoredLogicalRx;
    g_g2_physics_interval_tail_v1 = 0;
    g_install_state.store(2, std::memory_order_release);
    __atomic_store_n(&g_evidence.status, kStatusRestored,
                     __ATOMIC_RELEASE);
    return true;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_g2_physics_interval_protocol_v1() {
    return kVersion;
}

extern "C" __attribute__((visibility("default"))) jlong
a9tas_g2_physics_interval_command_v1(JNIEnv*, jobject, jlong command) {
    bool passed = false;
    std::uint32_t tag = kFailureTag;
    if (static_cast<std::uint64_t>(command) ==
        static_cast<std::uint64_t>(Command::kInstallAndArm)) {
        passed = InstallAndArm();
        if (passed) tag = kInstallPassTag;
    } else if (static_cast<std::uint64_t>(command) ==
               static_cast<std::uint64_t>(Command::kRestore)) {
        passed = Restore();
        if (passed) tag = kRestorePassTag;
    } else {
        Fault(kErrorCommand);
    }
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        kTag,
                        "command=%lld passed=%d status=%d cursor=%u error=%u",
                        static_cast<long long>(command), passed ? 1 : 0,
                        g_evidence.status, g_control.cursor,
                        g_evidence.first_error);
    return static_cast<jlong>(EncodeReturn(tag));
}

extern "C" __attribute__((visibility("default"))) Control*
a9tas_g2_physics_interval_control_v1() {
    return &g_control;
}

extern "C" __attribute__((visibility("default"))) Evidence*
a9tas_g2_physics_interval_evidence_v1() {
    return &g_evidence;
}

extern "C" __attribute__((visibility("default"))) Event*
a9tas_g2_physics_interval_events_v1() {
    return g_events;
}

extern "C" __attribute__((visibility("default"))) const char*
a9tas_g2_physics_interval_source_sha256_v1() {
    return kSourceSha256;
}

extern "C" __attribute__((visibility("default"))) const char*
a9tas_g2_physics_interval_expected_path_v1() {
    return kExpectedPayloadPath;
}

extern "C" __attribute__((visibility("default"))) const char*
a9tas_g2_physics_interval_game_sha256_v1() {
    return kGameFileSha256;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g2_physics_interval_wrapper_data_v1 =
        reinterpret_cast<std::uintptr_t>(&G2PhysicsIntervalEntryV1);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g2_physics_interval_control_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g2_physics_interval_evidence_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_g2_physics_interval_events_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_events);

__attribute__((constructor, visibility("hidden"))) void OnLoad() {
    Initialize();
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 protocol=1 installer=explicit-only");
}
