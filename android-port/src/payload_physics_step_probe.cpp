// Minimal, observation-only probe for the confirmed PhysicsContext simulation
// token executor. It never suppresses, changes, or delays a token.
#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_PHYS_STEP";
#ifndef A9TAS_PHYS_STEP_ENABLE
#define A9TAS_PHYS_STEP_ENABLE "/data/local/tmp/a9tas-enable-physics-step-probe"
#endif
#ifndef A9TAS_PHYS_STEP_STATUS
#define A9TAS_PHYS_STEP_STATUS \
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-physics-step-status"
#endif
#ifndef A9TAS_PHYS_STEP_OUTPUT
#define A9TAS_PHYS_STEP_OUTPUT \
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-physics-step.bin"
#endif
constexpr const char* kEnable = A9TAS_PHYS_STEP_ENABLE;
constexpr const char* kStatus = A9TAS_PHYS_STEP_STATUS;
constexpr const char* kOutput = A9TAS_PHYS_STEP_OUTPUT;
constexpr std::uintptr_t kTargetOffset = 0x38B74DC;
constexpr std::uint8_t kSignature[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
    0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
};
constexpr std::uint8_t kBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};
constexpr std::uint32_t kCapacity = 8192;

struct Mapping {
    std::uintptr_t base{};
    char path[1024]{};
};

struct Record {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::uint64_t token_us;
    std::uint64_t context;
    std::uint64_t output;
    std::uint32_t tid;
    std::uint32_t reserved;
};

struct Header {
    char magic[8];
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint64_t guest_base;
    std::uint64_t events;
    std::uint32_t record_count;
    std::uint32_t dropped;
};

extern "C" std::uintptr_t g_physics_step_continue = 0;
std::atomic<std::uintptr_t> g_base{0};
std::atomic<std::uint64_t> g_events{0};
std::atomic<std::uint32_t> g_records{0};
std::atomic<std::uint32_t> g_dropped{0};
std::atomic<std::uint64_t> g_last_token{0};
std::atomic<std::uint64_t> g_first_ns{0};
std::atomic<std::uint64_t> g_last_ns{0};
std::atomic<bool> g_installed{false};
Record g_ring[kCapacity]{};

