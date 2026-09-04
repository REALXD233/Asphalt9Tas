#include "src/native_arm64_loader_transaction_v1.h"

namespace tx = a9tas::native_arm64_loader_transaction_v1;

constexpr bool Selftest() {
  tx::Layout layout{};
  if (!tx::PrepareLayout(0x71040000, 0x71000000, 0x71080000, 64, &layout))
    return false;
  if (layout.backup_begin != 0x71020000 ||
      layout.backup_end != 0x71040000 ||
      layout.call_sp != 0x7103f000 ||
      layout.path_address != 0x7103f800)
    return false;
  if (tx::PrepareLayout(0x71040008, 0x71000000, 0x71080000, 64, &layout) ||
      tx::PrepareLayout(0x71010000, 0x71000000, 0x71080000, 64, &layout) ||
      tx::PrepareLayout(0x71040000, 0x71000000, 0x71080000, 0x900, &layout))
    return false;

  tx::MutationLedger ledger{};
  ledger.required_threads_stopped = true;
  ledger.selected_thread_stopped = true;
  ledger.stack_mutated = true;
  ledger.trap_mutated = true;
  ledger.registers_mutated = true;
  if (tx::DetachSafe(ledger)) return false;
  ledger.trap_restored = true;
  ledger.stack_restored = true;
  if (tx::DetachSafe(ledger)) return false;
  ledger.registers_restored = true;
  if (!tx::DetachSafe(ledger)) return false;
  ledger.selected_thread_stopped = false;
  if (tx::DetachSafe(ledger)) return false;

  return tx::IsBrkInstruction(0xd4200000) &&
         tx::IsBrkInstruction(0xd4212340) &&
         !tx::IsBrkInstruction(0xd503201f) &&
         tx::ExactTrapStop(5, 0x80, 0x123400, 0x123400) &&
         tx::ExactTrapStop(5, 1, 0x123400, 0x123400) &&
         !tx::ExactTrapStop(5, 1, 0x123404, 0x123400) &&
         !tx::ExactTrapStop(5, 2, 0x123400, 0x123400);
}

static_assert(Selftest(), "native ARM64 loader transaction selftest failed");

extern "C" int a9tas_native_arm64_loader_transaction_selftest_v1() {
  return Selftest() ? 1 : 0;
}
