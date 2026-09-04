#include "native_arm64_immutable_trap_resolver_v1.h"

#include "native_arm64_loader_transaction_v1.h"

#if !defined(__aarch64__) || !defined(__ANDROID__)
#error "native_arm64_immutable_trap_resolver_v1 requires Android AArch64"
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace a9tas::native_arm64_immutable_trap_resolver_v1 {
namespace {

namespace transaction = a9tas::native_arm64_loader_transaction_v1;

struct Mapping {
  std::uintptr_t start{};
  std::uintptr_t end{};
  std::uintptr_t offset{};
  unsigned dev_major{};
  unsigned dev_minor{};
  std::uint64_t inode{};
  bool readable{};
  bool writable{};
  bool executable{};
  bool private_mapping{};
  std::string path;
};

std::string Trim(std::string value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.erase(value.begin());
  return value;
}

std::vector<Mapping> ReadMaps(pid_t pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/maps");
  std::vector<Mapping> output;
  std::string line;
  while (std::getline(input, line)) {
    unsigned long long start=0, end=0, offset=0, inode=0;
    unsigned major_id=0, minor_id=0;
    char perms[5]{}, path[2048]{};
    const int fields=std::sscanf(
        line.c_str(), "%llx-%llx %4s %llx %x:%x %llu %2047[^\n]",
        &start, &end, perms, &offset, &major_id, &minor_id, &inode, path);
    if (fields < 7 || start >= end) continue;
    Mapping map{};
    map.start=start; map.end=end; map.offset=offset;
    map.dev_major=major_id; map.dev_minor=minor_id; map.inode=inode;
    map.readable=perms[0]=='r'; map.writable=perms[1]=='w';
    map.executable=perms[2]=='x'; map.private_mapping=perms[3]=='p';
    map.path=fields==8 ? Trim(path) : "";
    output.push_back(std::move(map));
  }
  return output;
}

bool ReadProcessMemory(pid_t pid, std::uintptr_t address, void* output,
                       std::size_t size) {
  if (pid <= 0 || address == 0 || !output || size == 0) return false;
  const std::string path="/proc/"+std::to_string(pid)+"/mem";
  const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);
  if (fd<0) return false;
  std::size_t done=0;
  auto* bytes=static_cast<std::uint8_t*>(output);
  while (done<size) {
    const ssize_t count=pread(fd,bytes+done,size-done,
                              static_cast<off_t>(address+done));
    if (count<=0) { (void)close(fd); return false; }
    done+=static_cast<std::size_t>(count);
  }
  return close(fd)==0;
}

bool AllowedResolverPath(const std::string& path) {
  return path.size()>9 &&
         (path.ends_with("/libdl.so") || path.ends_with("/linker64") ||
          path.ends_with("/libc.so"));
}

bool MapMatchesFile(const Mapping& map, const std::string& path,
                    const struct stat& file) {
  return map.path==path && map.path.find("(deleted)")==std::string::npos &&
         map.private_mapping &&
         map.inode==static_cast<std::uint64_t>(file.st_ino) &&
         map.dev_major==static_cast<unsigned>(major(file.st_dev)) &&
         map.dev_minor==static_cast<unsigned>(minor(file.st_dev));
}

}  // namespace

bool Resolve(pid_t remote_pid, Report* report) {
  if (remote_pid<=0 || !report) return false;
  *report={};
  const std::vector<Mapping> local_maps=ReadMaps(getpid());
  const std::vector<Mapping> remote_maps=ReadMaps(remote_pid);
  if (local_maps.empty() || remote_maps.empty()) return false;
  for (const Mapping& local:local_maps) {
    if (!local.readable || !local.executable || local.writable ||
        !local.private_mapping || !AllowedResolverPath(local.path)) continue;
    struct stat file{};
    if (lstat(local.path.c_str(),&file)!=0 || !S_ISREG(file.st_mode) ||
        S_ISLNK(file.st_mode) || !MapMatchesFile(local,local.path,file) ||
        local.offset>static_cast<std::uintptr_t>(file.st_size) ||
        local.end-local.start>
            static_cast<std::uintptr_t>(file.st_size)-local.offset)
      continue;
    const std::size_t size=static_cast<std::size_t>(local.end-local.start);
    std::vector<std::uint8_t> bytes(size);
    const int fd=open(local.path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) continue;
    std::size_t done=0;
    while (done<size) {
      const ssize_t count=pread(fd,bytes.data()+done,size-done,
                                static_cast<off_t>(local.offset+done));
      if (count<=0) break;
      done+=static_cast<std::size_t>(count);
    }
    const bool closed=close(fd)==0;
    if (!closed || done!=size) continue;
    for (std::size_t offset=0; offset+4<=bytes.size(); offset+=4) {
      std::uint32_t instruction=0;
      std::memcpy(&instruction,bytes.data()+offset,sizeof(instruction));
      if (!transaction::IsBrkInstruction(instruction)) continue;
      const std::uintptr_t file_offset=local.offset+offset;
      std::vector<std::uintptr_t> matches;
      for (const Mapping& remote:remote_maps) {
        if (!MapMatchesFile(remote,local.path,file) || !remote.readable ||
            !remote.executable || remote.writable ||
            file_offset<remote.offset ||
            file_offset>=remote.offset+(remote.end-remote.start)) continue;
        matches.push_back(remote.start+(file_offset-remote.offset));
      }
      if (matches.size()!=1) continue;
      std::uint32_t local_instruction=0, remote_instruction=0;
      if (!ReadProcessMemory(getpid(),local.start+offset,&local_instruction,4) ||
          local_instruction!=instruction ||
          !ReadProcessMemory(remote_pid,matches.front(),&remote_instruction,4) ||
          remote_instruction!=instruction) continue;
      if (local.path.size()>=sizeof(report->module_path)) return false;
      report->address=matches.front();
      report->instruction=instruction;
      std::memcpy(report->module_path,local.path.c_str(),local.path.size()+1);
      return true;
    }
  }
  return false;
}

}  // namespace a9tas::native_arm64_immutable_trap_resolver_v1
