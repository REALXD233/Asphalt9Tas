#include "final_writer_transaction_core_v1.h"

using namespace a9tas::final_writer_replay_v1;
using namespace a9tas::final_writer_transaction_core_v1;

int main() {
  static_assert(sizeof(Prepared::shadow) == kPrimaryShadowSize);
  Transaction transaction;
  if (transaction.MarkPrepared() != Result::kOk) return 1;
  if (transaction.MarkPublished() != Result::kOk) return 2;
  if (transaction.MarkInstalled() != Result::kOk) return 3;
  return transaction.phase() == Phase::kInstalled ? 0 : 4;
}
