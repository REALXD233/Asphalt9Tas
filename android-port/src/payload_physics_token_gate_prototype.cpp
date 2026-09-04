// Build-only prototype for an in-guest ARM64 physics-token gate.
//
// SAFETY: runtime arming is intentionally compiled out. Loading this library
// only logs that it is passive. It cannot patch the game until a later reviewed
// build explicitly changes kRuntimeArmingCompiled and calls the exported arm
// function from a non-persistent, manually controlled loader.
#include "physics_token_gate.h"

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kTag = "A9TAS_TOKEN_GATE_PROTO";
constexpr bool kRuntimeArmingCompiled = false;
constexpr std::uintptr_t kTargetOffset = 0x38B74DC;
constexpr std::uint8_t kTargetSignature[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
    0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
};
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

struct Mapping {
    std::uintptr_t base{};
    char path[1024]{};
};

a9tas::PhysicsTokenGate g_gate;
thread_local std::uint64_t g_shadow_token_us = 0;
std::atomic<bool> g_installed{false};
std::atomic<std::uintptr_t> g_target{0};
std::uint8_t g_original[16]{};
extern "C" std::uintptr_t g_token_gate_continue = 0;

bool FindGame(Mapping* result) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start{}, end{}, offset{};
        char perms[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
            &start, &end, perms, &offset, path);
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
    constexpr std::uint32_t load_x17 = 0x58000051;  // ldr x17, #8
    constexpr std::uint32_t branch_x17 = 0xd61f0220;  // br x17
    std::memcpy(output, &load_x17, 4);
    std::memcpy(output + 4, &branch_x17, 4);
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

std::uintptr_t BuildContinuation(std::uintptr_t target) {
    auto* code = static_cast<std::uint8_t*>(mmap(
        nullptr, 32, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
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

extern "C" __attribute__((noinline)) const std::uint64_t* FilterToken(
    const std::uint64_t* real_token) {
    const std::uint64_t value = real_token ? *real_token : 0;
    const auto decision = g_gate.Filter(value);
    g_shadow_token_us = decision.token_us;
    return &g_shadow_token_us;
}

// Uses a temporary hook-only frame, restores the incoming machine state (except
// the intentionally substituted X1 token pointer), then tail-branches to the
// trampoline. The trampoline — and only the trampoline — replays the exact
// overwritten 16-byte original prologue. This avoids the old probe bug where
// both the hook and trampoline replayed the prologue and allocated two frames.
// FilterToken returns per-thread TLS, so X1 stays valid in the original body.
extern "C" __attribute__((naked)) void PhysicsTokenGateEntry() {
    __asm__ volatile(
        "sub sp, sp, #0x30\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov x20, x8\n"
        "mov x21, x0\n"
        "mov x19, x1\n"
        "mov x0, x19\n"
        "bl FilterToken\n"
        "mov x1, x0\n"
        "mov x0, x21\n"
        "mov x8, x20\n"
        "ldp x19, x30, [sp, #0x20]\n"
        "ldp x21, x20, [sp, #0x10]\n"
        "add sp, sp, #0x30\n"
        "adrp x17, :got:g_token_gate_continue\n"
        "ldr x17, [x17, #:got_lo12:g_token_gate_continue]\n"
        "ldr x17, [x17]\n"
        "br x17\n");
}

int ArmInternal() {
    Mapping game{};
    std::uint8_t build_id[20]{};
    if (!FindGame(&game)) return -1;
    if (!ReadBuildId(game.path, build_id) ||
        std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0) return -2;
    auto* target = reinterpret_cast<std::uint8_t*>(game.base + kTargetOffset);
    if (std::memcmp(target, kTargetSignature, sizeof(kTargetSignature)) != 0)
        return -3;
    if (!ProcMem(reinterpret_cast<std::uintptr_t>(target), g_original,
                 sizeof(g_original), false)) return -4;
    g_token_gate_continue = BuildContinuation(
        reinterpret_cast<std::uintptr_t>(target));
    if (!g_token_gate_continue) return -5;

    auto rollback = [&](int code, bool restore_target) {
        if (restore_target) {
            ProcMem(reinterpret_cast<std::uintptr_t>(target), g_original,
                    sizeof(g_original), true);
            __builtin___clear_cache(reinterpret_cast<char*>(target),
                                    reinterpret_cast<char*>(target + sizeof(g_original)));
        }
        if (g_token_gate_continue) {
            munmap(reinterpret_cast<void*>(g_token_gate_continue), 32);
            g_token_gate_continue = 0;
        }
        g_target.store(0, std::memory_order_release);
        g_installed.store(false, std::memory_order_release);
        return code;
    };

    std::uint8_t patch[16]{};
    AbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&PhysicsTokenGateEntry));
    if (!ProcMem(reinterpret_cast<std::uintptr_t>(target), patch, sizeof(patch), true))
        return rollback(-6, false);
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(patch)));
    std::uint8_t verify[16]{};
    if (!ProcMem(reinterpret_cast<std::uintptr_t>(target), verify,
                 sizeof(verify), false) ||
        std::memcmp(verify, patch, sizeof(patch)) != 0) {
        return rollback(-7, true);
    }
    g_target.store(reinterpret_cast<std::uintptr_t>(target),
                   std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return 0;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 runtime_arming_compiled=%d",
                        kRuntimeArmingCompiled ? 1 : 0);
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int a9tas_token_gate_arm() {
    if (!kRuntimeArmingCompiled) return -100;
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        return -101;
    }
    const int rc = ArmInternal();
    if (rc != 0) g_installed.store(false, std::memory_order_release);
    return rc;
}

extern "C" __attribute__((visibility("default"))) int a9tas_token_gate_freeze() {
    if (!g_installed.load(std::memory_order_acquire)) return -1;
    g_gate.Freeze();
    return 0;
}

extern "C" __attribute__((visibility("default"))) int a9tas_token_gate_step(
    std::uint32_t count) {
    if (!g_installed.load(std::memory_order_acquire)) return -1;
    return g_gate.RequestSteps(count) ? 0 : -2;
}

extern "C" __attribute__((visibility("default"))) int a9tas_token_gate_resume() {
    if (!g_installed.load(std::memory_order_acquire)) return -1;
    g_gate.Resume();
    return 0;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_token_gate_protocol() { return 4; }
