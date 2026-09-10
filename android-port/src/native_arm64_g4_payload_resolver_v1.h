#pragma once

// Exact-build resolver for the already accepted G4 ARM64 payload on a native
// ARM64 Android process. Unlike the x86 NativeBridge controller it needs no
// bootstrap locator or translated guest trampoline: the command export and
// all storage pointers are resolved directly from the loaded ARM64 ELF.

#include "fc1_payload_elf_resolver_v1.h"
#include "g4_g3_adapter_v1.h"
#include "g4_multi_hook_runtime_v1.h"
#include "unified_tick_recording_v1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace a9tas::native_arm64_g4_payload_resolver_v1 {

namespace protocol = a9tas::g4_multi_hook_runtime_v1;
namespace bridge = a9tas::g4_g3_adapter_v1;
namespace recording = a9tas::unified_tick_v1;
namespace action = a9tas::g4_input_action_v1;

inline constexpr char kPayloadBasename[] =
    "liba9tas_g4_multi_hook_runtime_v1.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0xff,0x17,0xfb,0x16,0x9e,0xc3,0x98,0xb7,
    0x8e,0x9c,0x71,0x73,0x74,0xf5,0xf9,0xc5,
    0x0f,0xca,0x42,0xa8,0x47,0x9a,0x38,0x2a,
    0x8f,0xe7,0x78,0x39,0x72,0x29,0xf9,0xc2,
};
inline constexpr char kExpectedBuildId[] =
    "f86c451bd2981c0e4db24a61a81aebaeeef169a9";
inline constexpr std::uint64_t kExpectedFileSize = 0x3F4330;

inline constexpr std::uintptr_t kCommandRva = 0xE78C;
inline constexpr std::size_t kCommandSize = 0x3A48;
inline constexpr std::uintptr_t kControlLocatorRva = 0x23090;
inline constexpr std::uintptr_t kEvidenceLocatorRva = 0x23098;
inline constexpr std::uintptr_t kRuntimeLocatorRva = 0x230A0;
inline constexpr std::uintptr_t kBuildProfileLocatorRva = 0x230A8;
inline constexpr std::uintptr_t kReplayFramesLocatorRva = 0x230B0;
inline constexpr std::uintptr_t kReplayIntervalsLocatorRva = 0x230B8;
inline constexpr std::uintptr_t kRecordedFramesLocatorRva = 0x230C0;
inline constexpr std::uintptr_t kRecordedIntervalsLocatorRva = 0x230C8;

inline constexpr std::uintptr_t kRuntimeStorageRva = 0x23100;
inline constexpr std::uintptr_t kEvidenceStorageRva = 0x3FBAC0;
inline constexpr std::uintptr_t kControlStorageRva = 0x3FBE00;
inline constexpr std::uintptr_t kRecordedIntervalsStorageRva = 0x3FC1C0;
inline constexpr std::uintptr_t kRecordedFramesStorageRva = 0x5731C0;
inline constexpr std::uintptr_t kBuildProfileStorageRva = 0x8BEDC0;
inline constexpr std::uintptr_t kReplayFramesStorageRva = 0x8BEF40;
inline constexpr std::uintptr_t kReplayIntervalsStorageRva = 0xC0AB40;

inline constexpr std::uintptr_t kFinalRwRva = 0x226C0;
inline constexpr std::uintptr_t kFinalRwLogicalEndRva = 0xD81C40;

static_assert(sizeof(protocol::Control) == 0x240);
static_assert(sizeof(protocol::Evidence) == 0x340);
static_assert(sizeof(action::IntervalSampleV1) *
                  protocol::kMaximumIntervalSamples == 0x177000);
static_assert(sizeof(recording::RecordingFrameV1) *
                  protocol::kMaximumFrames == 0x34BC00);
static_assert(kRecordedIntervalsStorageRva + 0x177000 ==
              kRecordedFramesStorageRva);
static_assert(kRecordedFramesStorageRva + 0x34BC00 ==
              kBuildProfileStorageRva);
static_assert(kReplayFramesStorageRva + 0x34BC00 ==
              kReplayIntervalsStorageRva);
static_assert(kReplayIntervalsStorageRva + 0x177000 <=
              kFinalRwLogicalEndRva);

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  std::uint64_t offset{};
  std::uint32_t dev_major{};
  std::uint32_t dev_minor{};
  std::uint64_t inode{};
  char perms[5]{};
  std::string path;
};

