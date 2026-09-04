#include "final_writer_cursor_binding_v1.h"

using namespace a9tas::final_writer_cursor_binding_v1;

int main() {
  a9tas::final_writer_replay_v1::Evidence evidence{};
  evidence.last_status = a9tas::final_writer_replay_v1::kStatusPassive;
  Binding binding(2);
  if (binding.BeginTick(0, evidence) != Result::kOk) return 1;
  evidence.wrapper_entries = 1;
  evidence.original_calls = 1;
  evidence.clean_returns = 1;
  evidence.equal_frames = 1;
  evidence.processed_frames = 1;
  if (binding.AcknowledgeWriter(0, evidence) != Result::kOk) return 2;
  if (binding.CommitTick(0, evidence) != Result::kOk) return 3;
  return binding.next_index() == 1 && binding.phase() == Phase::kReady ? 0 : 4;
}