std::uint64_t MonotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool FindGame(Mapping* result) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{}, path[1024]{};
        const int n = std::sscanf(line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                  &start, &end, perms, &offset, path);
        if (n == 5 && offset == 0 && std::strstr(path, "libAsphalt9.so")) {
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

bool ReadBuildId(const char* path, std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Elf64_Ehdr header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_machine != EM_AARCH64) {
        std::fclose(file);
        return false;
    }
    bool found = false;
    for (std::uint16_t i = 0; i < header.e_phnum && !found; ++i) {
        Elf64_Phdr ph{};
        std::fseek(file, static_cast<long>(header.e_phoff) +
                           static_cast<long>(i) * sizeof(ph), SEEK_SET);
        if (std::fread(&ph, sizeof(ph), 1, file) != 1) break;
        if (ph.p_type != PT_NOTE || ph.p_filesz > 1024 * 1024) continue;
        std::uint64_t cursor = ph.p_offset;
        const std::uint64_t end = ph.p_offset + ph.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            std::fseek(file, static_cast<long>(cursor), SEEK_SET);
            if (std::fread(&note, sizeof(note), 1, file) != 1) break;
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

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination) {
    constexpr std::uint32_t load = 0x58000051;  // ldr x17, #8
    constexpr std::uint32_t branch = 0xd61f0220;  // br x17
    std::memcpy(output, &load, 4);
    std::memcpy(output + 4, &branch, 4);
    std::memcpy(output + 8, &destination, 8);
}

bool ProcMem(std::uintptr_t address, void* buffer, std::size_t size, bool write) {
    const int fd = open("/proc/self/mem", (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t done = write
        ? pwrite(fd, buffer, size, static_cast<off_t>(address))
        : pread(fd, buffer, size, static_cast<off_t>(address));
    close(fd);
    return done == static_cast<ssize_t>(size);
}

std::uintptr_t BuildContinue(std::uintptr_t target) {
    auto* code = static_cast<std::uint8_t*>(mmap(
        nullptr, 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return 0;
    std::memcpy(code, reinterpret_cast<const void*>(target), 16);
    AbsoluteJump(code + 16, target + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + 32));
    if (mprotect(code, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(code);
}

extern "C" __attribute__((noinline)) void PhysicsStepCapture(
    void* context, const std::uint64_t* token, void* output) {
    const std::uint64_t sequence = g_events.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t now = MonotonicNs();
    if (sequence == 0) g_first_ns.store(now, std::memory_order_relaxed);
    g_last_ns.store(now, std::memory_order_relaxed);
    const std::uint64_t value = token ? *token : 0;
    g_last_token.store(value, std::memory_order_relaxed);
    const std::uint32_t index = g_records.fetch_add(1, std::memory_order_relaxed);
    if (index < kCapacity) {
        g_ring[index] = {sequence, now, value,
                         reinterpret_cast<std::uintptr_t>(context),
                         reinterpret_cast<std::uintptr_t>(output),
                         static_cast<std::uint32_t>(syscall(SYS_gettid)), 0};
    } else {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// Uses a temporary hook-only frame, then restores the incoming state and lets
// the trampoline replay the overwritten prologue exactly once. The historical
// implementation replayed the prologue here and again in BuildContinue(),
// allocating two original frames; that implementation is forbidden.
extern "C" __attribute__((naked)) void PhysicsStepProbe() {
    __asm__ volatile(
        "sub sp, sp, #0x30\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov x20, x8\n"
        "mov x21, x0\n"
        "mov x19, x1\n"
        "mov x2, x20\n"
        "bl PhysicsStepCapture\n"
        "mov x0, x21\n"
        "mov x1, x19\n"
        "mov x8, x20\n"
        "ldp x19, x30, [sp, #0x20]\n"
        "ldp x21, x20, [sp, #0x10]\n"
        "add sp, sp, #0x30\n"
        "adrp x17, :got:g_physics_step_continue\n"
        "ldr x17, [x17, #:got_lo12:g_physics_step_continue]\n"
        "ldr x17, [x17]\n"
        "br x17\n"
        ::: "x0", "x1", "x2", "x8", "x17", "x19", "x20", "x21",
            "x30", "cc", "memory");
}

bool Dump() {
    std::uint32_t count = g_records.load(std::memory_order_acquire);
    if (count > kCapacity) count = kCapacity;
    FILE* file = std::fopen(kOutput, "wb");
    if (!file) return false;
    Header h{{'A','9','P','S','T','E','P','1'}, {}, sizeof(Record),
             g_base.load(std::memory_order_relaxed),
             g_events.load(std::memory_order_relaxed), count,
             g_dropped.load(std::memory_order_relaxed)};
    std::memcpy(h.build_id, kBuildId, sizeof(kBuildId));
    const bool ok = std::fwrite(&h, sizeof(h), 1, file) == 1 &&
                    (count == 0 || std::fwrite(g_ring, sizeof(Record), count, file) == count);
    std::fclose(file);
    return ok;
}

void Status(const char* state) {
    const auto events = g_events.load(std::memory_order_relaxed);
    const auto first = g_first_ns.load(std::memory_order_relaxed);
    const auto last = g_last_ns.load(std::memory_order_relaxed);
    const double hz = events > 1 && last > first
        ? static_cast<double>(events - 1) * 1.0e9 / static_cast<double>(last - first)
        : 0.0;
    FILE* file = std::fopen(kStatus, "w");
    if (!file) return;
    std::fprintf(file,
                 "state=%s installed=%d base=0x%" PRIxPTR
                 " events=%" PRIu64 " records=%u dropped=%u last_token_us=%" PRIu64
                 " observed_hz=%.6f\n",
                 state, g_installed.load() ? 1 : 0,
                 g_base.load(), events, g_records.load(), g_dropped.load(),
                 g_last_token.load(), hz);
    std::fclose(file);
}

bool Install(std::uintptr_t base) {
    auto* target = reinterpret_cast<std::uint8_t*>(base + kTargetOffset);
    if (std::memcmp(target, kSignature, sizeof(kSignature)) != 0) return false;
    std::uint8_t original[16]{};
    if (!ProcMem(reinterpret_cast<std::uintptr_t>(target), original, 16, false)) return false;
    g_physics_step_continue = BuildContinue(reinterpret_cast<std::uintptr_t>(target));
    if (!g_physics_step_continue) return false;
    std::uint8_t patch[16]{};
    AbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&PhysicsStepProbe));
    if (!ProcMem(reinterpret_cast<std::uintptr_t>(target), patch, 16, true)) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t verify[16]{};
    if (!ProcMem(reinterpret_cast<std::uintptr_t>(target), verify, 16, false) ||
        std::memcmp(verify, patch, 16) != 0) {
        ProcMem(reinterpret_cast<std::uintptr_t>(target), original, 16, true);
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    return true;
}

void* Worker(void*) {
    Mapping game{};
    for (int i = 0; i < 3000 && !FindGame(&game); ++i) usleep(10000);
    std::uint8_t build[20]{};
    if (!game.base || access(kEnable, F_OK) != 0 ||
        !ReadBuildId(game.path, build) || std::memcmp(build, kBuildId, 20) != 0) {
        Status("precondition_failed");
        return nullptr;
    }
    g_base.store(game.base, std::memory_order_release);
    if (!Install(game.base)) {
        Status("install_failed");
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "installed target=%p continuation=%p",
                        reinterpret_cast<void*>(game.base + kTargetOffset),
                        reinterpret_cast<void*>(g_physics_step_continue));
    while (true) {
        sleep(1);
        Dump();
        Status("observing");
    }
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 observation_only=1");
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, Worker, nullptr) == 0)
        pthread_detach(thread);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() { return 3; }
