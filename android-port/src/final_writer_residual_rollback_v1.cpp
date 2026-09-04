// One-shot conditional cleanup for a failed final-writer gate.  This tool
// restores only the vptr recorded by the hash-pinned payload control block,
// and only when no replay frame or permit was consumed.

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "final_writer_replay_elf_resolver_v1.h"

namespace {

bool ReadAt(int fd, std::uintptr_t address, void* output, std::size_t size) {
  return a9tas::fc1_payload_elf_v1::detail::ReadAt(fd, address, output, size);
}

bool WriteVerified(int fd, std::uintptr_t address, const void* input,
                   std::size_t size) {
  const ssize_t written =
      pwrite(fd, input, size, static_cast<off_t>(address));
  if (written != static_cast<ssize_t>(size)) return false;
  std::uint8_t verify[sizeof(std::uintptr_t)]{};
  return size <= sizeof(verify) && ReadAt(fd, address, verify, size) &&
         std::memcmp(verify, input, size) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace a9tas::final_writer_replay_v1;
  if (argc != 2) return 2;
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || *end != '\0' || parsed <= 0 ||
      parsed > INT32_MAX)
    return 2;
  const pid_t pid = static_cast<pid_t>(parsed);
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  const int mem = open(path, O_RDWR | O_CLOEXEC);
  if (mem < 0) return 3;

  a9tas::final_writer_replay_elf_v1::Layout layout{};
  Control control{};
  Evidence evidence{};
  std::uintptr_t observed = 0;
  bool ok = a9tas::final_writer_replay_elf_v1::Resolve(pid, mem, &layout) &&
            ReadAt(mem, layout.control, &control, sizeof(control)) &&
            ReadAt(mem, layout.evidence, &evidence, sizeof(evidence)) &&
            std::memcmp(control.magic, kControlMagic, 8) == 0 &&
            control.version == kProtocolVersion &&
            control.size == sizeof(Control) &&
            control.flags ==
                (kControlConfigured | kControlTargetsLoaded) &&
            control.frame_count == 30 && control.expected_object != 0 &&
            control.original_vptr != 0 && control.shadow_vptr != 0 &&
            control.reserved[0] == kFramePermitDisarmed &&
            control.reserved[1] == 0 &&
            std::memcmp(evidence.magic, kEvidenceMagic, 8) == 0 &&
            evidence.version == kProtocolVersion &&
            evidence.size == sizeof(Evidence) &&
            evidence.wrapper_entries == 0 && evidence.original_calls == 0 &&
            evidence.clean_returns == 0 && evidence.equal_frames == 0 &&
            evidence.corrected_frames == 0 &&
            evidence.correction_writes == 0 && evidence.failures == 0 &&
            evidence.recursive_entries == 0 && evidence.processed_frames == 0 &&
            evidence.last_status == kStatusPassive &&
            ReadAt(mem, control.expected_object, &observed, sizeof(observed));
  if (!ok) {
    close(mem);
    std::fprintf(stderr, "FINAL_WRITER_RESIDUAL_ROLLBACK rejected=identity\n");
    return 4;
  }
  if (observed == control.original_vptr) {
    close(mem);
    std::printf("FINAL_WRITER_RESIDUAL_ROLLBACK already_original=1\n");
    return 0;
  }
  if (observed != control.shadow_vptr ||
      !WriteVerified(mem, control.expected_object, &control.original_vptr,
                     sizeof(control.original_vptr))) {
    close(mem);
    std::fprintf(stderr,
                 "FINAL_WRITER_RESIDUAL_ROLLBACK rejected=vptr_or_write"
                 " observed=0x%" PRIxPTR "\n",
                 observed);
    return 5;
  }
  close(mem);
  std::printf(
      "FINAL_WRITER_RESIDUAL_ROLLBACK restored=1 object=0x%" PRIxPTR
      " shadow=0x%" PRIxPTR " original=0x%" PRIxPTR "\n",
      control.expected_object, control.shadow_vptr, control.original_vptr);
  return 0;
}
