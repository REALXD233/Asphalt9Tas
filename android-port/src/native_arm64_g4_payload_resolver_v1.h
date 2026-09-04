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
    0xc9,0x20,0xcc,0xec,0x61,0xea,0x9a,0xa1,
    0xf7,0x88,0x9a,0x88,0xb8,0xb5,0xdf,0x8a,
    0x31,0xb5,0x7b,0x5a,0x74,0x5d,0x8a,0x77,
    0x6c,0xf1,0x5c,0x6b,0xd5,0xd2,0xb0,0x23,
};
inline constexpr char kExpectedBuildId[] =
    "a71eecfe9c960050ad8d4ce1f8d398d5e8853028";
inline constexpr std::uint64_t kExpectedFileSize = 0x142D98;

inline constexpr std::uintptr_t kCommandRva = 0xE370;
inline constexpr std::size_t kCommandSize = 0x3A14;
inline constexpr std::uintptr_t kControlLocatorRva = 0x22C10;
inline constexpr std::uintptr_t kEvidenceLocatorRva = 0x22C18;
inline constexpr std::uintptr_t kRuntimeLocatorRva = 0x22C20;
inline constexpr std::uintptr_t kBuildProfileLocatorRva = 0x22C28;
inline constexpr std::uintptr_t kReplayFramesLocatorRva = 0x22C30;
inline constexpr std::uintptr_t kReplayIntervalsLocatorRva = 0x22C38;
inline constexpr std::uintptr_t kRecordedFramesLocatorRva = 0x22C40;
inline constexpr std::uintptr_t kRecordedIntervalsLocatorRva = 0x22C48;

inline constexpr std::uintptr_t kRuntimeStorageRva = 0x22C80;
inline constexpr std::uintptr_t kEvidenceStorageRva = 0x14A540;
inline constexpr std::uintptr_t kControlStorageRva = 0x14A880;
inline constexpr std::uintptr_t kRecordedIntervalsStorageRva = 0x14AC40;
inline constexpr std::uintptr_t kRecordedFramesStorageRva = 0x18AC40;
inline constexpr std::uintptr_t kBuildProfileStorageRva = 0x287E40;
inline constexpr std::uintptr_t kReplayFramesStorageRva = 0x287FC0;
inline constexpr std::uintptr_t kReplayIntervalsStorageRva = 0x3851C0;

inline constexpr std::uintptr_t kFinalRwRva = 0x22240;
inline constexpr std::uintptr_t kFinalRwLogicalEndRva = 0x3C52C0;

static_assert(sizeof(protocol::Control) == 0x240);
static_assert(sizeof(protocol::Evidence) == 0x340);
static_assert(sizeof(action::IntervalSampleV1) *
                  protocol::kMaximumIntervalSamples == 0x40000);
static_assert(sizeof(recording::RecordingFrameV1) *
                  protocol::kMaximumFrames == 0xFD200);
static_assert(kRecordedIntervalsStorageRva + 0x40000 ==
              kRecordedFramesStorageRva);
static_assert(kRecordedFramesStorageRva + 0xFD200 ==
              kBuildProfileStorageRva);
static_assert(kReplayFramesStorageRva + 0xFD200 ==
              kReplayIntervalsStorageRva);
static_assert(kReplayIntervalsStorageRva + 0x40000 <=
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
  const int file=open(identity.path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
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
