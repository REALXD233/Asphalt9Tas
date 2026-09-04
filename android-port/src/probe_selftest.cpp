// probe_selftest.cpp v8 — Design G: compiled probe with the copied game
// prologue embedded as inline asm, no naked code blocks anywhere in the chain.
// Isolated process; the game process is never touched. stderr logging only.
//
// Established facts (v1-v7):
//  - Houdini mangles integer register delivery at naked-function entries
//    whose first instructions access memory (str/ldp); pure-register naked
//    entries (CallIndirect) work; compiled entries work via bl AND br.
//  - mprotect(+W) crashes the process on pages with existing translations;
//    works on never-executed pages (file-backed and anonymous).
//  - /proc/self/mem pwrite can patch a read-only page; self-ptrace is EPERM.
//
// DummyT mirrors the game's 0x38b7930 exactly: same 16-byte prologue
// (kDispatchSignature bytes), body consumes only x0/x1 (as the real function
// does at +16): result = pair[0]+pair[1]+frame = 316 with pair={100,200},
// frame=16.
//
// ProbeFuncG (Design G): compiled entry (args delivered correctly via br),
// then a single asm block:
//   1. replay the copied 16-byte prologue stores (saves ORIGINAL x19-x22/x30
//      into the game frame before anything clobbers them),
//   2. park context/frame in x19/x20 (the game body overwrites both at +16
//      before the epilogue, which restores from the frame),
//   3. bl ProbeImpl (count),
//   4. restore x0/x1, re-run the 4th prologue instruction (ldp x8,x9,[x0]),
//   5. absolute jump to target+16 via g_trampoline_target (GOT).
// The game body's epilogue then returns directly to the ORIGINAL caller.
//
// Modes:
//   plain   : compiled control (325)
//   dummyT  : DummyT direct bl (known broken under Houdini — reference)
//   tramp2  : raw naked trampoline (DummyT prologue copy) called via blr
//   probeG  : full Design G chain via /proc/self/mem patch
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
std::atomic<std::uint64_t> g_calls{0};
std::uintptr_t g_trampoline_target = 0;  // target+16, set by installer
}

#define LOG(fmt, ...) \
    do { std::fprintf(stderr, "[selftest] " fmt "\n", ##__VA_ARGS__); } while (0)

constexpr std::uint8_t kDispatchSignature[16] = {
    0xf6, 0x0f, 0x1d, 0xf8, 0xf5, 0x53, 0x01, 0xa9,
    0xf3, 0x7b, 0x02, 0xa9, 0x08, 0x24, 0x40, 0xa9,
};

extern "C" __attribute__((noinline)) void ProbeImpl(void*, const void*) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
}

// Design G probe. See file header. No naked entry, no separate trampoline
// block: the copied prologue and the absolute jump live inside this compiled
// function. The asm must be the function's only content and the clobber list
// must NOT include x19/x20/x30: the function is a leaf, so the compiler
// allocates no frame and never touches sp — the game prologue frame is the
// only frame, and the game epilogue's `ret` returns to the original caller
// with the stack perfectly balanced.
extern "C" __attribute__((noinline)) void ProbeFuncG(void* context,
                                                     const void* frame) {
    register void* ctx __asm__("x0") = context;
    register const void* frm __asm__("x1") = frame;
    __asm__ volatile(
        // 1. copied game prologue bytes 0-11 (x19-x22/x30 still original here)
        "str x22, [sp, #-0x30]!\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        // 2. park context/frame in callee-saved regs (game body overwrites
        //    both at +16 before the epilogue restores them from the frame)
        "mov x19, x0\n"
        "mov x20, x1\n"
        // 3. count
        "bl ProbeImpl\n"
        // 4. restore context/frame, re-run the 4th prologue instruction
        "mov x0, x19\n"
        "mov x1, x20\n"
        "ldp x8, x9, [x0]\n"
        // 5. absolute jump to target+16
        "adrp x17, :got:g_trampoline_target\n"
        "ldr x17, [x17, #:got_lo12:g_trampoline_target]\n"
        "ldr x17, [x17]\n"
        "br x17\n"
        :: "r"(ctx), "r"(frm)
        : "x8", "x9", "x17", "cc", "memory");
}

// DummyT — game-shaped target. Prologue bytes == kDispatchSignature.
// Args: x0=pair (2 u64), x1=frame. result = pair0+pair1+frame = 316.
extern "C" __attribute__((naked)) std::uint64_t DummyT(
    const std::uint64_t* pair, std::uint64_t frame) {
    __asm__ volatile(
        "str x22, [sp, #-0x30]!\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "ldp x8, x9, [x0]\n"
        "mov x20, x1\n"
        "add x8, x8, x9\n"
        "add x8, x8, x20\n"
        "mov x0, x8\n"
        "ldp x19, x30, [sp, #0x20]\n"
        "ldp x21, x20, [sp, #0x10]\n"
        "ldr x22, [sp], #0x30\n"
        "ret\n");
}

extern "C" __attribute__((noinline)) std::uint64_t PlainFunc(
    const std::uint64_t* pair, std::uint64_t frame, std::uint64_t a,
    std::uint64_t b, double c) {
    return pair[0] + pair[1] + frame + a + b + static_cast<std::uint64_t>(c);
}

void WriteAbsoluteJump(std::uint8_t* destination, std::uintptr_t target) {
    constexpr std::uint32_t load_x17_literal = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xD61F0220;
    std::memcpy(destination, &load_x17_literal, sizeof(load_x17_literal));
    std::memcpy(destination + 4, &branch_x17, sizeof(branch_x17));
    std::memcpy(destination + 8, &target, sizeof(target));
}

bool WriteProcMem(std::uint8_t* dst, const std::uint8_t* src) {
    const int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) {
        LOG("open /proc/self/mem failed errno=%d", errno);
        return false;
    }
    const ssize_t n = pwrite(fd, src, 16, reinterpret_cast<off_t>(dst));
    close(fd);
    if (n != 16) {
        LOG("pwrite=%zd errno=%d", n, errno);
        return false;
    }
    return true;
}

