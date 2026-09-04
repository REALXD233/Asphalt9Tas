#include "final_writer_storage_transaction_v1.h"
#include "final_writer_target_blob_protocol_v1.h"

int main() {
  static_assert(sizeof(a9tas::final_writer_target_blob_v1::Header) == 128);
  static_assert(sizeof(a9tas::final_writer_replay_v1::FrameTarget) == 80);
  a9tas::final_writer_storage_transaction_v1::Layout layout{};
  return a9tas::final_writer_storage_transaction_v1::ValidateLayout(layout, 2)
             ? 1 : 0;
}
