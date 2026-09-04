#include <dlfcn.h>

#include <atomic>
#include <cstdint>

namespace {

constexpr char kNaturalPayload[] =
    "/data/local/tmp/liba9tas_natural_action_replay_v1_review_only.so";
constexpr char kIntervalPayload[] =
    "/data/local/tmp/liba9tas_physics_interval_getter_v2_passive.so";
constexpr char kBarrelPayload[] =
    "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so";

std::atomic<std::int32_t> g_status{0};
std::atomic<std::uintptr_t> g_natural_handle{0};
std::atomic<std::uintptr_t> g_interval_handle{0};
std::atomic<std::uintptr_t> g_barrel_handle{0};

__attribute__((constructor)) void LoadSuccessorPayloadsInGuestContext() {
  g_status.store(1, std::memory_order_release);

  void* natural = dlopen(kNaturalPayload, RTLD_NOW | RTLD_LOCAL);
  g_natural_handle.store(reinterpret_cast<std::uintptr_t>(natural),
                         std::memory_order_release);
  if (natural == nullptr) {
    g_status.store(-1, std::memory_order_release);
    return;
  }

  void* interval = dlopen(kIntervalPayload, RTLD_NOW | RTLD_LOCAL);
  g_interval_handle.store(reinterpret_cast<std::uintptr_t>(interval),
                          std::memory_order_release);
  if (interval == nullptr) {
    g_status.store(-2, std::memory_order_release);
    return;
  }

  void* barrel = dlopen(kBarrelPayload, RTLD_NOW | RTLD_LOCAL);
  g_barrel_handle.store(reinterpret_cast<std::uintptr_t>(barrel),
                        std::memory_order_release);
  g_status.store(barrel != nullptr ? 4 : -3, std::memory_order_release);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_payload_bundle_barrel_successor_status_v1() {
  return g_status.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_payload_bundle_barrel_successor_natural_handle_v1() {
  return g_natural_handle.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_payload_bundle_barrel_successor_interval_handle_v1() {
  return g_interval_handle.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_payload_bundle_barrel_successor_barrel_handle_v1() {
  return g_barrel_handle.load(std::memory_order_acquire);
}
