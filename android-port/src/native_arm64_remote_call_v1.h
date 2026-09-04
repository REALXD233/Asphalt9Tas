#pragma once

#include <stddef.h>
#include <stdint.h>

namespace a9tas::native_arm64_remote_call_v1 {

struct RegisterImage {
  uint64_t x[31]{};
  uint64_t sp{};
  uint64_t pc{};
  uint64_t pstate{};
};

struct PreparedCall {
  RegisterImage registers{};
  uint64_t function{};
  uint64_t trap{};
};

struct CallReport {
  bool tracee_stopped{};
  bool expected_trap{};
  bool rollback_attempted{};
  bool rollback_succeeded{};
  bool detach_safe{};
  int wait_status{};
  int stop_signal{};
  int signal_code{};
  int error{};
  uint64_t return_value{};
  uint64_t observed_pc{};
};

constexpr bool InstructionAddress(uint64_t value) {
  return value != 0 && (value & 3u) == 0;
}

// A zero return address is a portable, non-mutating fallback for old Android
// system images which do not expose a reusable BRK instruction.  A successful
// callee return then stops at SIGSEGV/SEGV_MAPERR with PC exactly zero.  Any
// fault inside the callee has a different PC and is rejected.
constexpr bool ReturnStopAddress(uint64_t value) {
  return value == 0 || InstructionAddress(value);
}

constexpr bool Prepare(const RegisterImage& original, uint64_t function,
                       uint64_t trap, const uint64_t arguments[6],
                       PreparedCall* output) {
  if (!output || !InstructionAddress(function) || !ReturnStopAddress(trap) ||
      original.sp == 0 || (original.sp & 15u) != 0)
    return false;
  output->registers = original;
  for (size_t index = 0; index < 6; ++index)
    output->registers.x[index] = arguments[index];
  output->registers.x[30] = trap;
  output->registers.pc = function;
  output->function = function;
  output->trap = trap;
  return true;
}

constexpr bool ReturnedAtExactTrap(const RegisterImage& returned,
                                   uint64_t trap) {
  return ReturnStopAddress(trap) && returned.pc == trap;
}

#if defined(__aarch64__) && defined(__ANDROID__)
bool GetRegisters(int tid, RegisterImage* output);
bool SetRegistersExact(int tid, const RegisterImage& value);

// The caller must already own a ptrace stop for tid and must keep every other
// possible user of the target object frozen. No game memory is modified here.
bool CallStoppedThread(int tid, const RegisterImage& original,
                       uint64_t function, uint64_t trap,
                       const uint64_t arguments[6],
                       CallReport* report);
#endif

}  // namespace a9tas::native_arm64_remote_call_v1
