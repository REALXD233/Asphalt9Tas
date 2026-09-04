#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "final_writer_replay_protocol_v1.h"

namespace a9tas::final_writer_target_blob_v1 {

inline constexpr char kMagic[8] = {'A', '9', 'F', 'W', 'T', '1', 0, 0};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kHeaderSize = 128;
inline constexpr std::uint32_t kRecordSize = 80;
inline constexpr std::uint32_t kRequiredIntervalUs = 16667;
inline constexpr std::uint32_t kRequiredFlags = 3;
inline constexpr std::uint8_t kSupportedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

#pragma pack(push, 1)
struct Header {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint32_t record_size;
  std::uint32_t frame_count;
  std::uint32_t fixed_interval_us;
  std::uint32_t flags;
  std::uint32_t source_size;
  std::uint32_t reserved_u32;
  std::uint8_t source_sha256[32];
  std::uint8_t build_id[20];
  std::uint8_t reserved[36];
};
#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderSize, "A9FWT1 header ABI");
static_assert(sizeof(a9tas::final_writer_replay_v1::FrameTarget) == kRecordSize,
              "A9FWT1 target ABI");

struct View {
  Header header{};
  const std::uint8_t* records{};
  std::size_t records_size{};
};

inline bool Decode(const std::uint8_t* bytes, std::size_t size,
                   const std::uint8_t expected_source_sha256[32],
                   std::uint32_t expected_source_size, View* output) {
  using a9tas::final_writer_replay_v1::kMaximumFrames;
  if (bytes == nullptr || expected_source_sha256 == nullptr || output == nullptr ||
      size < sizeof(Header))
    return false;
  Header header{};
  std::memcpy(&header, bytes, sizeof(header));
  if (std::memcmp(header.magic, kMagic, 8) != 0 ||
      header.version != kVersion || header.header_size != sizeof(Header) ||
      header.record_size != kRecordSize || header.frame_count < 2 ||
      header.frame_count > kMaximumFrames ||
      header.fixed_interval_us != kRequiredIntervalUs ||
      header.flags != kRequiredFlags ||
      header.source_size != expected_source_size || header.reserved_u32 != 0 ||
      std::memcmp(header.source_sha256, expected_source_sha256, 32) != 0 ||
      std::memcmp(header.build_id, kSupportedBuildId, 20) != 0)
    return false;
  for (const auto value : header.reserved)
    if (value != 0) return false;
  const std::size_t records_size =
      static_cast<std::size_t>(header.frame_count) * kRecordSize;
  if (records_size / kRecordSize != header.frame_count ||
      records_size > SIZE_MAX - sizeof(Header) ||
      size != sizeof(Header) + records_size)
    return false;
  for (std::uint32_t index = 0; index < header.frame_count; ++index) {
    std::uint32_t reserved = 0;
    std::memcpy(&reserved,
                bytes + sizeof(Header) + index * kRecordSize + 76, 4);
    if (reserved != 0) return false;
  }
  *output = View{header, bytes + sizeof(Header), records_size};
  return true;
}

}  // namespace a9tas::final_writer_target_blob_v1
