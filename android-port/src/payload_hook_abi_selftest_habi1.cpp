#include "physics_token_gate.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef A9TAS_HABI1_SOURCE_SHA256
#define A9TAS_HABI1_SOURCE_SHA256 UNSET
#endif
#ifndef A9TAS_HABI1_HEADER_SHA256
#define A9TAS_HABI1_HEADER_SHA256 UNSET
#endif
#ifndef A9TAS_HABI1_EXPECTED_PAYLOAD_PATH
#define A9TAS_HABI1_EXPECTED_PAYLOAD_PATH \
    /data/local/tmp/liba9tas_hook_abi_selftest_habi1.so
#endif

#define A9TAS_HABI1_STRINGIFY_INNER(value) #value
#define A9TAS_HABI1_STRINGIFY(value) A9TAS_HABI1_STRINGIFY_INNER(value)

namespace {
constexpr const char* kTag = "A9TAS_HABI1";
constexpr const char* kStatusPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/cache/"
    "a9tas-hook-abi-selftest-habi1.status";
constexpr const char* kStatusDirectory =
    "/data/user/0/com.aligames.kuang.kybc.aligames/cache";
constexpr const char* kSourceSha256 =
    A9TAS_HABI1_STRINGIFY(A9TAS_HABI1_SOURCE_SHA256);
constexpr const char* kHeaderSha256 =
    A9TAS_HABI1_STRINGIFY(A9TAS_HABI1_HEADER_SHA256);
constexpr const char* kExpectedPayloadPath =
    A9TAS_HABI1_STRINGIFY(A9TAS_HABI1_EXPECTED_PAYLOAD_PATH);
constexpr std::size_t kPatchSize = 16;
constexpr std::size_t kTrampolineCodeSize = 32;
constexpr std::uint32_t kRunPassTag = 0x48414201;
constexpr std::uint32_t kRunAlreadyPassedTag = 0x48414202;
constexpr std::uint32_t kRunAlreadyFailedTag = 0x48414203;
constexpr std::uint32_t kRunFailedTag = 0x48414210;
constexpr std::uint32_t kRunReceiptFailedTag = 0x48414211;
constexpr int kLogicalCodeProtection = PROT_READ | PROT_EXEC;

constexpr std::uint32_t kExpectedPrologueWords[4] = {
    0xd10183ff,  // sub sp, sp, #0x60
    0xa90453f5,  // stp x21, x20, [sp, #0x40]
    0xa9057bf3,  // stp x19, x30, [sp, #0x50]
    0xaa0803f4,  // mov x20, x8
};
constexpr std::uint8_t kExpectedPrologue[kPatchSize] = {
    0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
    0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
};

struct alignas(16) ProbeContext {
    std::uint64_t expected_gpr[31]{};
    std::uint64_t observed_gpr[31]{};
    alignas(16) std::uint8_t expected_simd[32][16]{};
    alignas(16) std::uint8_t observed_simd[32][16]{};
    std::uint64_t expected_nzcv{};
    std::uint64_t observed_nzcv{};
    std::uint64_t expected_fpcr{};
    std::uint64_t observed_fpcr{};
    std::uint64_t expected_fpsr{};
    std::uint64_t observed_fpsr{};
    std::uint64_t target_entry_sp{};
    std::uint64_t target_body_sp{};
    std::uint64_t target_prologue_count{};
};

static_assert(offsetof(ProbeContext, expected_gpr) == 0x000);
static_assert(offsetof(ProbeContext, observed_gpr) == 0x0f8);
static_assert(offsetof(ProbeContext, expected_simd) == 0x1f0);
static_assert(offsetof(ProbeContext, observed_simd) == 0x3f0);
static_assert(offsetof(ProbeContext, expected_nzcv) == 0x5f0);
static_assert(offsetof(ProbeContext, observed_nzcv) == 0x5f8);
static_assert(offsetof(ProbeContext, expected_fpcr) == 0x600);
static_assert(offsetof(ProbeContext, observed_fpcr) == 0x608);
static_assert(offsetof(ProbeContext, expected_fpsr) == 0x610);
static_assert(offsetof(ProbeContext, observed_fpsr) == 0x618);
static_assert(offsetof(ProbeContext, target_entry_sp) == 0x620);
static_assert(offsetof(ProbeContext, target_body_sp) == 0x628);
static_assert(offsetof(ProbeContext, target_prologue_count) == 0x630);
static_assert(sizeof(ProbeContext) == 0x640);

a9tas::PhysicsTokenGate g_gate;
thread_local std::uint64_t g_shadow_token{};
std::atomic<std::uint64_t> g_hook_calls{};
std::atomic<std::uint32_t> g_filter_sp_mod16{0xff};
std::atomic<bool> g_installed{false};
std::atomic<int> g_run_state{0};  // 0=not-run, 1=running, 2=pass, 3=fail

extern "C" {
__attribute__((visibility("hidden")))
std::uintptr_t g_hook_abi_selftest_continue = 0;
}

// The target starts on its own page. A second page-aligned symbol below forces
// the linker section to reserve a complete page for the target and padding.
extern "C" __attribute__((naked, noinline, aligned(4096),
                          section(".text.a9tas_habi1_target"),
                          visibility("hidden"))) void
HookAbiSelftestTarget() {
    __asm__ volatile(
        "sub sp, sp, #0x60\n"
        "stp x21, x20, [sp, #0x40]\n"
        "stp x19, x30, [sp, #0x50]\n"
        "mov x20, x8\n"
        "mov x21, x0\n"
        "stp x0, x1, [x21, #0x0f8]\n"
        "stp x2, x3, [x21, #0x108]\n"
        "stp x4, x5, [x21, #0x118]\n"
        "stp x6, x7, [x21, #0x128]\n"
        "stp x8, x9, [x21, #0x138]\n"
        "stp x10, x11, [x21, #0x148]\n"
        "stp x12, x13, [x21, #0x158]\n"
        "stp x14, x15, [x21, #0x168]\n"
        "stp x16, x17, [x21, #0x178]\n"
        "str x18, [x21, #0x188]\n"
        "ldr x10, [sp, #0x50]\n"
        "str x10, [x21, #0x190]\n"
        "ldr x10, [sp, #0x48]\n"
        "str x10, [x21, #0x198]\n"
        "ldr x10, [sp, #0x40]\n"
        "str x10, [x21, #0x1a0]\n"
        "stp x22, x23, [x21, #0x1a8]\n"
        "stp x24, x25, [x21, #0x1b8]\n"
        "stp x26, x27, [x21, #0x1c8]\n"
        "stp x28, x29, [x21, #0x1d8]\n"
        "ldr x10, [sp, #0x58]\n"
        "str x10, [x21, #0x1e8]\n"
        "add x10, x21, #0x3f0\n"
        "stp q0, q1, [x10, #0x000]\n"
        "stp q2, q3, [x10, #0x020]\n"
        "stp q4, q5, [x10, #0x040]\n"
        "stp q6, q7, [x10, #0x060]\n"
        "stp q8, q9, [x10, #0x080]\n"
        "stp q10, q11, [x10, #0x0a0]\n"
        "stp q12, q13, [x10, #0x0c0]\n"
        "stp q14, q15, [x10, #0x0e0]\n"
        "stp q16, q17, [x10, #0x100]\n"
        "stp q18, q19, [x10, #0x120]\n"
        "stp q20, q21, [x10, #0x140]\n"
        "stp q22, q23, [x10, #0x160]\n"
        "stp q24, q25, [x10, #0x180]\n"
        "stp q26, q27, [x10, #0x1a0]\n"
        "stp q28, q29, [x10, #0x1c0]\n"
        "stp q30, q31, [x10, #0x1e0]\n"
        "mrs x10, nzcv\n"
        "str x10, [x21, #0x5f8]\n"
        "mrs x10, fpcr\n"
        "str x10, [x21, #0x608]\n"
        "mrs x10, fpsr\n"
        "str x10, [x21, #0x618]\n"
        "mov x10, sp\n"
        "str x10, [x21, #0x628]\n"
        "ldr x10, [x21, #0x630]\n"
        "add x10, x10, #1\n"
        "str x10, [x21, #0x630]\n"
        "mov x19, x1\n"
        "ldr x9, [x19]\n"
        "str x9, [x20]\n"
        "add x0, x9, #7\n"
        "ldp x19, x30, [sp, #0x50]\n"
        "ldp x21, x20, [sp, #0x40]\n"
        "add sp, sp, #0x60\n"
        "ret\n");
}

extern "C" __attribute__((naked, noinline, aligned(4096),
                          section(".text.a9tas_habi1_target"),
                          visibility("hidden"))) void
HookAbiSelftestTargetPageEnd() {
    __asm__ volatile("ret\n");
}

extern "C" __attribute__((noinline, visibility("hidden")))
const std::uint64_t* HookAbiSelftestFilterCore(const std::uint64_t* token) {
    std::uintptr_t sp = 0;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    g_filter_sp_mod16.store(static_cast<std::uint32_t>(sp & 0xf),
                            std::memory_order_relaxed);
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    const auto decision = g_gate.Filter(token ? *token : 0);
    g_shadow_token = decision.token_us;
    return &g_shadow_token;
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
HookAbiSelftestClobberVolatile() {
    __asm__ volatile(
        "mov x0, xzr\n"  "mov x1, xzr\n"  "mov x2, xzr\n"
        "mov x3, xzr\n"  "mov x4, xzr\n"  "mov x5, xzr\n"
        "mov x6, xzr\n"  "mov x7, xzr\n"  "mov x8, xzr\n"
        "mov x9, xzr\n"  "mov x10, xzr\n" "mov x11, xzr\n"
        "mov x12, xzr\n" "mov x13, xzr\n" "mov x14, xzr\n"
        "mov x15, xzr\n" "mov x16, xzr\n" "mov x17, xzr\n"
        "movi v0.16b, #0x5a\n"  "movi v1.16b, #0x5a\n"
        "movi v2.16b, #0x5a\n"  "movi v3.16b, #0x5a\n"
        "movi v4.16b, #0x5a\n"  "movi v5.16b, #0x5a\n"
        "movi v6.16b, #0x5a\n"  "movi v7.16b, #0x5a\n"
        "movi v16.16b, #0xa5\n" "movi v17.16b, #0xa5\n"
        "movi v18.16b, #0xa5\n" "movi v19.16b, #0xa5\n"
        "movi v20.16b, #0xa5\n" "movi v21.16b, #0xa5\n"
        "movi v22.16b, #0xa5\n" "movi v23.16b, #0xa5\n"
        "movi v24.16b, #0xa5\n" "movi v25.16b, #0xa5\n"
        "movi v26.16b, #0xa5\n" "movi v27.16b, #0xa5\n"
        "movi v28.16b, #0xa5\n" "movi v29.16b, #0xa5\n"
        "movi v30.16b, #0xa5\n" "movi v31.16b, #0xa5\n"
        "movz x9, #0x40, lsl #16\n"
        "msr fpcr, x9\n"
        "mov x9, #0x1f\n"
        "msr fpsr, x9\n"
        "movz x9, #0xf000, lsl #16\n"
        "msr nzcv, x9\n"
        "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden")))
const std::uint64_t* HookAbiSelftestFilter(const std::uint64_t*) {
    __asm__ volatile(
        "sub sp, sp, #0x20\n"
        "stp x19, x30, [sp, #0x10]\n"
        "bl HookAbiSelftestFilterCore\n"
        "mov x19, x0\n"
        "bl HookAbiSelftestClobberVolatile\n"
        "mov x0, x19\n"
        "ldp x19, x30, [sp, #0x10]\n"
        "add sp, sp, #0x20\n"
        "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
HookAbiSelftestEntry() {
    __asm__ volatile(
        "sub sp, sp, #0x310\n"
        "stp x0, x1, [sp, #0x000]\n"
        "stp x2, x3, [sp, #0x010]\n"
        "stp x4, x5, [sp, #0x020]\n"
        "stp x6, x7, [sp, #0x030]\n"
        "stp x8, x9, [sp, #0x040]\n"
        "stp x10, x11, [sp, #0x050]\n"
        "stp x12, x13, [sp, #0x060]\n"
        "stp x14, x15, [sp, #0x070]\n"
        "stp x16, x17, [sp, #0x080]\n"
        "stp x18, x19, [sp, #0x090]\n"
        "stp x20, x21, [sp, #0x0a0]\n"
        "stp x22, x23, [sp, #0x0b0]\n"
        "stp x24, x25, [sp, #0x0c0]\n"
        "stp x26, x27, [sp, #0x0d0]\n"
        "stp x28, x29, [sp, #0x0e0]\n"
        "str x30, [sp, #0x0f0]\n"
        "mrs x9, nzcv\n" "str x9, [sp, #0x0f8]\n"
        "mrs x9, fpcr\n" "str x9, [sp, #0x100]\n"
        "mrs x9, fpsr\n" "str x9, [sp, #0x108]\n"
        "stp q0, q1, [sp, #0x110]\n"
        "stp q2, q3, [sp, #0x130]\n"
        "stp q4, q5, [sp, #0x150]\n"
        "stp q6, q7, [sp, #0x170]\n"
        "stp q8, q9, [sp, #0x190]\n"
        "stp q10, q11, [sp, #0x1b0]\n"
        "stp q12, q13, [sp, #0x1d0]\n"
        "stp q14, q15, [sp, #0x1f0]\n"
        "stp q16, q17, [sp, #0x210]\n"
        "stp q18, q19, [sp, #0x230]\n"
        "stp q20, q21, [sp, #0x250]\n"
        "stp q22, q23, [sp, #0x270]\n"
        "stp q24, q25, [sp, #0x290]\n"
        "stp q26, q27, [sp, #0x2b0]\n"
        "stp q28, q29, [sp, #0x2d0]\n"
        "stp q30, q31, [sp, #0x2f0]\n"
        "ldr x0, [sp, #0x008]\n"
        "bl HookAbiSelftestFilter\n"
        "str x0, [sp, #0x008]\n"
        "ldp q0, q1, [sp, #0x110]\n"
        "ldp q2, q3, [sp, #0x130]\n"
        "ldp q4, q5, [sp, #0x150]\n"
        "ldp q6, q7, [sp, #0x170]\n"
        "ldp q8, q9, [sp, #0x190]\n"
        "ldp q10, q11, [sp, #0x1b0]\n"
        "ldp q12, q13, [sp, #0x1d0]\n"
        "ldp q14, q15, [sp, #0x1f0]\n"
        "ldp q16, q17, [sp, #0x210]\n"
        "ldp q18, q19, [sp, #0x230]\n"
        "ldp q20, q21, [sp, #0x250]\n"
        "ldp q22, q23, [sp, #0x270]\n"
        "ldp q24, q25, [sp, #0x290]\n"
        "ldp q26, q27, [sp, #0x2b0]\n"
        "ldp q28, q29, [sp, #0x2d0]\n"
        "ldp q30, q31, [sp, #0x2f0]\n"
        "ldr x9, [sp, #0x100]\n" "msr fpcr, x9\n"
        "ldr x9, [sp, #0x108]\n" "msr fpsr, x9\n"
        "ldr x9, [sp, #0x0f8]\n" "msr nzcv, x9\n"
        "ldp x0, x1, [sp, #0x000]\n"
        "ldp x2, x3, [sp, #0x010]\n"
        "ldp x4, x5, [sp, #0x020]\n"
        "ldp x6, x7, [sp, #0x030]\n"
        "ldp x8, x9, [sp, #0x040]\n"
        "ldp x10, x11, [sp, #0x050]\n"
        "ldp x12, x13, [sp, #0x060]\n"
        "ldp x14, x15, [sp, #0x070]\n"
        "ldp x16, x17, [sp, #0x080]\n"
        "ldp x18, x19, [sp, #0x090]\n"
        "ldp x20, x21, [sp, #0x0a0]\n"
        "ldp x22, x23, [sp, #0x0b0]\n"
        "ldp x24, x25, [sp, #0x0c0]\n"
        "ldp x26, x27, [sp, #0x0d0]\n"
        "ldp x28, x29, [sp, #0x0e0]\n"
        "ldr x30, [sp, #0x0f0]\n"
        "add sp, sp, #0x310\n"
        "adrp x17, :got:g_hook_abi_selftest_continue\n"
        "ldr x17, [x17, #:got_lo12:g_hook_abi_selftest_continue]\n"
        "ldr x17, [x17]\n"
        "br x17\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) std::uint64_t
HookAbiInvokeRaw(ProbeContext*, const std::uint64_t*, std::uint64_t*) {
    __asm__ volatile(
        "sub sp, sp, #0x120\n"
        "stp x19, x20, [sp, #0x000]\n"
        "stp x21, x22, [sp, #0x010]\n"
        "stp x23, x24, [sp, #0x020]\n"
        "stp x25, x26, [sp, #0x030]\n"
        "stp x27, x28, [sp, #0x040]\n"
        "stp x29, x30, [sp, #0x050]\n"
        "stp q8, q9, [sp, #0x060]\n"
        "stp q10, q11, [sp, #0x080]\n"
        "stp q12, q13, [sp, #0x0a0]\n"
        "stp q14, q15, [sp, #0x0c0]\n"
        "str x0, [sp, #0x0e0]\n"
        "str x1, [sp, #0x0e8]\n"
        "str x2, [sp, #0x0f0]\n"
        // X18 is the Android platform register.  Preserve the process-owned
        // value in the wrapper, use that same live value as the expected
        // sentinel, and never replace it with synthetic test data.
        "str x18, [sp, #0x118]\n"
        "str x18, [x0, #0x090]\n"
        "mrs x9, fpcr\n" "str x9, [sp, #0x0f8]\n"
        "mrs x9, fpsr\n" "str x9, [sp, #0x100]\n"
        "mrs x9, nzcv\n" "str x9, [sp, #0x108]\n"
        "mov x9, sp\n"
        "str x9, [x0, #0x620]\n"
        "movz x9, #0xa000, lsl #16\n"
        "str x9, [x0, #0x5f0]\n"
        "msr nzcv, x9\n"
        "mov x9, xzr\n"
        "str x9, [x0, #0x600]\n"
        "str x9, [x0, #0x610]\n"
        "msr fpcr, x9\n"
        "msr fpsr, x9\n"
        "adr x9, 1f\n"
        "str x9, [x0, #0x0f0]\n"
        "add x10, x0, #0x1f0\n"
        "ldp q0, q1, [x10, #0x000]\n"
        "ldp q2, q3, [x10, #0x020]\n"
        "ldp q4, q5, [x10, #0x040]\n"
        "ldp q6, q7, [x10, #0x060]\n"
        "ldp q8, q9, [x10, #0x080]\n"
        "ldp q10, q11, [x10, #0x0a0]\n"
        "ldp q12, q13, [x10, #0x0c0]\n"
        "ldp q14, q15, [x10, #0x0e0]\n"
        "ldp q16, q17, [x10, #0x100]\n"
        "ldp q18, q19, [x10, #0x120]\n"
        "ldp q20, q21, [x10, #0x140]\n"
        "ldp q22, q23, [x10, #0x160]\n"
        "ldp q24, q25, [x10, #0x180]\n"
        "ldp q26, q27, [x10, #0x1a0]\n"
        "ldp q28, q29, [x10, #0x1c0]\n"
        "ldp q30, q31, [x10, #0x1e0]\n"
        "ldp x2, x3, [x0, #0x010]\n"
        "ldp x4, x5, [x0, #0x020]\n"
        "ldp x6, x7, [x0, #0x030]\n"
        "ldr x9, [x0, #0x048]\n"
        "ldp x10, x11, [x0, #0x050]\n"
        "ldp x12, x13, [x0, #0x060]\n"
        "ldp x14, x15, [x0, #0x070]\n"
        "ldr x16, [x0, #0x080]\n"
        "ldp x19, x20, [x0, #0x098]\n"
        "ldp x21, x22, [x0, #0x0a8]\n"
        "ldp x23, x24, [x0, #0x0b8]\n"
        "ldp x25, x26, [x0, #0x0c8]\n"
        "ldp x27, x28, [x0, #0x0d8]\n"
        "ldr x29, [x0, #0x0e8]\n"
        "ldr x1, [sp, #0x0e8]\n"
        "ldr x8, [sp, #0x0f0]\n"
        "bl HookAbiSelftestTarget\n"
        "1:\n"
        "str x0, [sp, #0x110]\n"
        "ldr x9, [sp, #0x0f8]\n" "msr fpcr, x9\n"
        "ldr x9, [sp, #0x100]\n" "msr fpsr, x9\n"
        "ldr x9, [sp, #0x108]\n" "msr nzcv, x9\n"
        "ldp q8, q9, [sp, #0x060]\n"
        "ldp q10, q11, [sp, #0x080]\n"
        "ldp q12, q13, [sp, #0x0a0]\n"
        "ldp q14, q15, [sp, #0x0c0]\n"
        "ldp x19, x20, [sp, #0x000]\n"
        "ldp x21, x22, [sp, #0x010]\n"
        "ldp x23, x24, [sp, #0x020]\n"
        "ldp x25, x26, [sp, #0x030]\n"
        "ldp x27, x28, [sp, #0x040]\n"
        "ldp x29, x30, [sp, #0x050]\n"
        "ldr x18, [sp, #0x118]\n"
        "ldr x0, [sp, #0x110]\n"
        "add sp, sp, #0x120\n"
        "ret\n");
}

enum class FailureStage : int {
    kNone = 0,
    kReceiptPreparation = 1,
    kSelfScope = 2,
    kPrologue = 3,
    kTrampoline = 4,
    kInstallProtection = 5,
    kInstallReadback = 6,
    kBehavior = 7,
    kAbiState = 8,
    kRestoreBytes = 9,
    kRestoreProtection = 10,
    kTrampolineUnmap = 11,
};

struct Results {
    int checks{};
    int failures{};
    FailureStage first_failure{FailureStage::kNone};
    int first_errno{};
    bool trigger_claimed{};
    bool stale_receipt_cleared{};
    bool build_identity{};
    bool self_payload_scope{};
    bool target_elf_exec_load{};
    bool target_initial_guest_code_view{};
    bool target_initial_mapping_private{};
    bool target_initial_host_exec_visible{};
    bool target_page_isolated{};
    bool exact_prologue{};
    bool trampoline_pic{};
    bool trampoline_rw_no_exec_readback{};
    bool trampoline_bytes_readback{};
    bool trampoline_rx_readback{};
    bool target_rw_no_exec_readback{};
    bool install_bytes_readback{};
    bool install_protection_readback{};
    bool patch_published{};
    bool install_succeeded{};
    bool baseline_original_executed{};
    bool passthrough{};
    bool freeze{};
    bool single_step{};
    bool stress{};
    bool return_abi{};
    bool filter_stack_aligned{};
    bool target_stack_exact{};
    bool required_gpr_preserved{};
    bool simd_preserved{};
    bool nzcv_preserved{};
    bool fpcr_preserved{};
    bool fpsr_preserved{};
    bool prologue_once_per_call{};
    bool abi_all_calls{};
    bool restore_attempted{};
    bool restore_bytes_readback{};
    bool restore_rw_no_exec_readback{};
    bool restore_protection_readback{};
    bool target_page_w_xor_x{};
    bool trampoline_unmapped_readback{};
    bool restored_code_executed{};
    bool cleanup_complete{};
    std::uint64_t calls{};
    std::uint64_t hooked_target_calls{};
    std::uint64_t mapping_metadata_queries{};
    std::uint64_t trigger_nonce{};
    int target_initial_host_protection{};
    char payload_build_id[65]{};
};

std::atomic<std::uint64_t> g_mapping_metadata_queries{};

const char* FailureStageName(FailureStage stage) {
    switch (stage) {
        case FailureStage::kNone: return "none";
        case FailureStage::kReceiptPreparation: return "receipt_preparation";
        case FailureStage::kSelfScope: return "self_scope";
        case FailureStage::kPrologue: return "prologue";
        case FailureStage::kTrampoline: return "trampoline";
        case FailureStage::kInstallProtection: return "install_protection";
        case FailureStage::kInstallReadback: return "install_readback";
        case FailureStage::kBehavior: return "behavior";
        case FailureStage::kAbiState: return "abi_state";
        case FailureStage::kRestoreBytes: return "restore_bytes";
        case FailureStage::kRestoreProtection: return "restore_protection";
        case FailureStage::kTrampolineUnmap: return "trampoline_unmap";
    }
    return "unknown";
}

bool Check(Results* results, bool condition, FailureStage stage,
           int error_number = 0) {
    ++results->checks;
    if (condition) return true;
    ++results->failures;
    if (results->first_failure == FailureStage::kNone) {
        results->first_failure = stage;
        results->first_errno = error_number;
    }
    return false;
}

bool IsLowerHexSha256(const char* text) {
    if (text == nullptr || std::strlen(text) != 64) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::size_t AlignNote(std::size_t value) {
    return (value + 3U) & ~std::size_t{3U};
}

bool ReadSelfBuildId(const void* image_base, char output[65]) {
    if (image_base == nullptr || output == nullptr) return false;
    const auto* base = static_cast<const std::uint8_t*>(image_base);
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_machine != EM_AARCH64 || header->e_type != ET_DYN ||
        header->e_phentsize != sizeof(Elf64_Phdr) || header->e_phnum == 0) {
        return false;
    }
    const auto* programs = reinterpret_cast<const Elf64_Phdr*>(
        base + header->e_phoff);
    constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < header->e_phnum; ++index) {
        const Elf64_Phdr& program = programs[index];
        if (program.p_type != PT_NOTE || program.p_memsz < sizeof(Elf64_Nhdr))
            continue;
        const std::uint8_t* cursor = base + program.p_vaddr;
        const std::uint8_t* end = cursor + program.p_memsz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            std::memcpy(&note, cursor, sizeof(note));
            cursor += sizeof(note);
            const std::size_t name_size = AlignNote(note.n_namesz);
            const std::size_t desc_size = AlignNote(note.n_descsz);
            if (name_size > static_cast<std::size_t>(end - cursor) ||
                desc_size > static_cast<std::size_t>(end - cursor) - name_size)
                return false;
            const std::uint8_t* name = cursor;
            const std::uint8_t* description = cursor + name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(name, "GNU", 4) == 0 && note.n_descsz != 0 &&
                note.n_descsz <= 32) {
                for (std::size_t byte = 0; byte < note.n_descsz; ++byte) {
                    output[byte * 2] = kHex[description[byte] >> 4];
                    output[byte * 2 + 1] = kHex[description[byte] & 0xf];
                }
                output[note.n_descsz * 2] = '\0';
                return true;
            }
            cursor = description + desc_size;
        }
    }
    return false;
}

struct MappingMetadata {
    std::uintptr_t start{};
    std::uintptr_t end{};
    int protection{};
    bool found{};
    bool query_ok{};
    bool private_mapping{};
};

MappingMetadata QueryMapping(const void* address) {
    MappingMetadata metadata{};
    g_mapping_metadata_queries.fetch_add(1, std::memory_order_relaxed);
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return metadata;
    char line[2048]{};
    const auto target = reinterpret_cast<std::uintptr_t>(address);
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
    if (begin > UINTPTR_MAX - size) return false;
    const auto end = begin + size;
    return begin >= mapping.start && end <= mapping.end;
}

bool IsPrivateGuestCodeView(const MappingMetadata& mapping,
                            const void* address, std::size_t size) {
    const int protection = mapping.protection;
    const bool host_view = protection == PROT_READ ||
        protection == (PROT_READ | PROT_EXEC);
    return MappingCovers(mapping, address, size) && mapping.private_mapping &&
        host_view;
}

bool IsPrivateWritableNoExec(const MappingMetadata& mapping,
                             const void* address, std::size_t size) {
    return MappingCovers(mapping, address, size) && mapping.private_mapping &&
        mapping.protection == (PROT_READ | PROT_WRITE);
}

bool TargetRangeInExecutableLoad(const void* image_base, const void* target,
                                 std::size_t size) {
    if (image_base == nullptr || target == nullptr || size == 0) return false;
    const auto* base = static_cast<const std::uint8_t*>(image_base);
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_machine != EM_AARCH64 || header->e_type != ET_DYN ||
        header->e_phentsize != sizeof(Elf64_Phdr) || header->e_phnum == 0) {
        return false;
    }
    const auto image = reinterpret_cast<std::uintptr_t>(image_base);
    const auto target_begin = reinterpret_cast<std::uintptr_t>(target);
    if (target_begin > UINTPTR_MAX - size) return false;
    const auto target_end = target_begin + size;
    const auto* programs = reinterpret_cast<const Elf64_Phdr*>(
        base + header->e_phoff);
    for (std::size_t index = 0; index < header->e_phnum; ++index) {
        const Elf64_Phdr& program = programs[index];
        if (program.p_type != PT_LOAD ||
            (program.p_flags & (PF_R | PF_X | PF_W)) != (PF_R | PF_X)) {
            continue;
        }
        if (program.p_vaddr > UINTPTR_MAX - image ||
            program.p_memsz > UINTPTR_MAX - (image + program.p_vaddr)) {
            return false;
        }
        const auto segment_begin = image + program.p_vaddr;
        const auto segment_end = segment_begin + program.p_memsz;
        if (target_begin >= segment_begin && target_end <= segment_end)
            return true;
    }
    return false;
}

void AbsoluteJump(std::uint8_t output[kPatchSize],
                  std::uintptr_t destination) {
    constexpr std::uint32_t kLoadX17 = 0x58000051;
    constexpr std::uint32_t kBranchX17 = 0xd61f0220;
    std::memcpy(output, &kLoadX17, sizeof(kLoadX17));
    std::memcpy(output + 4, &kBranchX17, sizeof(kBranchX17));
    std::memcpy(output + 8, &destination, sizeof(destination));
}

bool PrologueWordsAreRelocationFree(const std::uint8_t* bytes) {
    std::uint32_t words[4]{};
    std::memcpy(words, bytes, sizeof(words));
    return std::memcmp(words, kExpectedPrologueWords, sizeof(words)) == 0;
}

class SelfHookTransaction {
public:
    bool Install(Results* results) {
        target_ = reinterpret_cast<std::uint8_t*>(&HookAbiSelftestTarget);
        page_size_ = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        const bool page_size_ok =
            page_size_ != 0 && (page_size_ & (page_size_ - 1)) == 0;
        if (!Check(results, page_size_ok, FailureStage::kInstallProtection))
            return false;

        const auto address = reinterpret_cast<std::uintptr_t>(target_);
        const auto page_end = reinterpret_cast<std::uintptr_t>(
            &HookAbiSelftestTargetPageEnd);
        target_page_ = address & ~static_cast<std::uintptr_t>(page_size_ - 1);
        results->target_page_isolated = address == target_page_ &&
            page_end >= address + page_size_;
        if (!Check(results, results->target_page_isolated,
                   FailureStage::kSelfScope)) return false;

        Dl_info target_info{};
        Dl_info entry_info{};
        Dl_info end_info{};
        const bool target_resolved =
            dladdr(reinterpret_cast<void*>(&HookAbiSelftestTarget),
                   &target_info) != 0;
        const bool entry_resolved =
            dladdr(reinterpret_cast<void*>(&HookAbiSelftestEntry),
                   &entry_info) != 0;
        const bool end_resolved =
            dladdr(reinterpret_cast<void*>(&HookAbiSelftestTargetPageEnd),
                   &end_info) != 0;
        results->build_identity = IsLowerHexSha256(kSourceSha256) &&
            IsLowerHexSha256(kHeaderSha256) && target_resolved &&
            ReadSelfBuildId(target_info.dli_fbase, results->payload_build_id);
        if (!Check(results, results->build_identity,
                   FailureStage::kSelfScope)) return false;
        results->self_payload_scope = target_resolved && entry_resolved &&
            end_resolved && target_info.dli_fbase != nullptr &&
            target_info.dli_fbase == entry_info.dli_fbase &&
            target_info.dli_fbase == end_info.dli_fbase &&
            target_info.dli_fname != nullptr && entry_info.dli_fname != nullptr &&
            end_info.dli_fname != nullptr &&
            std::strcmp(target_info.dli_fname, kExpectedPayloadPath) == 0 &&
            std::strcmp(entry_info.dli_fname, kExpectedPayloadPath) == 0 &&
            std::strcmp(end_info.dli_fname, kExpectedPayloadPath) == 0;
        if (!Check(results, results->self_payload_scope,
                   FailureStage::kSelfScope)) return false;

        results->target_elf_exec_load = TargetRangeInExecutableLoad(
            target_info.dli_fbase, target_, kPatchSize);
        if (!Check(results, results->target_elf_exec_load,
                   FailureStage::kSelfScope)) return false;

        original_host_mapping_ = QueryMapping(target_);
        results->target_initial_host_protection =
            original_host_mapping_.protection;
        results->target_initial_mapping_private =
            original_host_mapping_.private_mapping;
        results->target_initial_host_exec_visible =
            (original_host_mapping_.protection & PROT_EXEC) != 0;
        results->target_initial_guest_code_view = IsPrivateGuestCodeView(
            original_host_mapping_, target_, kPatchSize);
        if (!Check(results, results->target_initial_guest_code_view,
                   FailureStage::kInstallProtection))
            return false;

        results->exact_prologue =
            std::memcmp(target_, kExpectedPrologue, kPatchSize) == 0;
        if (!Check(results, results->exact_prologue,
                   FailureStage::kPrologue)) return false;
        results->trampoline_pic = PrologueWordsAreRelocationFree(target_);
        if (!Check(results, results->trampoline_pic,
                   FailureStage::kPrologue)) return false;
        std::memcpy(original_, target_, kPatchSize);
        original_captured_ = true;

        trampoline_ = static_cast<std::uint8_t*>(mmap(
            nullptr, page_size_, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (trampoline_ == MAP_FAILED) {
            trampoline_ = nullptr;
            Check(results, false, FailureStage::kTrampoline, errno);
            return false;
        }
        const MappingMetadata trampoline_writable_mapping =
            QueryMapping(trampoline_);
        results->trampoline_rw_no_exec_readback = IsPrivateWritableNoExec(
            trampoline_writable_mapping, trampoline_, kTrampolineCodeSize);
        if (!Check(results, results->trampoline_rw_no_exec_readback,
                   FailureStage::kTrampoline)) return false;
        std::memcpy(trampoline_, original_, kPatchSize);
        AbsoluteJump(trampoline_ + kPatchSize,
                     reinterpret_cast<std::uintptr_t>(target_ + kPatchSize));
        std::uint8_t expected_trampoline[kTrampolineCodeSize]{};
        std::memcpy(expected_trampoline, original_, kPatchSize);
        AbsoluteJump(expected_trampoline + kPatchSize,
                     reinterpret_cast<std::uintptr_t>(target_ + kPatchSize));
        __builtin___clear_cache(reinterpret_cast<char*>(trampoline_),
                                reinterpret_cast<char*>(trampoline_ +
                                                        kTrampolineCodeSize));
        results->trampoline_bytes_readback =
            std::memcmp(trampoline_, expected_trampoline,
                        kTrampolineCodeSize) == 0;
        if (!Check(results, results->trampoline_bytes_readback,
                   FailureStage::kTrampoline)) return false;
        if (mprotect(trampoline_, page_size_, PROT_READ | PROT_EXEC) != 0) {
            Check(results, false, FailureStage::kTrampoline, errno);
            return false;
        }
        const MappingMetadata trampoline_mapping = QueryMapping(trampoline_);
        results->trampoline_rx_readback = IsPrivateGuestCodeView(
            trampoline_mapping, trampoline_, kTrampolineCodeSize);
        if (!Check(results, results->trampoline_rx_readback,
                   FailureStage::kTrampoline)) return false;

        g_hook_abi_selftest_continue =
            reinterpret_cast<std::uintptr_t>(trampoline_);
        AbsoluteJump(patch_,
                     reinterpret_cast<std::uintptr_t>(&HookAbiSelftestEntry));
        if (!SetTargetWritable()) {
            Check(results, false, FailureStage::kInstallProtection, errno);
            return false;
        }
        target_writable_ = true;
        const MappingMetadata writable_mapping = QueryMapping(target_);
        results->target_rw_no_exec_readback = IsPrivateWritableNoExec(
            writable_mapping, target_, kPatchSize);
        if (!Check(results, results->target_rw_no_exec_readback,
                   FailureStage::kInstallProtection)) return false;

        std::memcpy(target_, patch_, kPatchSize);
        __builtin___clear_cache(reinterpret_cast<char*>(target_),
                                reinterpret_cast<char*>(target_ + kPatchSize));
        g_installed.store(true, std::memory_order_release);
        results->patch_published = true;
        results->install_bytes_readback =
            std::memcmp(target_, patch_, kPatchSize) == 0;
        if (!Check(results, results->install_bytes_readback,
                   FailureStage::kInstallReadback)) return false;
        if (!SetTargetProtection(kLogicalCodeProtection)) {
            Check(results, false, FailureStage::kInstallProtection, errno);
            return false;
        }
        target_writable_ = false;
        const MappingMetadata installed_mapping = QueryMapping(target_);
        results->install_protection_readback = IsPrivateGuestCodeView(
            installed_mapping, target_, kPatchSize);
        if (!Check(results, results->install_protection_readback,
                   FailureStage::kInstallReadback)) return false;
        results->target_page_w_xor_x =
            results->target_initial_guest_code_view &&
            results->target_rw_no_exec_readback &&
            results->install_protection_readback;
        results->install_succeeded = true;
        return true;
    }

    bool Restore(Results* results) {
        results->restore_attempted =
            original_captured_ || trampoline_ != nullptr || target_writable_ ||
            g_installed.load(std::memory_order_acquire);
        bool bytes_ok = true;
        bool protection_ok = true;

        if (original_captured_) {
            if (std::memcmp(target_, original_, kPatchSize) != 0) {
                if (!target_writable_) {
                    target_writable_ = SetTargetWritable();
                }
                if (target_writable_) {
                    const MappingMetadata restore_writable_mapping =
                        QueryMapping(target_);
                    results->restore_rw_no_exec_readback =
                        IsPrivateWritableNoExec(restore_writable_mapping,
                                                target_, kPatchSize);
                    Check(results, results->restore_rw_no_exec_readback,
                          FailureStage::kRestoreProtection);
                    std::memcpy(target_, original_, kPatchSize);
                    __builtin___clear_cache(
                        reinterpret_cast<char*>(target_),
                        reinterpret_cast<char*>(target_ + kPatchSize));
                }
            } else {
                results->restore_rw_no_exec_readback = true;
            }
            bytes_ok = std::memcmp(target_, original_, kPatchSize) == 0;
            results->restore_bytes_readback = bytes_ok;
            Check(results, bytes_ok, FailureStage::kRestoreBytes);

            if (target_writable_) {
                protection_ok = SetTargetProtection(kLogicalCodeProtection);
                target_writable_ = !protection_ok;
            }
            const MappingMetadata restored_mapping = QueryMapping(target_);
            protection_ok = protection_ok && IsPrivateGuestCodeView(
                restored_mapping, target_, kPatchSize);
            results->restore_protection_readback = protection_ok;
            results->target_page_w_xor_x = results->target_page_w_xor_x &&
                results->target_rw_no_exec_readback &&
                results->restore_rw_no_exec_readback && protection_ok;
            Check(results, protection_ok, FailureStage::kRestoreProtection,
                  protection_ok ? 0 : errno);
            Check(results, results->target_page_w_xor_x,
                  FailureStage::kRestoreProtection);
        } else {
            results->restore_bytes_readback = true;
            results->restore_rw_no_exec_readback = true;
            results->restore_protection_readback = true;
        }
        return bytes_ok && protection_ok;
    }

    bool ReleaseTrampoline(Results* results, bool restored_execution_proven) {
        bool trampoline_ok = true;
        const bool release_allowed = results->restore_bytes_readback &&
                                     restored_execution_proven;
        if (trampoline_ != nullptr && !release_allowed) {
            // A translated stale patch may still branch through Entry.  Keep
            // both the continuation and mapping valid and require the caller
            // to terminate this disposable process.
            trampoline_ok = false;
        } else if (release_allowed) {
            g_installed.store(false, std::memory_order_release);
            g_hook_abi_selftest_continue = 0;
            if (trampoline_) {
                void* const old_mapping = trampoline_;
                errno = 0;
                const int unmap_result = munmap(trampoline_, page_size_);
                const int unmap_errno = unmap_result == 0 ? 0 : errno;

                // Verify the hole immediately, before stdio or another thread
                // can reuse the released address.  Re-opening /proc/self/maps
                // here made successful munmap calls look like failures when
                // the one-page address was recycled before the scan.
                unsigned char residency = 0;
                errno = 0;
                const int residency_result =
                    mincore(old_mapping, page_size_, &residency);
                const int residency_errno =
                    residency_result == 0 ? 0 : errno;
                trampoline_ok = unmap_result == 0 &&
                    residency_result == -1 && residency_errno == ENOMEM;
                if (trampoline_ok) trampoline_ = nullptr;
                errno = unmap_result == 0 ? residency_errno : unmap_errno;
            }
        }
        results->trampoline_unmapped_readback = trampoline_ok;
        Check(results, trampoline_ok, FailureStage::kTrampolineUnmap,
              trampoline_ok ? 0 : errno);
        results->cleanup_complete = results->restore_bytes_readback &&
            results->restore_protection_readback &&
            results->target_page_w_xor_x && trampoline_ok;
        return results->cleanup_complete;
    }

private:
    bool SetTargetWritable() const {
        return SetTargetProtection(PROT_READ | PROT_WRITE);
    }

    bool SetTargetProtection(int protection) const {
        return mprotect(reinterpret_cast<void*>(target_page_), page_size_,
                        protection) == 0;
    }

    std::uint8_t* target_{};
    std::uintptr_t target_page_{};
    std::size_t page_size_{};
    MappingMetadata original_host_mapping_{};
    std::uint8_t original_[kPatchSize]{};
    std::uint8_t patch_[kPatchSize]{};
    std::uint8_t* trampoline_{};
    bool original_captured_{};
    bool target_writable_{};
};

void PrepareProbe(ProbeContext* context, const std::uint64_t* token,
                  std::uint64_t* observed, bool hooked) {
    for (std::size_t i = 0; i < 31; ++i) {
        context->expected_gpr[i] =
            0x1100000000000000ULL +
            static_cast<std::uint64_t>(i) * 0x0101010101010101ULL;
    }
    for (std::size_t reg = 0; reg < 32; ++reg) {
        for (std::size_t byte = 0; byte < 16; ++byte) {
            context->expected_simd[reg][byte] = static_cast<std::uint8_t>(
                0x31U + reg * 17U + byte * 13U);
        }
    }
    context->expected_gpr[0] = reinterpret_cast<std::uintptr_t>(context);
    context->expected_gpr[1] = reinterpret_cast<std::uintptr_t>(
        hooked ? &g_shadow_token : token);
    context->expected_gpr[8] = reinterpret_cast<std::uintptr_t>(observed);
}

bool ValidateGpr(const ProbeContext& context) {
    for (std::size_t i = 0; i < 31; ++i) {
        if (i == 17) continue;  // X17/IP1 is consumed by both absolute jumps.
        if (context.expected_gpr[i] != context.observed_gpr[i]) return false;
    }
    return true;
}

bool ValidateTargetStack(const ProbeContext& context) {
    return context.target_entry_sp >= context.target_body_sp &&
        context.target_entry_sp - context.target_body_sp == 0x60 &&
        (context.target_entry_sp & 0xf) == 0 &&
        (context.target_body_sp & 0xf) == 0;
}

bool ValidateAbiSnapshot(const ProbeContext& context,
                         std::uint64_t expected_prologue_count) {
    return ValidateGpr(context) && ValidateTargetStack(context) &&
        std::memcmp(context.expected_simd, context.observed_simd,
                    sizeof(context.expected_simd)) == 0 &&
        context.expected_nzcv == context.observed_nzcv &&
        context.expected_fpcr == context.observed_fpcr &&
        context.expected_fpsr == context.observed_fpsr &&
        context.target_prologue_count == expected_prologue_count;
}

void RecordAbiChecks(Results* results, const ProbeContext& context,
                     std::uint64_t expected_prologue_count) {
    results->required_gpr_preserved = ValidateGpr(context);
    Check(results, results->required_gpr_preserved,
          FailureStage::kAbiState);
    results->simd_preserved =
        std::memcmp(context.expected_simd, context.observed_simd,
                    sizeof(context.expected_simd)) == 0;
    Check(results, results->simd_preserved, FailureStage::kAbiState);
    results->nzcv_preserved = context.expected_nzcv == context.observed_nzcv;
    Check(results, results->nzcv_preserved, FailureStage::kAbiState);
    results->fpcr_preserved = context.expected_fpcr == context.observed_fpcr;
    Check(results, results->fpcr_preserved, FailureStage::kAbiState);
    results->fpsr_preserved = context.expected_fpsr == context.observed_fpsr;
    Check(results, results->fpsr_preserved, FailureStage::kAbiState);
    results->target_stack_exact = ValidateTargetStack(context);
    Check(results, results->target_stack_exact, FailureStage::kAbiState);
    results->prologue_once_per_call =
        context.target_prologue_count == expected_prologue_count;
    Check(results, results->prologue_once_per_call,
          FailureStage::kAbiState);
}

bool ValidateRestoredProbe(const ProbeContext& context) {
    return ValidateAbiSnapshot(context, 1);
}

Results RunSelftest(bool stale_receipt_cleared, int receipt_clear_errno,
                    std::uint64_t trigger_nonce) {
    Results results{};
    results.trigger_claimed = true;
    results.trigger_nonce = trigger_nonce;
    results.stale_receipt_cleared = stale_receipt_cleared;
    const bool nonce_ok = Check(&results, trigger_nonce != 0,
                                FailureStage::kReceiptPreparation);
    const bool receipt_ready = Check(
        &results, stale_receipt_cleared, FailureStage::kReceiptPreparation,
        receipt_clear_errno);
    if (!nonce_ok || !receipt_ready) {
        results.cleanup_complete = true;
        results.restore_bytes_readback = true;
        results.restore_protection_readback = true;
        results.trampoline_unmapped_readback = true;
        return results;
    }

    g_gate.Resume();
    g_gate.ResetTimeline();
    g_hook_calls.store(0, std::memory_order_release);
    g_filter_sp_mod16.store(0xff, std::memory_order_release);
    g_mapping_metadata_queries.store(0, std::memory_order_release);

    // Prime Houdini's translation cache with the unmodified target before any
    // self-modification.  HABI-1 is specifically intended to prove that both
    // the install and restore transitions invalidate translated code safely.
    ProbeContext baseline_context{};
    std::uint64_t baseline_token = 777777;
    std::uint64_t baseline_observed = 0;
    PrepareProbe(&baseline_context, &baseline_token, &baseline_observed, false);
    const std::uint64_t baseline_result = HookAbiInvokeRaw(
        &baseline_context, &baseline_token, &baseline_observed);
    const bool baseline_behavior = baseline_observed == baseline_token &&
        baseline_result == baseline_token + 7;
    const bool baseline_abi = ValidateAbiSnapshot(baseline_context, 1);
    results.baseline_original_executed = baseline_behavior && baseline_abi &&
        g_hook_calls.load(std::memory_order_acquire) == 0;
    Check(&results, baseline_behavior, FailureStage::kBehavior);
    Check(&results, baseline_abi, FailureStage::kAbiState);
    Check(&results, results.baseline_original_executed,
          FailureStage::kBehavior);

    SelfHookTransaction transaction;
    const bool installed = results.baseline_original_executed &&
                           transaction.Install(&results);
    ProbeContext context{};
    std::uint64_t observed = 0;

    if (installed) {
        std::uint64_t token = 12345;
        PrepareProbe(&context, &token, &observed, true);
        std::uint64_t result = HookAbiInvokeRaw(&context, &token, &observed);
        results.passthrough = observed == token && result == token + 7;
        results.return_abi = results.passthrough;
        Check(&results, results.passthrough, FailureStage::kBehavior);
        RecordAbiChecks(&results, context, 1);
        bool abi_all_calls = ValidateAbiSnapshot(context, 1);
        results.filter_stack_aligned =
            g_filter_sp_mod16.load(std::memory_order_relaxed) == 0;
        Check(&results, results.filter_stack_aligned,
              FailureStage::kAbiState);

        g_gate.Freeze();
        token = 99999;
        result = HookAbiInvokeRaw(&context, &token, &observed);
        results.freeze = observed == 0 && result == 7;
        Check(&results, results.freeze, FailureStage::kBehavior);
        const bool freeze_abi = ValidateAbiSnapshot(context, 2);
        abi_all_calls = abi_all_calls && freeze_abi;

        const bool step_accepted = g_gate.RequestSteps();
        result = HookAbiInvokeRaw(&context, &token, &observed);
        const bool released_step = observed == 16666 && result == 16673;
        const bool released_step_abi = ValidateAbiSnapshot(context, 3);
        result = HookAbiInvokeRaw(&context, &token, &observed);
        const bool refrozen = observed == 0 && result == 7;
        const bool refrozen_abi = ValidateAbiSnapshot(context, 4);
        abi_all_calls = abi_all_calls && released_step_abi && refrozen_abi;
        results.single_step = step_accepted && released_step && refrozen;
        Check(&results, results.single_step, FailureStage::kBehavior);

        g_gate.Resume();
        bool stress_ok = true;
        bool stress_abi_ok = true;
        for (std::uint64_t i = 0; i < 100000; ++i) {
            result = HookAbiInvokeRaw(&context, &i, &observed);
            const bool call_abi = ValidateAbiSnapshot(context, i + 5);
            if (!call_abi) stress_abi_ok = false;
            if (observed != i || result != i + 7 || !call_abi) {
                stress_ok = false;
                break;
            }
        }
        abi_all_calls = abi_all_calls && stress_abi_ok;
        results.abi_all_calls = abi_all_calls;
        Check(&results, results.abi_all_calls, FailureStage::kAbiState);
        results.calls = g_hook_calls.load(std::memory_order_acquire);
        results.hooked_target_calls = context.target_prologue_count;
        results.stress = stress_ok && results.calls == 100004 &&
                         results.hooked_target_calls == 100004;
        Check(&results, results.stress, FailureStage::kBehavior);
    }

    const bool restored = transaction.Restore(&results);
    if (results.patch_published && restored) {
        ProbeContext restored_context{};
        std::uint64_t restored_token = 424242;
        std::uint64_t restored_observed = 0;
        PrepareProbe(&restored_context, &restored_token, &restored_observed,
                     false);
        const std::uint64_t restored_result = HookAbiInvokeRaw(
            &restored_context, &restored_token, &restored_observed);
        results.restored_code_executed =
            restored_observed == restored_token &&
            restored_result == restored_token + 7 &&
            ValidateRestoredProbe(restored_context) &&
            g_hook_calls.load(std::memory_order_acquire) == results.calls;
        Check(&results, results.restored_code_executed,
              FailureStage::kRestoreBytes);
    }
    const bool safe_to_release = results.restore_bytes_readback &&
        (!results.patch_published || results.restored_code_executed);
    transaction.ReleaseTrampoline(&results, safe_to_release);
    results.mapping_metadata_queries =
        g_mapping_metadata_queries.load(std::memory_order_acquire);
    return results;
}

bool WriteAll(int fd, const char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool WriteStatus(const Results& results) {
    char status[8192]{};
    const bool pass = results.failures == 0 && results.cleanup_complete &&
                      results.restored_code_executed;
    const int size = std::snprintf(
        status, sizeof(status),
        "protocol=hook-abi-selftest-habi1\nrevision=2\nabi=arm64-v8a\n"
        "gate=HABI-1\nscope=self-only\nrun_mode=explicit-export-only\n"
        "selftest_constructor_trigger=0\nthreads_created=0\n"
        "receipt_commit=atomic-rename-file-fsync-dir-fsync\n"
        "pid=%d\ntid=%ld\ntrigger_nonce=%llu\nrun_calls=1\n"
        "source_sha256=%s\nphysics_token_gate_sha256=%s\n"
        "payload_build_id=%s\nexpected_payload_path=%s\n"
        "game_module_names_queried=0\ngame_addresses_dereferenced=0\n"
        "game_addresses_modified=0\nmapping_metadata_queries=%llu\n"
        "absolute_jump_scratch=x17-ip1\ntarget_page_w_xor_x=%d\n"
        "checks=%d\nfailures=%d\nfirst_failure_stage=%s\n"
        "first_failure_code=%d\nfirst_errno=%d\ntrigger_claimed=%d\n"
        "stale_receipt_cleared=%d\nbuild_identity=%d\n"
        "self_payload_scope=%d\ntarget_elf_exec_load=%d\n"
        "target_initial_guest_code_view=%d\n"
        "target_initial_mapping_private=%d\n"
        "target_initial_host_exec_visible=%d\n"
        "target_initial_host_protection=%d\n"
        "target_page_isolated=%d\nexact_16b_prologue=%d\n"
        "trampoline_prologue_pic=%d\ntrampoline_rw_no_exec_readback=%d\n"
        "trampoline_bytes_readback=%d\n"
        "trampoline_rx_readback=%d\ntarget_rw_no_exec_readback=%d\n"
        "install_bytes_readback=%d\ninstall_protection_readback=%d\n"
        "patch_published=%d\ninstall_succeeded=%d\n"
        "baseline_original_executed=%d\n"
        "passthrough=%d\nfreeze=%d\nsingle_step=%d\n"
        "stress_100000=%d\nreturn_abi=%d\nfilter_stack_aligned=%d\n"
        "target_stack_exact=%d\nrequired_gpr_preserved=%d\n"
        "simd_q0_q31_preserved=%d\nnzcv_preserved=%d\nfpcr_preserved=%d\n"
        "fpsr_preserved=%d\nprologue_once_per_call=%d\n"
        "abi_all_100004_calls=%d\ncalls=%llu\n"
        "hooked_target_calls=%llu\nrestore_attempted=%d\n"
        "restore_bytes_readback=%d\nrestore_rw_no_exec_readback=%d\n"
        "restore_protection_readback=%d\n"
        "trampoline_unmapped_readback=%d\nrestored_code_executed=%d\n"
        "cleanup_complete=%d\nresult=%s\n",
        static_cast<int>(getpid()), static_cast<long>(syscall(__NR_gettid)),
        static_cast<unsigned long long>(results.trigger_nonce),
        kSourceSha256, kHeaderSha256, results.payload_build_id,
        kExpectedPayloadPath,
        static_cast<unsigned long long>(results.mapping_metadata_queries),
        results.target_page_w_xor_x ? 1 : 0,
        results.checks, results.failures,
        FailureStageName(results.first_failure),
        static_cast<int>(results.first_failure), results.first_errno,
        results.trigger_claimed ? 1 : 0,
        results.stale_receipt_cleared ? 1 : 0,
        results.build_identity ? 1 : 0,
        results.self_payload_scope ? 1 : 0,
        results.target_elf_exec_load ? 1 : 0,
        results.target_initial_guest_code_view ? 1 : 0,
        results.target_initial_mapping_private ? 1 : 0,
        results.target_initial_host_exec_visible ? 1 : 0,
        results.target_initial_host_protection,
        results.target_page_isolated ? 1 : 0,
        results.exact_prologue ? 1 : 0, results.trampoline_pic ? 1 : 0,
        results.trampoline_rw_no_exec_readback ? 1 : 0,
        results.trampoline_bytes_readback ? 1 : 0,
        results.trampoline_rx_readback ? 1 : 0,
        results.target_rw_no_exec_readback ? 1 : 0,
        results.install_bytes_readback ? 1 : 0,
        results.install_protection_readback ? 1 : 0,
        results.patch_published ? 1 : 0,
        results.install_succeeded ? 1 : 0,
        results.baseline_original_executed ? 1 : 0,
        results.passthrough ? 1 : 0,
        results.freeze ? 1 : 0, results.single_step ? 1 : 0,
        results.stress ? 1 : 0, results.return_abi ? 1 : 0,
        results.filter_stack_aligned ? 1 : 0,
        results.target_stack_exact ? 1 : 0,
        results.required_gpr_preserved ? 1 : 0,
        results.simd_preserved ? 1 : 0, results.nzcv_preserved ? 1 : 0,
        results.fpcr_preserved ? 1 : 0, results.fpsr_preserved ? 1 : 0,
        results.prologue_once_per_call ? 1 : 0,
        results.abi_all_calls ? 1 : 0,
        static_cast<unsigned long long>(results.calls),
        static_cast<unsigned long long>(results.hooked_target_calls),
        results.restore_attempted ? 1 : 0,
        results.restore_bytes_readback ? 1 : 0,
        results.restore_rw_no_exec_readback ? 1 : 0,
        results.restore_protection_readback ? 1 : 0,
        results.trampoline_unmapped_readback ? 1 : 0,
        results.restored_code_executed ? 1 : 0,
        results.cleanup_complete ? 1 : 0, pass ? "PASS" : "FAIL");
    const bool formatted =
        size > 0 && static_cast<std::size_t>(size) < sizeof(status);
    if (!formatted) return false;

    char temporary_path[256]{};
    const int temporary_size = std::snprintf(
        temporary_path, sizeof(temporary_path), "%s.tmp.%d.%ld", kStatusPath,
        static_cast<int>(getpid()), static_cast<long>(syscall(__NR_gettid)));
    if (temporary_size <= 0 ||
        static_cast<std::size_t>(temporary_size) >= sizeof(temporary_path)) {
        return false;
    }
    (void)unlink(temporary_path);
    const int fd = open(temporary_path,
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    const bool written = WriteAll(fd, status, static_cast<std::size_t>(size));
    const bool synced = written && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!written || !synced || !closed) {
        (void)unlink(temporary_path);
        return false;
    }
    if (::rename(temporary_path, kStatusPath) != 0) {
        (void)unlink(temporary_path);
        return false;
    }
    const int directory = open(kStatusDirectory,
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        (void)unlink(kStatusPath);
        return false;
    }
    const bool directory_synced = fsync(directory) == 0;
    const bool directory_closed = close(directory) == 0;
    if (!directory_synced || !directory_closed) {
        (void)unlink(kStatusPath);
        return false;
    }
    return true;
}

std::uint64_t EncodeRunReturn(std::uint32_t tag) {
    const auto tid = static_cast<std::uint32_t>(syscall(__NR_gettid));
    return (static_cast<std::uint64_t>(tid) << 32) | tag;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int
a9tas_hook_abi_selftest_habi1_protocol() {
    return 2;
}

extern "C" __attribute__((visibility("default"))) jlong
a9tas_hook_abi_selftest_habi1_run(JNIEnv*, jobject, jlong nonce) {
    int expected = 0;
    if (!g_run_state.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return static_cast<jlong>(EncodeRunReturn(
            expected == 2 ? kRunAlreadyPassedTag : kRunAlreadyFailedTag));
    }

    errno = 0;
    const bool stale_receipt_cleared =
        unlink(kStatusPath) == 0 || errno == ENOENT;
    const int receipt_clear_errno = stale_receipt_cleared ? 0 : errno;
    const Results results = RunSelftest(
        stale_receipt_cleared, receipt_clear_errno,
        static_cast<std::uint64_t>(nonce));
    const bool receipt_written = WriteStatus(results);
    const bool pass = results.failures == 0 && results.cleanup_complete &&
                      results.restored_code_executed && receipt_written;
    g_run_state.store(pass ? 2 : 3, std::memory_order_release);
    __android_log_print(
        pass ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "explicit self-only checks=%d failures=%d cleanup=%d receipt=%d",
        results.checks, results.failures,
        results.cleanup_complete ? 1 : 0, receipt_written ? 1 : 0);
    const std::uint32_t tag = pass ? kRunPassTag :
        (receipt_written ? kRunFailedTag : kRunReceiptFailedTag);
    return static_cast<jlong>(EncodeRunReturn(tag));
}
