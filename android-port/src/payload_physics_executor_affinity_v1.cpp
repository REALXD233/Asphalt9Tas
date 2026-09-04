// P1 ARM64 observer for PhysicsContext_execute_simulation_token.
//
// Runtime arming is absent unless A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING is
// explicitly set to 1 at compile time.  The default build remains impossible
// to arm.  Both variants create no thread and do nothing from their
// constructor beyond initializing payload-owned state.

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

#ifndef A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING
#define A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING 0
#endif

#if A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING != 0 && \
    A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING != 1
#error "A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING must be 0 or 1"
#endif

constexpr const char* kTag = "A9TAS_EXEC_AFFINITY_V1";
constexpr bool kRuntimeArmingCompiled =
    A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING == 1;
constexpr std::uintptr_t kTargetOffset = 0x38B74DC;
constexpr std::uint32_t kCapacity = 4096;
constexpr std::uint32_t kDefaultCaptureLimit = 300;
constexpr std::uint8_t kTargetSignature[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
    0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
};
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

enum ReportFlag : std::uint32_t {
    kFlagPassiveBuild = 1u << 0,
    kFlagRuntimeArmingCompiled = 1u << 1,
    kFlagInstalled = 1u << 2,
    kFlagAtomicBranchPatch = 1u << 3,
};

enum EventFlag : std::uint32_t {
    kEventTokenNull = 1u << 0,
    kEventTokenZero = 1u << 1,
};

struct Event {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::uint64_t token_us;
    std::uint64_t context;
    std::uint64_t output;
    std::uint32_t tid;
    std::uint32_t flags;
    std::uint64_t commit_sequence;
};

struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t capacity;
    std::uint32_t flags;
    std::uint32_t reserved;
    std::uint64_t events;
    std::uint64_t dropped;
    std::uint64_t first_ns;
    std::uint64_t last_ns;
    std::uint32_t first_tid;
    std::uint32_t last_tid;
    std::uint32_t tid_changes;
    std::uint32_t null_tokens;
    std::uint64_t zero_tokens;
    std::uint64_t nonzero_tokens;
    std::uint64_t first_token_us;
    std::uint64_t last_token_us;
    std::uint64_t guest_base;
    std::uint64_t target;
};

struct alignas(64) Report {
    Header header;
    Event events[kCapacity];
};

struct alignas(64) Control {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t capture_enabled;
    std::uint32_t completed;
    std::uint32_t active_captures;
    std::uint32_t capture_limit;
    std::uint64_t reserved[4];
};

static_assert(sizeof(Event) == 56, "P1 event ABI");
static_assert(sizeof(Header) == 128, "P1 header ABI");
static_assert(offsetof(Report, events) == sizeof(Header), "P1 report ABI");
static_assert(sizeof(Control) == 64, "P1 control ABI");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "P1 requires lock-free 64-bit atomics");

struct Mapping {
    std::uintptr_t base{};
    char path[1024]{};
};

alignas(64) Report g_report{};
alignas(64) Control g_control{};
std::atomic<std::int32_t> g_status{0};
std::atomic<bool> g_installed{false};
[[maybe_unused]] std::atomic<std::uint32_t> g_arm_phase{0};
std::uint8_t g_original[4]{};
std::uintptr_t g_near_bridge = 0;
std::size_t g_near_bridge_size = 0;
extern "C" std::uintptr_t g_physics_executor_affinity_continue_v1 = 0;

std::uint64_t MonotonicNs() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

void InitializeReport() {
    std::memset(&g_report, 0, sizeof(g_report));
    std::memset(&g_control, 0, sizeof(g_control));
    const char magic[8] = {'A', '9', 'P', 'E', 'A', '1', 0, 0};
    const char control_magic[8] = {'A', '9', 'P', 'E', 'C', '1', 0, 0};
    std::memcpy(g_report.header.magic, magic, sizeof(magic));
    std::memcpy(g_control.magic, control_magic, sizeof(control_magic));
    g_report.header.version = 1;
    g_report.header.header_size = sizeof(Header);
    g_report.header.event_size = sizeof(Event);
    g_report.header.capacity = kCapacity;
    if (kRuntimeArmingCompiled) {
        g_report.header.flags |= kFlagRuntimeArmingCompiled;
    } else {
        g_report.header.flags |= kFlagPassiveBuild;
    }
    g_control.version = 1;
    g_control.size = sizeof(Control);
    g_control.capture_limit = kDefaultCaptureLimit;
}