// Design G install: write the entry patch via /proc/self/mem (no mprotect).
bool InstallG(std::uint8_t* target, void (*probe)(void*, const void*)) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    g_trampoline_target = reinterpret_cast<std::uintptr_t>(target + 16);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(probe));
    if (!WriteProcMem(target, patch)) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    LOG("patched %p readback=%02x%02x%02x%02x %02x%02x%02x%02x target16=%p",
        target, back[0], back[1], back[2], back[3], back[4], back[5],
        back[6], back[7], reinterpret_cast<void*>(g_trampoline_target));
    return true;
}

static constexpr std::uint64_t kExpectPlain = 325;
static constexpr std::uint64_t kExpectT = 316;

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "plain";
    std::uint64_t pair[2] = {100, 200};
    LOG("mode=%s pid=%d", mode, getpid());

    if (std::strcmp(mode, "plain") == 0) {
        const std::uint64_t r = PlainFunc(pair, 16, 2, 3, 4.0);
        LOG("plain result=%llu (expect %llu)", static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(kExpectPlain));
        return 0;
    }

    if (std::strcmp(mode, "dummyT") == 0) {
        const std::uint64_t r = DummyT(pair, 16);
        LOG("dummyT result=%llu (expect %llu)", static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(kExpectT));
        return 0;
    }

    if (std::strcmp(mode, "tramp2") == 0) {
        const auto* target = reinterpret_cast<const std::uint8_t*>(&DummyT);
        auto* trampoline = static_cast<std::uint8_t*>(
            mmap(nullptr, 32, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (trampoline == MAP_FAILED) {
            LOG("mmap failed");
            return 1;
        }
        std::memcpy(trampoline, target, 16);
        WriteAbsoluteJump(trampoline + 16,
                          reinterpret_cast<std::uintptr_t>(target + 16));
        __builtin___clear_cache(reinterpret_cast<char*>(trampoline),
                                reinterpret_cast<char*>(trampoline + 32));
        if (mprotect(trampoline, 32, PROT_READ) != 0) {
            LOG("trampoline mprotect(R) failed errno=%d", errno);
        }
        LOG("trampoline=%p first16=%02x%02x%02x%02x %02x%02x%02x%02x",
            trampoline, target[0], target[1], target[2], target[3],
            target[4], target[5], target[6], target[7]);
        using Fn = std::uint64_t (*)(const std::uint64_t*, std::uint64_t);
        Fn fn = reinterpret_cast<Fn>(trampoline);
        const std::uint64_t r = fn(pair, 16);
        LOG("tramp2 result=%llu (expect %llu)", static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(kExpectT));
        return 0;
    }

    if (std::strcmp(mode, "probeG") == 0) {
        auto* target = reinterpret_cast<std::uint8_t*>(&DummyT);
        if (!InstallG(target, &ProbeFuncG)) {
            LOG("INSTALL FAILED");
            return 1;
        }
        const std::uint64_t r = DummyT(pair, 16);
        LOG("probeG result=%llu calls=%llu (expect %llu 1)",
            static_cast<unsigned long long>(r),
            static_cast<unsigned long long>(g_calls.load()),
            static_cast<unsigned long long>(kExpectT));
        return 0;
    }

    if (std::strcmp(mode, "lateG") == 0) {
        // Call the target BEFORE installing (Houdini translates the original
        // entry), then install, then call again. calls==1 => Houdini re-read
        // the patched bytes; calls==0 => cached translation reused (patch
        // invisible) — the game's timing requirement in a nutshell.
        const std::uint64_t before = DummyT(pair, 16);
        LOG("lateG first result=%llu calls=%llu", static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(g_calls.load()));
        auto* target = reinterpret_cast<std::uint8_t*>(&DummyT);
        if (!InstallG(target, &ProbeFuncG)) {
            LOG("INSTALL FAILED");
            return 1;
        }
        const std::uint64_t after = DummyT(pair, 16);
        LOG("lateG second result=%llu calls=%llu (calls==1 => re-read; calls==0 => cache)",
            static_cast<unsigned long long>(after),
            static_cast<unsigned long long>(g_calls.load()));
        return 0;
    }

    LOG("unknown mode %s", mode);
    return 2;
}
