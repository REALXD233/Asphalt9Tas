#include "natural_action_recording_host_v1.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

bool WriteAt(int fd, std::uintptr_t address, const void* data,
             std::size_t size) {
  return pwrite(fd, data, size, static_cast<off_t>(address)) ==
         static_cast<ssize_t>(size);
}

bool ReuseTransactionSelftest() {
  namespace host = a9tas::natural_action_recording_host_v1;
  namespace protocol = a9tas::natural_action_recording_v1;
  char path[] = "/data/local/tmp/a9tas_nar_host_selftest_XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return false;
  unlink(path);
  bool ok = ftruncate(fd, 0x10000) == 0;
  host::Runtime runtime{};
  runtime.service = 0x1000;
  runtime.original_vptr = 0x2000;
  runtime.original_activate = 0xa000;
  runtime.payload.shadow = 0x3000;
  runtime.payload.control = 0x4000;
  runtime.payload.evidence = 0x4100;
  runtime.payload.counts = 0x5000;
  runtime.payload.wrapper = 0x9000;
  runtime.frame_count = 3;
  runtime.session_id = 7;
  runtime.staged = true;
  runtime.armed = true;
  std::uint8_t original_table[protocol::kShadowVtableSize]{};
  std::memcpy(original_table + protocol::kActivateSlotOffset,
              &runtime.original_activate, sizeof(runtime.original_activate));
  protocol::Evidence evidence = host::MakeEvidence();
  evidence.wrapper_entries = 1;
  evidence.original_calls = 1;
  evidence.clean_returns = 1;
  evidence.counted_calls = 1;
  evidence.last_sequence = 1;
  evidence.last_service = runtime.service;
  evidence.last_status = protocol::kCounted;
  std::uint32_t counts[3] = {1, 0, 0};
  const std::uintptr_t live_shadow = runtime.payload.shadow;
  ok = ok && WriteAt(fd, runtime.original_vptr, original_table,
                     sizeof(original_table)) &&
       WriteAt(fd, runtime.service, &live_shadow, sizeof(live_shadow)) &&
       WriteAt(fd, runtime.payload.evidence, &evidence, sizeof(evidence)) &&
       WriteAt(fd, runtime.payload.counts, counts, sizeof(counts));
  host::Result result{};
  ok = ok && host::Finish(fd, &runtime, &result) &&
       result.count_sum == 1 && result.service_restored &&
       result.evidence_exact;
  protocol::Evidence cleared{};
  std::uint32_t cleared_counts[protocol::kMaximumFrames]{};
  ok = ok && host::ReadExact(fd, runtime.payload.evidence, &cleared,
                            sizeof(cleared)) &&
       host::ReadExact(fd, runtime.payload.counts, cleared_counts,
                       sizeof(cleared_counts)) &&
       host::EvidenceIdentity(cleared) && cleared.wrapper_entries == 0;
  for (const auto count : cleared_counts) ok = ok && count == 0;
  runtime.staged = false;
  runtime.armed = false;
  runtime.restored = false;
  ok = ok && host::Stage(fd, &runtime);
  host::Result second{};
  ok = ok && host::Finish(fd, &runtime, &second) &&
       second.service_restored && second.evidence_exact &&
       second.count_sum == 0;
  close(fd);
  return ok;
}

}  // namespace

int main() {
  namespace host = a9tas::natural_action_recording_host_v1;
  namespace protocol = a9tas::natural_action_recording_v1;
  host::Runtime runtime{};
  runtime.service = 0x1000;
  runtime.original_vptr = 0x2000;
  runtime.original_activate = 0x3000;
  runtime.payload.shadow = 0x4000;
  runtime.frame_count = 900;
  runtime.session_id = 7;
  const protocol::Control control = host::MakeControl(
      runtime, protocol::kConfigured | protocol::kInstalled, 1);
  const protocol::Evidence evidence = host::MakeEvidence();
  const bool ok = host::ControlIdentity(control) &&
      host::EvidenceIdentity(evidence) && control.expected_service == 0x1000 &&
      control.original_vptr == 0x2000 && control.original_activate == 0x3000 &&
      control.shadow_vptr == 0x4000 && control.frame_count == 900 &&
      control.session_id == 7 && control.active_sequence == 1 &&
      evidence.last_status == protocol::kPassive && ReuseTransactionSelftest();
  std::printf("NATURAL_ACTION_RECORDING_HOST_SELFTEST passed=%u\n", ok ? 1u : 0u);
  return ok ? 0 : 1;
}