bool ReserveSequence(std::uint64_t* sequence) {
    if (sequence == nullptr) return false;
    const std::uint64_t limit = __atomic_load_n(
        &g_control.capture_limit, __ATOMIC_ACQUIRE);
    if (limit == 0 || limit > kCapacity) return false;
    std::uint64_t observed =
        __atomic_load_n(&g_report.header.events, __ATOMIC_RELAXED);
    while (observed < limit) {
        if (__atomic_compare_exchange_n(
                &g_report.header.events, &observed, observed + 1, true,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            *sequence = observed;
            return true;
        }
    }
    return false;
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
PhysicsExecutorAffinityCaptureV1(void* context, const std::uint64_t* token,
                                 void* output) {
    if (__atomic_load_n(&g_control.capture_enabled, __ATOMIC_ACQUIRE) == 0)
        return;
    __atomic_fetch_add(&g_control.active_captures, 1u, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&g_control.capture_enabled, __ATOMIC_ACQUIRE) == 0) {
        __atomic_fetch_sub(&g_control.active_captures, 1u, __ATOMIC_RELEASE);
        return;
    }
    std::uint64_t sequence = 0;
    if (!ReserveSequence(&sequence)) {
        __atomic_store_n(&g_control.capture_enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_sub(&g_control.active_captures, 1u, __ATOMIC_RELEASE);
        return;
    }
    const std::uint64_t now = MonotonicNs();
    const std::uint32_t tid =
        static_cast<std::uint32_t>(syscall(SYS_gettid));
    std::uint32_t flags = 0;
    std::uint64_t token_us = 0;
    if (token) {
        token_us = *token;
        if (token_us == 0) flags |= kEventTokenZero;
    } else {
        flags |= kEventTokenNull;
        __atomic_fetch_add(&g_report.header.null_tokens, 1u,
                           __ATOMIC_RELAXED);
    }

    if (sequence == 0) {
        __atomic_store_n(&g_report.header.first_ns, now, __ATOMIC_RELAXED);
        __atomic_store_n(&g_report.header.first_tid, tid, __ATOMIC_RELAXED);
        __atomic_store_n(&g_report.header.first_token_us, token_us,
                         __ATOMIC_RELAXED);
    } else {
        const std::uint32_t previous =
            __atomic_load_n(&g_report.header.last_tid, __ATOMIC_RELAXED);
        if (previous != 0 && previous != tid)
            __atomic_fetch_add(&g_report.header.tid_changes, 1u,
                               __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_report.header.last_ns, now, __ATOMIC_RELAXED);
    __atomic_store_n(&g_report.header.last_tid, tid, __ATOMIC_RELAXED);
    __atomic_store_n(&g_report.header.last_token_us, token_us,
                     __ATOMIC_RELAXED);
    if (!token) {
        // Counted above as null; do not also classify it as a zero token.
    } else if (token_us == 0) {
        __atomic_fetch_add(&g_report.header.zero_tokens, 1ULL,
                           __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&g_report.header.nonzero_tokens, 1ULL,
                           __ATOMIC_RELAXED);
    }

    if (sequence >= kCapacity) {
        __atomic_fetch_add(&g_report.header.dropped, 1ULL, __ATOMIC_RELAXED);
        __atomic_store_n(&g_control.capture_enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_sub(&g_control.active_captures, 1u, __ATOMIC_RELEASE);
        return;
    }
    Event* event = &g_report.events[sequence];
    event->sequence = sequence;
    event->monotonic_ns = now;
    event->token_us = token_us;
    event->context = reinterpret_cast<std::uintptr_t>(context);
    event->output = reinterpret_cast<std::uintptr_t>(output);
    event->tid = tid;
    event->flags = flags;
    __atomic_store_n(&event->commit_sequence, sequence + 1,
                     __ATOMIC_RELEASE);
    const std::uint64_t limit = __atomic_load_n(
        &g_control.capture_limit, __ATOMIC_ACQUIRE);
    if (sequence + 1 >= limit) {
        __atomic_store_n(&g_control.capture_enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
    }
    __atomic_fetch_sub(&g_control.active_captures, 1u, __ATOMIC_RELEASE);
}

// The entry uses a private 0x160-byte frame and preserves all volatile integer
// argument registers, Q0-Q7, NZCV, FPCR and FPSR across the C capture call.
// X17 is consumed by the near bridge's absolute jump and is used again only to
// branch to the continuation.  The target itself is changed with one aligned
// 32-bit B instruction.  The continuation, and only the continuation, executes
// the overwritten original 4-byte prologue instruction.
extern "C" __attribute__((naked, visibility("hidden"))) void
PhysicsExecutorAffinityEntryV1() {
    __asm__ volatile(
        "sub sp, sp, #0x160\n"
        "stp x0, x1, [sp, #0x00]\n"
        "stp x2, x3, [sp, #0x10]\n"
        "stp x4, x5, [sp, #0x20]\n"
        "stp x6, x7, [sp, #0x30]\n"
        "stp x8, x9, [sp, #0x40]\n"
        "stp x10, x11, [sp, #0x50]\n"
        "stp x12, x13, [sp, #0x60]\n"
        "stp x14, x15, [sp, #0x70]\n"
        "stp x16, x17, [sp, #0x80]\n"
        "stp x18, x30, [sp, #0x90]\n"
        "stp q0, q1, [sp, #0xA0]\n"
        "stp q2, q3, [sp, #0xC0]\n"
        "stp q4, q5, [sp, #0xE0]\n"
        "stp q6, q7, [sp, #0x100]\n"
        "mrs x9, nzcv\n"
        "mrs x10, fpcr\n"
        "stp x9, x10, [sp, #0x120]\n"
        "mrs x9, fpsr\n"
        "str x9, [sp, #0x130]\n"
        "ldr x0, [sp, #0x00]\n"
        "ldr x1, [sp, #0x08]\n"
        "ldr x2, [sp, #0x40]\n"
        "bl PhysicsExecutorAffinityCaptureV1\n"
        "ldr x9, [sp, #0x130]\n"
        "msr fpsr, x9\n"
        "ldp x9, x10, [sp, #0x120]\n"
        "msr nzcv, x9\n"
        "msr fpcr, x10\n"
        "ldp q6, q7, [sp, #0x100]\n"
        "ldp q4, q5, [sp, #0xE0]\n"
        "ldp q2, q3, [sp, #0xC0]\n"
        "ldp q0, q1, [sp, #0xA0]\n"
        "ldp x18, x30, [sp, #0x90]\n"
        "ldp x16, x17, [sp, #0x80]\n"
        "ldp x14, x15, [sp, #0x70]\n"
        "ldp x12, x13, [sp, #0x60]\n"
        "ldp x10, x11, [sp, #0x50]\n"
        "ldp x8, x9, [sp, #0x40]\n"
        "ldp x6, x7, [sp, #0x30]\n"
        "ldp x4, x5, [sp, #0x20]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "ldp x0, x1, [sp, #0x00]\n"
        "add sp, sp, #0x160\n"
        "adrp x17, :got:g_physics_executor_affinity_continue_v1\n"
        "ldr x17, [x17, #:got_lo12:g_physics_executor_affinity_continue_v1]\n"
        "ldr x17, [x17]\n"
        "br x17\n");
}

[[maybe_unused]] bool FindGame(Mapping* result) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &start,
            &end, perms, &offset, path);
        if (fields == 5 && offset == 0 &&
            std::strstr(path, "libAsphalt9.so")) {
            char* clean = path;
            while (*clean == ' ') ++clean;
            result->base = static_cast<std::uintptr_t>(start);
            std::snprintf(result->path, sizeof(result->path), "%s", clean);
            found = true;
            break;
        }
    }
    std::fclose(maps);
    return found;
}

[[maybe_unused]] bool ReadBuildId(const char* path,
                                  std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Elf64_Ehdr header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_AARCH64) {
        std::fclose(file);
        return false;
    }
    bool found = false;
    for (std::uint16_t index = 0; index < header.e_phnum && !found; ++index) {
        Elf64_Phdr phdr{};
        if (std::fseek(file, static_cast<long>(header.e_phoff) +
                                static_cast<long>(index) * sizeof(phdr),
                       SEEK_SET) != 0 ||
            std::fread(&phdr, sizeof(phdr), 1, file) != 1)
            break;
        if (phdr.p_type != PT_NOTE || phdr.p_filesz > 1024 * 1024) continue;
        std::uint64_t cursor = phdr.p_offset;
        const std::uint64_t end = phdr.p_offset + phdr.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1)
                break;
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

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination) {
    constexpr std::uint32_t kLoadX17 = 0x58000051;
    constexpr std::uint32_t kBranchX17 = 0xD61F0220;
    std::memcpy(output, &kLoadX17, sizeof(kLoadX17));
    std::memcpy(output + 4, &kBranchX17, sizeof(kBranchX17));
    std::memcpy(output + 8, &destination, sizeof(destination));
}

[[maybe_unused]] bool ProcMem(std::uintptr_t address, void* buffer,
                              std::size_t size, bool write) {
    const int fd = open("/proc/self/mem",
                        (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t done = write
        ? pwrite(fd, buffer, size, static_cast<off_t>(address))
        : pread(fd, buffer, size, static_cast<off_t>(address));
    close(fd);
    return done == static_cast<ssize_t>(size);
}

[[maybe_unused]] std::uintptr_t BuildContinuation(std::uintptr_t target) {
    auto* code = static_cast<std::uint8_t*>(mmap(
        nullptr, 32, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return 0;
    if (!ProcMem(target, code, 4, false)) {
        munmap(code, 32);
        return 0;
    }
    AbsoluteJump(code + 4, target + 4);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + 20));
    if (mprotect(code, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(code);
}

bool EncodeBranch(std::uintptr_t source, std::uintptr_t destination,
                  std::uint32_t* instruction) {
    if (instruction == nullptr || (source & 3u) != 0 ||
        (destination & 3u) != 0)
        return false;
    const std::int64_t delta = static_cast<std::int64_t>(destination) -
                               static_cast<std::int64_t>(source);
    if ((delta & 3) != 0 || delta < -0x08000000LL ||
        delta > 0x07FFFFFCLL)
        return false;
    const std::uint32_t immediate =
        static_cast<std::uint32_t>(delta >> 2) & 0x03FFFFFFu;
    *instruction = 0x14000000u | immediate;
    return true;
}

std::uintptr_t TryNearPage(std::uintptr_t target, std::uintptr_t hint,
                           std::size_t page_size) {
    void* mapped = mmap(reinterpret_cast<void*>(hint), page_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return 0;
    const auto address = reinterpret_cast<std::uintptr_t>(mapped);
    std::uint32_t ignored = 0;
    if (!EncodeBranch(target, address, &ignored)) {
        munmap(mapped, page_size);
        return 0;
    }
    return address;
}

__attribute__((noinline)) std::uintptr_t BuildNearBridge(
    std::uintptr_t target, std::uintptr_t destination,
    std::size_t* mapped_size) {
    if (mapped_size == nullptr) return 0;
    const long page_value = sysconf(_SC_PAGESIZE);
    if (page_value <= 0 ||
        (static_cast<unsigned long>(page_value) &
         (static_cast<unsigned long>(page_value) - 1)) != 0)
        return 0;
    const std::size_t page_size = static_cast<std::size_t>(page_value);
    const std::uintptr_t mask = page_size - 1;
    constexpr std::uintptr_t kMargin = 0x07000000;
    const std::uintptr_t low = target > kMargin ? target - kMargin : page_size;
    const std::uintptr_t high =
        target < UINTPTR_MAX - kMargin ? target + kMargin : UINTPTR_MAX;
    std::uintptr_t cursor = (low + mask) & ~mask;
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return 0;
    char line[2048]{};
    std::uintptr_t bridge = 0;
    while (!bridge && std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0;
        if (std::sscanf(line, "%llx-%llx", &start, &end) != 2) continue;
        const std::uintptr_t map_start = static_cast<std::uintptr_t>(start);
        const std::uintptr_t map_end = static_cast<std::uintptr_t>(end);
        if (map_end <= cursor) continue;
        if (map_start > cursor && map_start - cursor >= page_size &&
            cursor <= high - page_size) {
            bridge = TryNearPage(target, cursor, page_size);
            if (bridge) break;
        }
        if (map_end > cursor) cursor = (map_end + mask) & ~mask;
        if (cursor >= high || cursor > high - page_size) break;
    }
    if (!bridge && cursor < high && cursor <= high - page_size)
        bridge = TryNearPage(target, cursor, page_size);
    std::fclose(maps);
    if (!bridge) return 0;
    std::uint8_t jump[16]{};
    AbsoluteJump(jump, destination);
    std::memcpy(reinterpret_cast<void*>(bridge), jump, sizeof(jump));
    __builtin___clear_cache(reinterpret_cast<char*>(bridge),
                            reinterpret_cast<char*>(bridge + sizeof(jump)));
    if (mprotect(reinterpret_cast<void*>(bridge), page_size,
                 PROT_READ | PROT_EXEC) != 0) {
        munmap(reinterpret_cast<void*>(bridge), page_size);
        return 0;
    }
    *mapped_size = page_size;
    return bridge;
}

int MappingProtectionAt(std::uintptr_t address) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return 0;
    char line[2048]{};
    int protection = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3)
            continue;
        if (address >= start && address < end) {
            if (perms[0] == 'r') protection |= PROT_READ;
            if (perms[1] == 'w') protection |= PROT_WRITE;
            if (perms[2] == 'x') protection |= PROT_EXEC;
            break;
        }
    }
    std::fclose(maps);
    return protection;
}

__attribute__((noinline)) bool AtomicSwapInstruction(
    std::uintptr_t target, std::uint32_t expected, std::uint32_t desired,
    int original_protection) {
    const long page_value = sysconf(_SC_PAGESIZE);
    if (page_value <= 0 || (target & 3u) != 0 ||
        (original_protection & PROT_READ) == 0 ||
        (original_protection & PROT_WRITE) != 0)
        return false;
    const std::size_t page_size = static_cast<std::size_t>(page_value);
    const std::uintptr_t page = target & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size,
                 original_protection | PROT_WRITE) != 0)
        return false;
    auto* instruction = reinterpret_cast<std::uint32_t*>(target);
    bool swapped = false;
    if (__atomic_load_n(instruction, __ATOMIC_ACQUIRE) == expected) {
        __atomic_store_n(instruction, desired, __ATOMIC_RELEASE);
        __builtin___clear_cache(reinterpret_cast<char*>(target),
                                reinterpret_cast<char*>(target + 4));
        swapped = __atomic_load_n(instruction, __ATOMIC_ACQUIRE) == desired;
    }
    const bool restored =
        mprotect(reinterpret_cast<void*>(page), page_size,
                 original_protection) == 0;
    return swapped && restored;
}

int ResolvePristineTarget(Mapping* game, std::uintptr_t* target,
                          std::uint32_t* original_instruction,
                          int* target_protection) {
    if (game == nullptr || target == nullptr || original_instruction == nullptr ||
        target_protection == nullptr)
        return -19;
    std::uint8_t build_id[20]{};
    if (!FindGame(game)) return -1;
    if (!ReadBuildId(game->path, build_id) ||
        std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0)
        return -2;
    *target = game->base + kTargetOffset;
    std::uint8_t signature[16]{};
    if (!ProcMem(*target, signature, sizeof(signature), false) ||
        std::memcmp(signature, kTargetSignature, sizeof(signature)) != 0)
        return -3;
    if (!ProcMem(*target, original_instruction, sizeof(*original_instruction),
                 false))
        return -4;
    *target_protection = MappingProtectionAt(*target);
    // Houdini maps the ARM64 image as host-readable data and executes a
    // translated cache elsewhere, so the guest image is normally r-- rather
    // than r-x. It must be readable and must not already be writable.
    if ((*target_protection & PROT_READ) == 0 ||
        (*target_protection & PROT_WRITE) != 0)
        return -5;
    return 0;
}

[[maybe_unused]] __attribute__((noinline)) int ProbeTargetProtection() {
    Mapping game{};
    std::uintptr_t target = 0;
    std::uint32_t original_instruction = 0;
    int target_protection = 0;
    const int resolved = ResolvePristineTarget(
        &game, &target, &original_instruction, &target_protection);
    if (resolved != 0) return resolved;
    const long page_value = sysconf(_SC_PAGESIZE);
    if (page_value <= 0) return -20;
    const std::size_t page_size = static_cast<std::size_t>(page_value);
    const std::uintptr_t page = target & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size,
                 target_protection | PROT_WRITE) != 0)
        return -21;
    std::uint32_t during = 0;
    const bool during_ok =
        ProcMem(target, &during, sizeof(during), false) &&
        during == original_instruction;
    const bool restored =
        mprotect(reinterpret_cast<void*>(page), page_size,
                 target_protection) == 0;
    std::uint32_t after = 0;
    const bool after_ok =
        restored && ProcMem(target, &after, sizeof(after), false) &&
        after == original_instruction &&
        MappingProtectionAt(target) == target_protection;
    if (!restored) return -22;
    if (!during_ok) return -23;
    if (!after_ok) return -24;
    return 0;
}

