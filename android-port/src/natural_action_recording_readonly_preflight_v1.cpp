// Read-only diagnostic for a passively preloaded recording payload and the
// current NitroService owner.  It performs no ptrace and no target writes.

#define A9TAS_UNIFIED_TICK_EXECUTOR_NO_MAIN
#include "hwbp_unified_tick_executor_v1.cpp"
#include "natural_action_recording_host_v1.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s PID GAME_BASE_HEX\n", argv[0]);
    return 2;
  }
  std::uint64_t pid_value = 0, base_value = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) || pid_value == 0 ||
      pid_value > static_cast<std::uint64_t>(INT32_MAX) || base_value == 0)
    return 2;
  const pid_t pid = static_cast<pid_t>(pid_value);
  const std::uintptr_t base = static_cast<std::uintptr_t>(base_value);
  if (!VerifyTargetBuild(pid, base)) {
    std::fprintf(stderr, "READONLY_PREFLIGHT_FAIL stage=game_identity\n");
    return 3;
  }
  std::uintptr_t final_owner = 0;
  if (!ResolveFinalOwner(pid, base, 0, &final_owner)) {
    std::fprintf(stderr, "READONLY_PREFLIGHT_FAIL stage=final_owner\n");
    return 3;
  }
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return 4;
  a9tas::natural_action_recording_host_v1::Runtime runtime{};
  const char* failure = nullptr;
  const bool ok = a9tas::natural_action_recording_host_v1::Resolve(
      mem, pid, base, final_owner, 900, 1, &runtime, &failure);
  if (!ok) {
    std::vector<a9tas::natural_action_recording_elf_v1::Mapping> payload_maps;
    std::string payload_path;
    std::uintptr_t payload_bias = 0;
    std::uintptr_t storage[4]{};
    const bool maps_ok =
        a9tas::natural_action_recording_elf_v1::detail::ReadMaps(
            pid, &payload_maps, &payload_path, &payload_bias);
    if (maps_ok) {
      const std::uintptr_t locators[4] = {
          payload_bias + a9tas::natural_action_recording_elf_v1::kControlLocatorRva,
          payload_bias + a9tas::natural_action_recording_elf_v1::kEvidenceLocatorRva,
          payload_bias + a9tas::natural_action_recording_elf_v1::kCountsLocatorRva,
          payload_bias + a9tas::natural_action_recording_elf_v1::kShadowLocatorRva,
      };
      for (std::size_t index = 0; index < 4; ++index)
        (void)a9tas::natural_action_recording_host_v1::ReadExact(
            mem, locators[index], &storage[index], sizeof(storage[index]));
    }
    std::fprintf(stderr,
                 "READONLY_PREFLIGHT_FAIL stage=%s owner=0x%" PRIxPTR
                 " maps=%u bias=0x%" PRIxPTR " control=0x%" PRIxPTR
                 " evidence=0x%" PRIxPTR " counts=0x%" PRIxPTR
                 " shadow=0x%" PRIxPTR "\n",
                 failure ? failure : "unknown", final_owner,
                 maps_ok ? 1u : 0u, payload_bias, storage[0], storage[1],
                 storage[2], storage[3]);
    close(mem);
    return 5;
  }
  close(mem);
  std::printf(
      "NATURAL_ACTION_RECORDING_READONLY_PREFLIGHT passed=1 pid=%d "
      "owner=0x%" PRIxPTR " service=0x%" PRIxPTR
      " original_vptr=0x%" PRIxPTR " activate=0x%" PRIxPTR
      " payload=0x%" PRIxPTR " wrapper=0x%" PRIxPTR
      " gameplay_writes=0 ptrace_calls=0\n",
      static_cast<int>(pid), final_owner, runtime.service,
      runtime.original_vptr, runtime.original_activate,
      runtime.payload.load_bias, runtime.payload.wrapper);
  return 0;
}
