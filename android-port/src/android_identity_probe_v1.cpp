#include "fc1_payload_elf_resolver_v1.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void PrintHex(const std::uint8_t digest[32]) {
  for (std::size_t index = 0; index < 32; ++index)
    std::printf("%02x", static_cast<unsigned>(digest[index]));
}

bool ParseHex(const char* text, std::uint8_t output[32]) {
  if (text == nullptr || std::strlen(text) != 64) return false;
  for (std::size_t index = 0; index < 32; ++index) {
    const auto digit = [](char value) -> int {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      return -1;
    };
    const int high = digit(text[index * 2]);
    const int low = digit(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::strcmp(argv[1], "--expect") == 0) {
    std::uint8_t expected[32]{}, observed[32]{};
    if (!ParseHex(argv[2], expected)) return 2;
    const int fd = open(argv[3], O_RDONLY | O_CLOEXEC);
    const bool hashed = fd >= 0 &&
        a9tas::fc1_payload_elf_v1::detail::HashFile(fd, observed);
    if (fd >= 0) close(fd);
    const bool matched = hashed && std::memcmp(expected, observed, 32) == 0;
    std::printf("A9TAS_EXPECT_V1 matched=%d sha256=", matched ? 1 : 0);
    if (hashed) PrintHex(observed);
    else std::printf("unavailable");
    std::printf(" path=%s\n", argv[3]);
    return matched ? 0 : 1;
  }
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: a9tas_identity_probe_v1 FILE... | --expect SHA256 FILE\n");
    return 2;
  }

  bool failed = false;
  for (int index = 1; index < argc; ++index) {
    const int fd = open(argv[index], O_RDONLY | O_CLOEXEC);
    std::uint8_t digest[32]{};
    if (fd < 0 ||
        !a9tas::fc1_payload_elf_v1::detail::HashFile(fd, digest)) {
      if (fd >= 0) close(fd);
      std::fprintf(stderr, "A9TAS_HASH_ERROR_V1\t%s\n", argv[index]);
      failed = true;
      continue;
    }
    close(fd);
    std::printf("A9TAS_HASH_V1\t");
    PrintHex(digest);
    std::printf("\t%s\n", argv[index]);
  }
  return failed ? 1 : 0;
}
