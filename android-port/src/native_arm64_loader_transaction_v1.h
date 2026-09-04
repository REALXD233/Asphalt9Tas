#pragma once

#include <stddef.h>
#include <stdint.h>

namespace a9tas::native_arm64_loader_transaction_v1 {

constexpr size_t kTemporaryStackBytes = 128u * 1024u;
constexpr size_t kPathOffsetFromTop = 0x800u;
constexpr size_t kCallStackOffsetFromTop = 0x1000u;
constexpr uint32_t kBrkInstruction = 0xd4200000u;

constexpr bool IsBrkInstruction(uint32_t instruction) {
  return (instruction & 0xffe0001fu) == kBrkInstruction;
}

struct Layout {
  uint64_t backup_begin{};
  uint64_t backup_end{};
  uint64_t call_sp{};
  uint64_t path_address{};
};

constexpr bool CheckedSub(uint64_t value, uint64_t amount,
                          uint64_t* output) {
  if (!output || value < amount) return false;
  *output = value - amount;
  return true;
}

constexpr bool PrepareLayout(uint64_t original_sp,
                             uint64_t mapping_begin,
                             uint64_t mapping_end,
                             size_t path_size, Layout* output) {
  if (!output || original_sp == 0 || (original_sp & 15u) != 0 ||
      mapping_begin >= mapping_end || original_sp > mapping_end ||
      path_size < 2 || path_size > kPathOffsetFromTop)
    return false;
  Layout result{};
  if (!CheckedSub(original_sp, kTemporaryStackBytes, &result.backup_begin) ||
      !CheckedSub(original_sp, kCallStackOffsetFromTop, &result.call_sp) ||
      !CheckedSub(original_sp, kPathOffsetFromTop, &result.path_address))
    return false;
  result.backup_end = original_sp;
  if (result.backup_begin < mapping_begin || result.backup_end > mapping_end ||
      (result.call_sp & 15u) != 0 ||
      result.path_address + path_size > result.backup_end ||
      result.path_address < result.call_sp)
    return false;
  *output = result;
  return true;
}

struct MutationLedger {
  bool required_threads_stopped{};
  bool selected_thread_stopped{};
  bool stack_mutated{};
  bool trap_mutated{};
  bool registers_mutated{};
  bool stack_restored{};
  bool trap_restored{};
  bool registers_restored{};
};

constexpr bool Restored(const MutationLedger& state) {
  return (!state.stack_mutated || state.stack_restored) &&
         (!state.trap_mutated || state.trap_restored) &&
         (!state.registers_mutated || state.registers_restored);
}

constexpr bool DetachSafe(const MutationLedger& state) {
  return state.required_threads_stopped && state.selected_thread_stopped &&
         Restored(state);
}

constexpr bool ExactTrapStop(int stop_signal, int signal_code,
                             uint64_t observed_pc,
                             uint64_t expected_pc) {
  // SIGTRAP=5. Linux arm64 reports BRK as SI_KERNEL(0x80) or TRAP_BRKPT(1).
  return stop_signal == 5 && (signal_code == 0x80 || signal_code == 1) &&
         observed_pc == expected_pc && (expected_pc & 3u) == 0;
}

}  // namespace a9tas::native_arm64_loader_transaction_v1
