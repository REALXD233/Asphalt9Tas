#ifndef A9TAS_G4_DEVICE_PAYLOAD_PATH
#error "A9TAS_G4_DEVICE_PAYLOAD_PATH must be fixed by the build"
#endif
#ifndef A9TAS_G4_PAYLOAD_SHA256
#error "A9TAS_G4_PAYLOAD_SHA256 must be fixed by the build"
#endif
#ifndef A9TAS_G4_PAYLOAD_BUILD_ID
#error "A9TAS_G4_PAYLOAD_BUILD_ID must be fixed by the build"
#endif
#ifndef A9TAS_G4_PAYLOAD_SOURCE_SHA256
#error "A9TAS_G4_PAYLOAD_SOURCE_SHA256 must be fixed by the build"
#endif

#define A9TAS_G4_STRINGIFY_INNER(value) #value
#define A9TAS_G4_STRINGIFY(value) A9TAS_G4_STRINGIFY_INNER(value)

#define A9TAS_ENABLE_SAME_THREAD_PROBE 1
#define A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD 0
#define A9TAS_PAYLOAD_PATH A9TAS_G4_STRINGIFY(A9TAS_G4_DEVICE_PAYLOAD_PATH)
#define A9TAS_SAME_THREAD_PROBE_SYMBOL "a9tas_g4_command_v1"
#define A9TAS_SAME_THREAD_PROBE_SHORTY "JJ"

#include "bootstrap.cpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" __attribute__((naked, noinline, visibility("default"), used)) void
a9tas_bootstrap_g4_tick_coordinator_return_trap_v1() {
  __asm__ volatile("int3\nret\n");
}

extern "C" __attribute__((noinline, visibility("default"), used))
std::uint64_t a9tas_bootstrap_g4_tick_coordinator_calibrate_tid_v1() {
  return static_cast<std::uint64_t>(syscall(__NR_gettid));
}

namespace {
constexpr std::uint64_t kG4LocatorMagic = 0x47345443314c4f43ULL;

struct alignas(8) G4BootstrapLocator {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t size;
  char payload_path[192];
  char payload_sha256[65];
  char payload_build_id[41];
  char payload_source_sha256[65];
  char run_symbol[64];
  char shorty[8];
  const void* stage_address;
  const void* probe_status_address;
  const void* trampoline_address;
  std::uint32_t stage_size;
  std::uint32_t probe_status_size;
  std::uint32_t trampoline_size;
  std::uint32_t reserved;
  const void* return_trap_address;
  const void* calibrate_tid_address;
};

static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<int>) == 4);
static_assert(sizeof(std::atomic<std::uintptr_t>) == 8);
static_assert(offsetof(G4BootstrapLocator, stage_address) == 456);
static_assert(offsetof(G4BootstrapLocator, return_trap_address) == 496);
static_assert(sizeof(G4BootstrapLocator) == 512);
}  // namespace

extern "C" __attribute__((visibility("default"), used)) const
G4BootstrapLocator a9tas_bootstrap_g4_tick_coordinator_locator_v1 = {
    kG4LocatorMagic,
    1,
    sizeof(G4BootstrapLocator),
    A9TAS_G4_STRINGIFY(A9TAS_G4_DEVICE_PAYLOAD_PATH),
    A9TAS_G4_STRINGIFY(A9TAS_G4_PAYLOAD_SHA256),
    A9TAS_G4_STRINGIFY(A9TAS_G4_PAYLOAD_BUILD_ID),
    A9TAS_G4_STRINGIFY(A9TAS_G4_PAYLOAD_SOURCE_SHA256),
    A9TAS_SAME_THREAD_PROBE_SYMBOL,
    A9TAS_SAME_THREAD_PROBE_SHORTY,
    &g_stage,
    &g_same_thread_probe_status,
    &g_same_thread_probe_trampoline,
    sizeof(g_stage),
    sizeof(g_same_thread_probe_status),
    sizeof(g_same_thread_probe_trampoline),
    0,
    reinterpret_cast<const void*>(
        &a9tas_bootstrap_g4_tick_coordinator_return_trap_v1),
    reinterpret_cast<const void*>(
        &a9tas_bootstrap_g4_tick_coordinator_calibrate_tid_v1),
};

// Compatibility locator consumed by the already live-proven early carrier.
extern "C" __attribute__((visibility("default"), used)) const
G4BootstrapLocator a9tas_bootstrap_hook_abi_selftest_habi1_locator = {
    0x48414249314c4f43ULL,
    2,
    sizeof(G4BootstrapLocator),
    A9TAS_G4_STRINGIFY(A9TAS_G4_DEVICE_PAYLOAD_PATH),
    A9TAS_G4_STRINGIFY(A9TAS_G4_PAYLOAD_SHA256),
    A9TAS_G4_STRINGIFY(A9TAS_G4_PAYLOAD_BUILD_ID),
    A9TAS_G4_STRINGIFY(A9TAS_G4_PAYLOAD_SOURCE_SHA256),
    A9TAS_SAME_THREAD_PROBE_SYMBOL,
    A9TAS_SAME_THREAD_PROBE_SHORTY,
    &g_stage,
    &g_same_thread_probe_status,
    &g_same_thread_probe_trampoline,
    sizeof(g_stage),
    sizeof(g_same_thread_probe_status),
    sizeof(g_same_thread_probe_trampoline),
    0,
    reinterpret_cast<const void*>(
        &a9tas_bootstrap_g4_tick_coordinator_return_trap_v1),
    reinterpret_cast<const void*>(
        &a9tas_bootstrap_g4_tick_coordinator_calibrate_tid_v1),
};

