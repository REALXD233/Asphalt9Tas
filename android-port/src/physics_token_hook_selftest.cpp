#include "physics_token_gate.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
a9tas::PhysicsTokenGate g_gate;
thread_local std::uint64_t g_shadow_token{};
std::atomic<std::uint64_t> g_hook_calls{};
extern "C" std::uintptr_t g_selftest_continue = 0;

extern "C" __attribute__((naked, noinline)) void SelftestTarget() {
    __asm__ volatile(
        // Exact 16-byte prologue from lib+0x38B74DC.
        "sub sp, sp, #0x60\n"
        "stp x21, x20, [sp, #0x40]\n"
        "stp x19, x30, [sp, #0x50]\n"
        "mov x20, x8\n"
        // Minimal body with the same input/output calling convention.
        "mov x19, x1\n"
        "ldr x9, [x19]\n"
        "str x9, [x20]\n"
        "add x0, x9, #7\n"
        "ldp x19, x30, [sp, #0x50]\n"
        "ldp x21, x20, [sp, #0x40]\n"
        "add sp, sp, #0x60\n"
        "ret\n");
}

extern "C" __attribute__((noinline)) const std::uint64_t* SelftestFilter(
    const std::uint64_t* token) {
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    const auto decision = g_gate.Filter(token ? *token : 0);
    g_shadow_token = decision.token_us;
    return &g_shadow_token;
}

extern "C" __attribute__((naked)) void SelftestHookEntry() {
    __asm__ volatile(
        "sub sp, sp, #0x30\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov x20, x8\n"
        "mov x21, x0\n"
        "mov x19, x1\n"
        "mov x0, x19\n"
        "bl SelftestFilter\n"
        "mov x1, x0\n"
        "mov x0, x21\n"
        "mov x8, x20\n"
        "ldp x19, x30, [sp, #0x20]\n"
        "ldp x21, x20, [sp, #0x10]\n"
        "add sp, sp, #0x30\n"
        "adrp x17, :got:g_selftest_continue\n"
        "ldr x17, [x17, #:got_lo12:g_selftest_continue]\n"
        "ldr x17, [x17]\n"
        "br x17\n");
}

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination) {
    constexpr std::uint32_t load_x17 = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xd61f0220;
    std::memcpy(output, &load_x17, 4);
    std::memcpy(output + 4, &branch_x17, 4);
    std::memcpy(output + 8, &destination, 8);
}

bool Install() {
    auto* target = reinterpret_cast<std::uint8_t*>(&SelftestTarget);
    constexpr std::uint8_t expected[16] = {
        0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
        0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
    };
    if (std::memcmp(target, expected, sizeof(expected)) != 0) return false;

    auto* continuation = static_cast<std::uint8_t*>(mmap(
        nullptr, 32, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (continuation == MAP_FAILED) return false;
    std::memcpy(continuation, target, 16);
    AbsoluteJump(continuation + 16,
                 reinterpret_cast<std::uintptr_t>(target + 16));
    __builtin___clear_cache(reinterpret_cast<char*>(continuation),
                            reinterpret_cast<char*>(continuation + 32));
    if (mprotect(continuation, 32, PROT_READ | PROT_EXEC) != 0) return false;
    g_selftest_continue = reinterpret_cast<std::uintptr_t>(continuation);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const auto page_addr = reinterpret_cast<std::uintptr_t>(target) &
                           ~static_cast<std::uintptr_t>(page_size - 1);
    if (mprotect(reinterpret_cast<void*>(page_addr),
                 static_cast<std::size_t>(page_size),
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    std::uint8_t patch[16]{};
    AbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&SelftestHookEntry));
    std::memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(patch)));
    const bool installed = std::memcmp(target, patch, sizeof(patch)) == 0;
    if (mprotect(reinterpret_cast<void*>(page_addr),
                 static_cast<std::size_t>(page_size),
                 PROT_READ | PROT_EXEC) != 0)
        return false;
    return installed;
}

extern "C" __attribute__((naked, noinline)) std::uint64_t InvokeRaw(
    void*, const std::uint64_t*, std::uint64_t*) {
    __asm__ volatile(
        // The ordinary third argument arrives in X2. The real target expects
        // its output pointer in the non-standard X8 register.
        "mov x8, x2\n"
        "b SelftestTarget\n");
}

std::uint64_t Invoke(std::uint64_t input, std::uint64_t* observed) {
    return InvokeRaw(nullptr, &input, observed);
}

void Check(bool condition, const char* name, int* failures) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++*failures;
}
}  // namespace

int main() {
    int failures = 0;
    Check(Install(), "exact_prologue_hook_installed", &failures);
    if (failures) return 1;

    std::uint64_t observed{};
    auto result = Invoke(12345, &observed);
    Check(observed == 12345 && result == 12352,
          "passthrough_preserves_x1_x8_return_and_stack", &failures);

    g_gate.Freeze();
    result = Invoke(99999, &observed);
    Check(observed == 0 && result == 7,
          "frozen_substitutes_zero_through_trampoline", &failures);

    g_gate.RequestSteps();
    result = Invoke(99999, &observed);
    Check(observed == 16666 && result == 16673,
          "single_step_substitutes_fixed_token", &failures);
    result = Invoke(99999, &observed);
    Check(observed == 0 && result == 7,
          "single_step_refreezes_next_call", &failures);

    g_gate.Resume();
    bool stress_ok = true;
    for (std::uint64_t i = 0; i < 100000; ++i) {
        result = Invoke(i, &observed);
        if (observed != i || result != i + 7) {
            stress_ok = false;
            break;
        }
    }
    Check(stress_ok, "one_hundred_thousand_passthrough_calls_stable", &failures);
    Check(g_hook_calls.load() == 100004,
          "hook_call_count_exact", &failures);

    std::printf("SUMMARY failures=%d calls=%llu\n", failures,
                static_cast<unsigned long long>(g_hook_calls.load()));
    return failures == 0 ? 0 : 1;
}