[[maybe_unused]] __attribute__((noinline)) int InstallInternal() {
    Mapping game{};
    std::uintptr_t target = 0;
    std::uint32_t original_instruction = 0;
    int target_protection = 0;
    const int resolved = ResolvePristineTarget(
        &game, &target, &original_instruction, &target_protection);
    if (resolved != 0) return resolved;
    std::memcpy(g_original, &original_instruction, sizeof(original_instruction));
    g_physics_executor_affinity_continue_v1 = BuildContinuation(target);
    if (!g_physics_executor_affinity_continue_v1) return -6;
    g_near_bridge = BuildNearBridge(
        target,
        reinterpret_cast<std::uintptr_t>(&PhysicsExecutorAffinityEntryV1),
        &g_near_bridge_size);
    if (!g_near_bridge) {
        munmap(reinterpret_cast<void*>(
                   g_physics_executor_affinity_continue_v1),
               32);
        g_physics_executor_affinity_continue_v1 = 0;
        return -7;
    }
    std::uint32_t branch_instruction = 0;
    if (!EncodeBranch(target, g_near_bridge, &branch_instruction)) {
        munmap(reinterpret_cast<void*>(g_near_bridge), g_near_bridge_size);
        munmap(reinterpret_cast<void*>(
                   g_physics_executor_affinity_continue_v1),
               32);
        g_near_bridge = 0;
        g_near_bridge_size = 0;
        g_physics_executor_affinity_continue_v1 = 0;
        return -8;
    }

    auto rollback = [&](int code, bool restore_target) {
        bool rollback_ok = true;
        if (restore_target) {
            rollback_ok = AtomicSwapInstruction(
                target, branch_instruction, original_instruction,
                target_protection);
        } else {
            const long page_value = sysconf(_SC_PAGESIZE);
            if (page_value > 0) {
                const std::uintptr_t page =
                    target & ~(static_cast<std::uintptr_t>(page_value) - 1);
                rollback_ok =
                    mprotect(reinterpret_cast<void*>(page),
                             static_cast<std::size_t>(page_value),
                             target_protection) == 0;
            }
        }
        if (g_near_bridge != 0) {
            rollback_ok =
                munmap(reinterpret_cast<void*>(g_near_bridge),
                       g_near_bridge_size) == 0 &&
                rollback_ok;
            g_near_bridge = 0;
            g_near_bridge_size = 0;
        }
        if (g_physics_executor_affinity_continue_v1 != 0) {
            rollback_ok =
                munmap(reinterpret_cast<void*>(
                           g_physics_executor_affinity_continue_v1),
                       32) == 0 &&
                rollback_ok;
            g_physics_executor_affinity_continue_v1 = 0;
        }
        g_installed.store(false, std::memory_order_release);
        return rollback_ok ? code : -200;
    };

    if (!AtomicSwapInstruction(target, original_instruction,
                               branch_instruction, target_protection)) {
        std::uint32_t observed = 0;
        const bool read_ok = ProcMem(target, &observed, sizeof(observed), false);
        return rollback(-9, read_ok && observed == branch_instruction);
    }
    std::uint32_t verify = 0;
    if (!ProcMem(target, &verify, sizeof(verify), false) ||
        verify != branch_instruction) {
        return rollback(-10, true);
    }
    g_report.header.guest_base = game.base;
    g_report.header.target = target;
    g_control.reserved[0] = g_near_bridge;
    g_control.reserved[1] = g_physics_executor_affinity_continue_v1;
    g_control.reserved[2] = sizeof(branch_instruction);
    __atomic_fetch_or(&g_report.header.flags,
                      kFlagInstalled | kFlagAtomicBranchPatch,
                      __ATOMIC_RELEASE);
    g_installed.store(true, std::memory_order_release);
    return 0;
}

