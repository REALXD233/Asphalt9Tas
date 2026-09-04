#include <dlfcn.h>

#include <atomic>
#include <cstdint>

namespace {

constexpr char kNaturalPayload[] =
    "/data/local/tmp/liba9tas_natural_action_replay_v1_review_only.so";
constexpr char kControllerPayload[] =
    "/data/local/tmp/liba9tas_controller_shadow_coordinator_v1_build_only.so";

std::atomic<std::int32_t> g_status{0};
std::atomic<std::uintptr_t> g_natural_handle{0};
std::atomic<std::uintptr_t> g_controller_handle{0};

__attribute__((constructor)) void LoadPayloadsInGuestContext() {
  g_status.store(1, std::memory_order_release);
  void* natural = dlopen(kNaturalPayload, RTLD_NOW | RTLD_LOCAL);
  g_natural_handle.store(reinterpret_cast<std::uintptr_t>(natural),
                         std::memory_order_release);
  if (natural == nullptr) {
    g_status.store(-1, std::memory_order_release);
    return;
  }
  void* controller = dlopen(kControllerPayload, RTLD_NOW | RTLD_LOCAL);
  g_controller_handle.store(reinterpret_cast<std::uintptr_t>(controller),
                            std::memory_order_release);
  g_status.store(controller != nullptr ? 2 : -2, std::memory_order_release);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_payload_bundle_status_v1() {
  return g_status.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_payload_bundle_natural_handle_v1() {
  return g_natural_handle.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_payload_bundle_controller_handle_v1() {
  return g_controller_handle.load(std::memory_order_acquire);
}
