// Passive ARM64 integration skeleton for the two-stage Nitro pre-physics
// mailbox.  Runtime hook installation is deliberately compiled out.
//
// Loading this library initializes an inert mailbox and exports its address for
// offline/bootstrap ABI work.  No thread is created, no game module is scanned,
// no code is patched and no game function can be called by this build.

#include "nitro_prephysics_mailbox_v1.h"

#include <android/log.h>

#include <atomic>
#include <cstdint>

namespace np = a9tas::nitro_prephysics_v1;

namespace {

constexpr const char* kTag = "A9TAS_NITRO_PREPHYS_V1";
constexpr bool kRuntimeArmingCompiled = false;

alignas(64) np::Mailbox g_mailbox{};
alignas(64) np::RuntimeState g_runtime{};
std::atomic<std::int32_t> g_status{0};

// These two bodies compile-check the final ownership split.  They are hidden,
// unreferenced by the passive exports and have no hook entry in this build.
extern "C" __attribute__((visibility("hidden"), noinline)) std::int32_t
OwnerSubmitStageBodyV1() {
    return static_cast<std::int32_t>(np::StageAtSubmit(&g_mailbox, &g_runtime));
}

extern "C" __attribute__((visibility("hidden"), noinline)) std::int32_t
PhysicsExecuteClaimBodyV1(std::uint32_t actual_tid, np::Command* command) {
    return static_cast<std::int32_t>(
        np::ClaimAtExecute(&g_mailbox, &g_runtime, actual_tid, command));
}

extern "C" __attribute__((visibility("hidden"), noinline)) std::int32_t
PhysicsExecuteCompleteBodyV1(std::uint64_t sequence,
                             std::uint32_t calls_completed,
                             bool semantic_ok) {
    return static_cast<std::int32_t>(np::CompleteExecution(
        &g_mailbox, &g_runtime, sequence, calls_completed, semantic_ok));
}

__attribute__((constructor)) void OnLoad() {
    np::InitializeMailbox(&g_mailbox);
    g_status.store(1, std::memory_order_release);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "loaded passive=1 runtime_arming_compiled=%d hooks=0 threads=0 calls=0",
        kRuntimeArmingCompiled ? 1 : 0);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_nitro_prephysics_v1_protocol() {
    return np::kVersion;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_nitro_prephysics_v1_status() {
    return g_status.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_nitro_prephysics_v1_mailbox() {
    return reinterpret_cast<std::uintptr_t>(&g_mailbox);
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_nitro_prephysics_v1_arm() {
    static_assert(!kRuntimeArmingCompiled,
                  "Build-only payload must remain impossible to arm");
    return -100;
}
