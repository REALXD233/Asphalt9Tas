#pragma once

// Build-dependent Camera Tool identities. The profile contains no live object
// address: RaceView, CameraManager, callback node and vehicle objects are still
// reacquired from the current race lifecycle.

#include "camera_active_state_resolver_v1.h"
#include "camera_manager_graph_v1.h"
#include "vehicle_state_resolver_v1.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace a9tas::camera_build_profile_v1 {

namespace active = camera_active_state_resolver_v1;
namespace manager = camera_manager_graph_v1;
namespace vehicle = vehicle_state_v1;

inline constexpr char kMagic[8] = {'A','9','C','P','R','1','\0','\0'};
inline constexpr std::uint32_t kVersion = 1;

struct alignas(64) Profile {
  char magic[8]{};
  std::uint32_t version{};
  std::uint32_t size{};
  std::uint64_t image_size{};
  std::uint64_t race_view_primary_vptr_rva{};
  std::uint64_t race_view_secondary_vptr_rva{};
  std::uint64_t camera_vptr_minimum_rva{};
  std::uint64_t camera_vptr_maximum_rva{};
  std::uint64_t manager_primary_vptr_rva{};
  std::uint64_t manager_secondary_vptr_rva{};
  std::uint64_t wrapper_vptr_rva{};
  std::uint64_t source_vptr_rva{};
  std::uint64_t callback_node_vptr_rva{};
  std::uint64_t callback_rva{};
  std::uint64_t embedded_vptr_rva{};
  std::uint64_t combined_function_rva{};
  std::uint64_t position_setter_rva{};
  std::uint64_t rotation_setter_rva{};
  std::uint64_t fov_setter_rva{};
  std::uint8_t native_sha256[32]{};
  vehicle::Profile vehicle{};
  std::uint8_t reserved[32]{};
};

static_assert(sizeof(vehicle::Profile) == 336);
static_assert(sizeof(Profile) == 576);

inline bool Nonzero(const std::uint8_t* bytes, std::size_t size) noexcept {
  std::uint8_t combined = 0;
  for (std::size_t index = 0; index < size; ++index) combined |= bytes[index];
  return combined != 0;
}

inline bool ValidRva(std::uint64_t value, std::uint64_t image_size,
                     std::uint64_t alignment = 4) noexcept {
  return value >= 0x1000 && value < image_size &&
         (alignment == 0 || value % alignment == 0);
}

inline bool Valid(const Profile& profile) noexcept {
  if (std::memcmp(profile.magic, kMagic, sizeof(kMagic)) != 0 ||
      profile.version != kVersion || profile.size != sizeof(Profile) ||
      profile.image_size < 0x100000 ||
      !Nonzero(profile.native_sha256, sizeof(profile.native_sha256)))
    return false;
  const std::uint64_t identities[] = {
      profile.race_view_primary_vptr_rva,
      profile.race_view_secondary_vptr_rva,
      profile.camera_vptr_minimum_rva,
      profile.camera_vptr_maximum_rva,
      profile.manager_primary_vptr_rva,
      profile.manager_secondary_vptr_rva,
      profile.wrapper_vptr_rva,
      profile.source_vptr_rva,
      profile.callback_node_vptr_rva,
      profile.callback_rva,
      profile.embedded_vptr_rva,
      profile.combined_function_rva,
      profile.position_setter_rva,
      profile.rotation_setter_rva,
      profile.fov_setter_rva,
  };
  for (const std::uint64_t value : identities)
    if (!ValidRva(value, profile.image_size)) return false;
  if (profile.camera_vptr_minimum_rva >= profile.camera_vptr_maximum_rva)
    return false;
  const auto* vehicle_rvas =
      reinterpret_cast<const std::uintptr_t*>(&profile.vehicle);
  for (std::size_t index = 0; index < 42; ++index)
    if (!ValidRva(vehicle_rvas[index], profile.image_size)) return false;
  for (const std::uint8_t value : profile.reserved)
    if (value != 0) return false;
  return true;
}

inline active::Profile ActiveProfile(const Profile& profile) noexcept {
  return {static_cast<std::uintptr_t>(profile.race_view_primary_vptr_rva),
          static_cast<std::uintptr_t>(profile.race_view_secondary_vptr_rva),
          static_cast<std::uintptr_t>(profile.camera_vptr_minimum_rva),
          static_cast<std::uintptr_t>(profile.camera_vptr_maximum_rva)};
}

inline manager::Profile ManagerProfile(const Profile& profile) noexcept {
  return {static_cast<std::uintptr_t>(profile.manager_primary_vptr_rva),
          static_cast<std::uintptr_t>(profile.manager_secondary_vptr_rva),
          static_cast<std::uintptr_t>(profile.wrapper_vptr_rva),
          static_cast<std::uintptr_t>(profile.source_vptr_rva),
          static_cast<std::uintptr_t>(profile.callback_node_vptr_rva),
          static_cast<std::uintptr_t>(profile.callback_rva)};
}

inline bool Load(const char* path, Profile* output) noexcept {
  if (path == nullptr || output == nullptr) return false;
  struct stat before{};
  if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
      S_ISLNK(before.st_mode) || before.st_size != sizeof(Profile))
    return false;
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat opened{};
  Profile profile{};
  bool ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
            opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
            opened.st_size == before.st_size;
  std::size_t done = 0;
  while (ok && done < sizeof(profile)) {
    const ssize_t count = read(fd, reinterpret_cast<std::uint8_t*>(&profile) + done,
                               sizeof(profile) - done);
    if (count <= 0) { ok = false; break; }
    done += static_cast<std::size_t>(count);
  }
  struct stat after{};
  ok = ok && fstat(fd, &after) == 0 && after.st_dev == opened.st_dev &&
       after.st_ino == opened.st_ino && after.st_size == opened.st_size;
  ok = close(fd) == 0 && ok && Valid(profile);
  if (!ok) return false;
  *output = profile;
  return true;
}

}  // namespace a9tas::camera_build_profile_v1