struct FileIdentity {
  std::uint32_t dev_major{};
  std::uint32_t dev_minor{};
  std::uint64_t inode{};
  std::string path;
};

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t command{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t runtime{};
  std::uintptr_t build_profile{};
  std::uintptr_t replay_frames{};
  std::uintptr_t replay_intervals{};
  std::uintptr_t recorded_frames{};
  std::uintptr_t recorded_intervals{};
  std::uint8_t file_sha256[32]{};
  char mapped_path[1024]{};
};

namespace detail {

using a9tas::fc1_payload_elf_v1::detail::HashFile;
using a9tas::fc1_payload_elf_v1::detail::ReadAt;

inline bool EndsWithPayload(const std::string& path) {
  const std::size_t length = sizeof(kPayloadBasename) - 1;
  if (path.size() < length) return false;
  const std::size_t offset = path.size() - length;
  return path.compare(offset, length, kPayloadBasename) == 0 &&
         (offset == 0 || path[offset - 1] == '/');
}

inline bool ParseMappingLine(const char* line, Mapping* output) {
  if (!line || !output) return false;
  unsigned long long begin=0,end=0,offset=0,inode=0;
  unsigned major_id=0,minor_id=0;
  char perms[5]{},path[1024]{};
  const int fields=std::sscanf(
      line,"%llx-%llx %4s %llx %x:%x %llu %1023[^\n]",
      &begin,&end,perms,&offset,&major_id,&minor_id,&inode,path);
  if ((fields!=7 && fields!=8) || begin>=end || begin>UINTPTR_MAX ||
      end>UINTPTR_MAX || (perms[0]!='r' && perms[0]!='-') ||
      (perms[1]!='w' && perms[1]!='-') ||
      (perms[2]!='x' && perms[2]!='-') ||
      (perms[3]!='p' && perms[3]!='s')) return false;
  std::string clean=fields==8 ? std::string(path) : std::string();
  while (!clean.empty() && clean.front()==' ') clean.erase(0,1);
  Mapping result{static_cast<std::uintptr_t>(begin),
                 static_cast<std::uintptr_t>(end),offset,major_id,minor_id,
                 inode,{},std::move(clean)};
  std::memcpy(result.perms,perms,4);
  *output=std::move(result);
  return true;
}

inline bool SameFile(const Mapping& mapping,const FileIdentity& identity) {
  return mapping.dev_major==identity.dev_major &&
         mapping.dev_minor==identity.dev_minor &&
         mapping.inode==identity.inode && mapping.path==identity.path;
}

inline bool ReadMappings(pid_t pid,std::vector<Mapping>* output,
                         FileIdentity* identity,std::uintptr_t* bias) {
  if (pid<=0 || !output || !identity || !bias) return false;
  char maps_path[64]{};
  std::snprintf(maps_path,sizeof(maps_path),"/proc/%d/maps",pid);
  FILE* file=std::fopen(maps_path,"re");
  if (!file) return false;
  std::vector<Mapping> maps;
  char line[2048]{};
  bool parse_ok=true;
  while (std::fgets(line,sizeof(line),file)) {
    Mapping mapping{};
    if (!ParseMappingLine(line,&mapping)) { parse_ok=false; break; }
    maps.push_back(std::move(mapping));
  }
  const bool closed=std::fclose(file)==0;
  if (!parse_ok || !closed || maps.empty()) return false;
  FileIdentity selected{};
  std::uintptr_t selected_bias=0;
  std::uint32_t segments=0,offset_zero=0;
  for (const Mapping& mapping:maps) {
    const bool mentions=mapping.path.find(kPayloadBasename)!=std::string::npos;
    if (!EndsWithPayload(mapping.path)) {
      if (mentions) return false;
      continue;
    }
    if (mapping.path.empty() || mapping.path.front()!='/' ||
        mapping.path.find(" (deleted)")!=std::string::npos ||
        mapping.path.size()>=sizeof(Layout::mapped_path) || mapping.inode==0 ||
        (mapping.dev_major==0 && mapping.dev_minor==0)) return false;
    if (segments==0) {
      selected={mapping.dev_major,mapping.dev_minor,mapping.inode,mapping.path};
    } else if (!SameFile(mapping,selected)) {
      return false;
    }
    if (mapping.offset==0) { selected_bias=mapping.begin; ++offset_zero; }
    ++segments;
  }
  if (segments<3 || offset_zero!=1) return false;
  *output=std::move(maps); *identity=std::move(selected);
  *bias=selected_bias;
  return true;
}

inline const Mapping* At(const std::vector<Mapping>& maps,
                         std::uintptr_t address,std::size_t size) {
  if (size==0 || address>UINTPTR_MAX-size) return nullptr;
  for (const Mapping& mapping:maps)
    if (address>=mapping.begin && address+size<=mapping.end) return &mapping;
  return nullptr;
}

inline bool PrivateAnonymousBss(const Mapping& mapping) {
  return (mapping.path.empty() || mapping.path=="[anon:.bss]") &&
         mapping.dev_major==0 && mapping.dev_minor==0 && mapping.inode==0 &&
         mapping.offset==0 && std::memcmp(mapping.perms,"rw-p",4)==0;
}

inline bool WritableRange(const std::vector<Mapping>& maps,
                          const FileIdentity& identity,
                          std::uintptr_t address,std::size_t size,
                          std::uintptr_t logical_begin,
                          std::uintptr_t logical_end) {
  if (size==0 || address<logical_begin || address>UINTPTR_MAX-size ||
      address+size>logical_end) return false;
  const std::uintptr_t end=address+size;
  std::uintptr_t cursor=address;
  while (cursor<end) {
    const Mapping* mapping=At(maps,cursor,1);
    if (!mapping || mapping->end<=cursor ||
        (std::memcmp(mapping->perms,"rw-p",4)!=0) ||
        (!SameFile(*mapping,identity) && !PrivateAnonymousBss(*mapping)))
      return false;
    cursor=std::min(end,mapping->end);
  }
  return true;
}

inline bool Add(std::uintptr_t base,std::uintptr_t rva,
                std::uintptr_t* output) {
  if (!output || base>UINTPTR_MAX-rva) return false;
  *output=base+rva;
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid,int mem,Layout* output,
                    const char** failure_reason=nullptr) {
  const auto fail=[&](const char* reason) {
    if (failure_reason) *failure_reason=reason;
    return false;
  };
  if (failure_reason) *failure_reason="invalid_arguments";
  if (pid<=0 || mem<0 || !output) return fail("invalid_arguments");
  std::vector<Mapping> maps;
  FileIdentity identity{};
  std::uintptr_t bias=0;
  if (!detail::ReadMappings(pid,&maps,&identity,&bias))
    return fail("payload_maps");
  const auto open_mapped = [&](const char* path, bool follow_kernel_link) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC |
                              (follow_kernel_link ? 0 : O_NOFOLLOW));
    if (fd < 0) return fd;
    struct stat mapped{};
    if (fstat(fd, &mapped) == 0 && S_ISREG(mapped.st_mode) &&
        static_cast<std::uint64_t>(mapped.st_ino) == identity.inode &&
        static_cast<std::uint32_t>(major(mapped.st_dev)) == identity.dev_major &&
        static_cast<std::uint32_t>(minor(mapped.st_dev)) == identity.dev_minor) return fd;
    close(fd);
    errno = ESTALE;
    return -1;
  };
  int file=open_mapped(identity.path.c_str(),false);
  if (file < 0) {
    // maps names are in the game's mount namespace, not necessarily su's.
    const std::string process_path = "/proc/" + std::to_string(pid) +
                                     "/root" + identity.path;
    file=open_mapped(process_path.c_str(),false);
  }
  if (file < 0) {
    for (const Mapping& mapping : maps) {
      if (!detail::SameFile(mapping, identity)) continue;
      char mapped_file[128]{};
      std::snprintf(mapped_file,sizeof(mapped_file),"/proc/%d/map_files/%" PRIxPTR
                    "-%" PRIxPTR,pid,mapping.begin,mapping.end);
      // map_files is a kernel-owned symlink. Validate the opened inode/dev/hash
      // below exactly as for the ordinary path, never a same-name replacement.
      file=open_mapped(mapped_file,true);
      if (file >= 0) break;
    }
  }
  if (file<0) return fail("payload_open");
  struct stat info{};
  std::uint8_t hash[32]{};
  const bool file_ok=fstat(file,&info)==0 && S_ISREG(info.st_mode) &&
      static_cast<std::uint64_t>(info.st_size)==kExpectedFileSize &&
      static_cast<std::uint64_t>(info.st_ino)==identity.inode &&
      static_cast<std::uint32_t>(major(info.st_dev))==identity.dev_major &&
      static_cast<std::uint32_t>(minor(info.st_dev))==identity.dev_minor &&
      detail::HashFile(file,hash) &&
      std::memcmp(hash,kExpectedSha256,sizeof(hash))==0;
  const bool closed=close(file)==0;
  if (!file_ok || !closed) return fail("payload_identity");

  Layout layout{};
  layout.load_bias=bias;
  const std::uintptr_t rvas[] = {
      kCommandRva,kControlStorageRva,kEvidenceStorageRva,kRuntimeStorageRva,
      kBuildProfileStorageRva,kReplayFramesStorageRva,
      kReplayIntervalsStorageRva,kRecordedFramesStorageRva,
      kRecordedIntervalsStorageRva};
  std::uintptr_t* addresses[] = {
      &layout.command,&layout.control,&layout.evidence,&layout.runtime,
      &layout.build_profile,&layout.replay_frames,&layout.replay_intervals,
      &layout.recorded_frames,&layout.recorded_intervals};
  for (std::size_t index=0; index<9; ++index)
    if (!detail::Add(bias,rvas[index],addresses[index]))
      return fail("address_overflow");
  std::uintptr_t logical_begin=0,logical_end=0;
  if (!detail::Add(bias,kFinalRwRva,&logical_begin) ||
      !detail::Add(bias,kFinalRwLogicalEndRva,&logical_end))
    return fail("range_overflow");

  const Mapping* command_map=detail::At(maps,layout.command,kCommandSize);
  if (!command_map || !detail::SameFile(*command_map,identity) ||
      std::memcmp(command_map->perms,"r-xp",4)!=0)
    return fail("command_mapping");
  const std::uintptr_t locator_rvas[8] = {
      kControlLocatorRva,kEvidenceLocatorRva,kRuntimeLocatorRva,
      kBuildProfileLocatorRva,kReplayFramesLocatorRva,
      kReplayIntervalsLocatorRva,kRecordedFramesLocatorRva,
      kRecordedIntervalsLocatorRva};
  const std::uintptr_t expected[8] = {
      layout.control,layout.evidence,layout.runtime,layout.build_profile,
      layout.replay_frames,layout.replay_intervals,layout.recorded_frames,
      layout.recorded_intervals};
  for (std::size_t index=0; index<8; ++index) {
    std::uintptr_t locator=0,relocated=0;
    if (!detail::Add(bias,locator_rvas[index],&locator))
      return fail("locator_overflow");
    const Mapping* locator_map=detail::At(maps,locator,8);
    if (!locator_map || !detail::SameFile(*locator_map,identity) ||
        locator_map->perms[0]!='r' ||
        !detail::ReadAt(mem,locator,&relocated,sizeof(relocated)) ||
        relocated!=expected[index])
      return fail("locator_identity");
  }

  const std::size_t sizes[8] = {
      sizeof(protocol::Control),sizeof(protocol::Evidence),sizeof(bridge::StateV1),
      0x180,sizeof(recording::RecordingFrameV1)*protocol::kMaximumFrames,
      sizeof(action::IntervalSampleV1)*protocol::kMaximumIntervalSamples,
      sizeof(recording::RecordingFrameV1)*protocol::kMaximumFrames,
      sizeof(action::IntervalSampleV1)*protocol::kMaximumIntervalSamples};
  for (std::size_t index=0; index<8; ++index)
    if (!detail::WritableRange(maps,identity,expected[index],sizes[index],
                               logical_begin,logical_end))
      return fail("storage_mapping");
  if ((layout.command&3u)!=0 || (layout.control&63u)!=0 ||
      (layout.evidence&63u)!=0 || (layout.runtime&63u)!=0 ||
      (layout.recorded_frames&63u)!=0 ||
      (layout.recorded_intervals&63u)!=0 ||
      (layout.replay_frames&63u)!=0 || (layout.replay_intervals&63u)!=0)
    return fail("storage_alignment");
  std::memcpy(layout.file_sha256,hash,sizeof(hash));
  std::memcpy(layout.mapped_path,identity.path.c_str(),identity.path.size()+1);
  *output=layout;
  if (failure_reason) *failure_reason="none";
  return true;
}

}  // namespace a9tas::native_arm64_g4_payload_resolver_v1
