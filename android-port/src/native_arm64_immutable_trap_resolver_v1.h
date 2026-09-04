#pragma once

#include <sys/types.h>

#include <cstdint>

namespace a9tas::native_arm64_immutable_trap_resolver_v1 {

struct Report {
  std::uintptr_t address{};
  std::uint32_t instruction{};
  char module_path[1024]{};
};

// Resolves an existing ARM64 BRK instruction from the same exact system ELF
// in this process and the stopped remote process. No remote code is modified.
bool Resolve(pid_t remote_pid, Report* report);

}  // namespace a9tas::native_arm64_immutable_trap_resolver_v1
