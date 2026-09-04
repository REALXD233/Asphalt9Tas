// Read-only post-mortem inspector for a failed final-writer gate.
// Opens /proc/PID/mem O_RDONLY, resolves the hash-pinned payload, and prints
// the residual control/evidence/frame-0 audit. It never ptraces or writes.

#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "final_writer_replay_elf_resolver_v1.h"

namespace {

template <typename T>
bool ReadValue(int fd, std::uintptr_t address, T* output) {
  return a9tas::fc1_payload_elf_v1::detail::ReadAt(fd, address, output,
                                                   sizeof(*output));
}

}  // namespace

int main(int argc, char** argv) {
  using namespace a9tas::final_writer_replay_v1;
  if (argc != 2 && argc != 3) return 2;
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || *end != '\0' || parsed <= 0 ||
      parsed > INT32_MAX)
    return 2;
  const pid_t pid = static_cast<pid_t>(parsed);
  std::uintptr_t scalar_address = 0;
  if (argc == 3) {
    char* scalar_end = nullptr;
    errno = 0;
    const unsigned long long scalar =
        std::strtoull(argv[2], &scalar_end, 16);
    if (errno != 0 || scalar_end == argv[2] || *scalar_end != '\0' ||
        scalar == 0)
      return 2;
    scalar_address = static_cast<std::uintptr_t>(scalar);
  }
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) {
    std::fprintf(stderr, "open %s failed: %s\n", mem_path,
                 std::strerror(errno));
    return 3;
  }
  a9tas::final_writer_replay_elf_v1::Layout layout{};
  if (!a9tas::final_writer_replay_elf_v1::Resolve(pid, mem, &layout)) {
    close(mem);
    std::fprintf(stderr, "payload resolve failed\n");
    return 4;
  }
  Control control{};
  Evidence evidence{};
  FrameAudit audit{};
  FrameTarget target{};
  std::uintptr_t object_vptr = 0;
  std::int64_t scalar_value = 0;
  const bool ok = ReadValue(mem, layout.control, &control) &&
                  ReadValue(mem, layout.evidence, &evidence) &&
                  ReadValue(mem, layout.audits, &audit) &&
                  ReadValue(mem, layout.targets, &target) &&
                  ReadValue(mem, control.expected_object, &object_vptr) &&
                  (scalar_address == 0 ||
                   ReadValue(mem, scalar_address, &scalar_value));
  close(mem);
  if (!ok) {
    std::fprintf(stderr, "residual snapshot read failed\n");
    return 5;
  }
  const bool immediate_transform_exact =
      std::memcmp(audit.immediate_transform, target.transform,
                  kTransformSize) == 0;
  const bool immediate_linear_exact =
      std::memcmp(audit.immediate_linear, target.linear, kLinearSize) == 0;
  std::printf(
      "FINAL_WRITER_RESIDUAL_V1 pid=%d load_bias=0x%" PRIxPTR
      " wrapper=0x%" PRIxPTR " control=0x%" PRIxPTR
      " audits=0x%" PRIxPTR " evidence=0x%" PRIxPTR "\n",
      static_cast<int>(pid), layout.load_bias, layout.wrapper, layout.control,
      layout.audits, layout.evidence);
  std::printf(
      "control_flags=0x%x frame_count=%u object=0x%" PRIxPTR
      " original_vptr=0x%" PRIxPTR " shadow_vptr=0x%" PRIxPTR
      " observed_object_vptr=0x%" PRIxPTR "\n",
      control.flags, control.frame_count, control.expected_object,
      control.original_vptr, control.shadow_vptr, object_vptr);
  std::printf(
      "evidence entries=%" PRIu64 " original_calls=%" PRIu64
      " clean_returns=%" PRIu64 " equal=%" PRIu64
      " corrected=%" PRIu64 " writes=%" PRIu64 " failures=%" PRIu64
      " recursive=%" PRIu64 " processed=%u status=%d last_object=0x%" PRIxPTR
      " observed_vptr=0x%" PRIxPTR " final_vptr=0x%" PRIxPTR "\n",
      evidence.wrapper_entries, evidence.original_calls, evidence.clean_returns,
      evidence.equal_frames, evidence.corrected_frames,
      evidence.correction_writes, evidence.failures, evidence.recursive_entries,
      evidence.processed_frames, evidence.last_status, evidence.last_object,
      evidence.observed_vptr, evidence.final_vptr);
  std::printf(
      "audit0 frame=%u flags=0x%x immediate_transform_exact=%u"
      " immediate_linear_exact=%u\n",
      audit.frame_index, audit.flags,
      immediate_transform_exact ? 1u : 0u,
      immediate_linear_exact ? 1u : 0u);
  if (scalar_address != 0)
    std::printf("scalar address=0x%" PRIxPTR " signed=%" PRId64
                " hex=0x%" PRIx64 "\n",
                scalar_address, scalar_value,
                static_cast<std::uint64_t>(scalar_value));
  return 0;
}