__attribute__((constructor)) void OnLoad() {
    InitializeReport();
    g_status.store(1, std::memory_order_release);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "loaded passive=%d runtime_arming_compiled=%d hooks=0 threads=0 calls=0",
        kRuntimeArmingCompiled ? 0 : 1, kRuntimeArmingCompiled ? 1 : 0);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_physics_executor_affinity_v1_protocol() {
    return 1;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_physics_executor_affinity_v1_status() {
    return g_status.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_physics_executor_affinity_v1_report() {
    return reinterpret_cast<std::uintptr_t>(&g_report);
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_physics_executor_affinity_v1_report_size() {
    return sizeof(g_report);
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_physics_executor_affinity_v1_arm(void*, void*) {
#if A9TAS_PHYS_EXEC_AFFINITY_RUNTIME_ARMING == 0
    return -100;
#else
    std::uint32_t expected = 0;
    if (g_arm_phase.compare_exchange_strong(
            expected, 1u, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // Stage 1 is a zero-hook address-transport proof. NativeBridge on the
        // target accepts an int32 return shorty but rejects a 64-bit one, so
        // publish the low and high halves on two ordered calls. The controller
        // must reconstruct and validate both payload-owned ranges before the
        // third call can install anything.
        InitializeReport();
        g_control.reserved[3] = reinterpret_cast<std::uintptr_t>(&g_report);
        g_status.store(1, std::memory_order_release);
        g_arm_phase.store(2u, std::memory_order_release);
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(&g_control)));
    }
    expected = 2u;
    if (g_arm_phase.compare_exchange_strong(
            expected, 3u, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(&g_control) >> 32u));
    }
    expected = 3u;
    if (g_arm_phase.compare_exchange_strong(
            expected, 4u, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        const std::int32_t permission_result = ProbeTargetProtection();
        if (permission_result != 0) {
            g_status.store(permission_result, std::memory_order_release);
            g_arm_phase.store(99u, std::memory_order_release);
            return permission_result;
        }
        g_status.store(3, std::memory_order_release);
        g_arm_phase.store(5u, std::memory_order_release);
        return 0;
    }
    expected = 5u;
    if (!g_arm_phase.compare_exchange_strong(
            expected, 6u, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return -101;
    }
    const char control_magic[8] = {'A', '9', 'P', 'E', 'C', '1', 0, 0};
    if (std::memcmp(g_control.magic, control_magic, sizeof(control_magic)) != 0 ||
        g_control.version != 1 || g_control.size != sizeof(Control) ||
        g_control.capture_enabled != 0 || g_control.completed != 0 ||
        g_control.active_captures != 0 ||
        g_control.capture_limit != kDefaultCaptureLimit ||
        g_control.reserved[0] != 0 || g_control.reserved[1] != 0 ||
        g_control.reserved[2] != 0 ||
        g_control.reserved[3] !=
            reinterpret_cast<std::uintptr_t>(&g_report)) {
        g_status.store(-11, std::memory_order_release);
        return -102;
    }
    g_status.store(-10, std::memory_order_release);
    const std::int32_t result = InstallInternal();
    if (result != 0) {
        g_status.store(result, std::memory_order_release);
        return result;
    }
    __atomic_store_n(&g_control.completed, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.active_captures, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_control.capture_enabled, 1u, __ATOMIC_RELEASE);
    g_status.store(2, std::memory_order_release);
    return 0;
#endif
}

// Data exports let the x86_64 host controller locate payload-owned state
// without issuing another guest call.  They contain pointers, not game state.
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_executor_affinity_v1_report_storage =
        reinterpret_cast<std::uintptr_t>(&g_report);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_executor_affinity_v1_control_storage =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_physics_executor_affinity_v1_control_size = sizeof(g_control);
