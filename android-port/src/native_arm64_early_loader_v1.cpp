#include "native_arm64_loader_transaction_v1.h"
#include "native_arm64_immutable_trap_resolver_v1.h"
#include "native_arm64_remote_call_v1.h"

#if !defined(__aarch64__) || !defined(__ANDROID__)
#error "native_arm64_early_loader_v1 must be built for Android AArch64"
#endif

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef A9TAS_NATIVE_ARM64_PAYLOAD_PATH
#error "A9TAS_NATIVE_ARM64_PAYLOAD_PATH must be fixed by the build"
#endif
#ifndef A9TAS_NATIVE_ARM64_PAYLOAD_SHA256
#error "A9TAS_NATIVE_ARM64_PAYLOAD_SHA256 must be fixed by the build"
#endif
#ifndef A9TAS_NATIVE_ARM64_PAYLOAD_BASENAME
#define A9TAS_NATIVE_ARM64_PAYLOAD_BASENAME liba9tas_g4_multi_hook_runtime_v1.so
#endif

#define A9TAS_STRING_INNER(value) #value
#define A9TAS_STRING(value) A9TAS_STRING_INNER(value)

namespace {

namespace call = a9tas::native_arm64_remote_call_v1;
namespace trap_resolver =
    a9tas::native_arm64_immutable_trap_resolver_v1;
namespace tx = a9tas::native_arm64_loader_transaction_v1;

constexpr const char* kPayloadPath =
    A9TAS_STRING(A9TAS_NATIVE_ARM64_PAYLOAD_PATH);
constexpr const char* kPayloadSha256 =
    A9TAS_STRING(A9TAS_NATIVE_ARM64_PAYLOAD_SHA256);
constexpr auto kProcessWait = std::chrono::seconds(60);
constexpr auto kStopWait = std::chrono::seconds(4);

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

struct PinnedFile {
  struct stat status{};
  std::string sha256;
};

struct Sha256 {
  std::uint32_t state[8]{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::uint64_t total{};
  std::uint8_t buffer[64]{};
  std::size_t buffered{};

  static std::uint32_t R(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
  }
  void Transform(const std::uint8_t block[64]) {
    static constexpr std::uint32_t constants[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::uint32_t words[64]{};
    for (std::size_t index = 0; index < 16; ++index)
      words[index] = (static_cast<std::uint32_t>(block[index*4]) << 24) |
                     (static_cast<std::uint32_t>(block[index*4+1]) << 16) |
                     (static_cast<std::uint32_t>(block[index*4+2]) << 8) |
                     static_cast<std::uint32_t>(block[index*4+3]);
    for (std::size_t index = 16; index < 64; ++index) {
      const std::uint32_t s0 = R(words[index-15],7) ^ R(words[index-15],18) ^
                               (words[index-15] >> 3);
      const std::uint32_t s1 = R(words[index-2],17) ^ R(words[index-2],19) ^
                               (words[index-2] >> 10);
      words[index] = words[index-16] + s0 + words[index-7] + s1;
    }
    std::uint32_t a=state[0],b=state[1],c=state[2],d=state[3];
    std::uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
    for (std::size_t index = 0; index < 64; ++index) {
      const std::uint32_t s1=R(e,6)^R(e,11)^R(e,25);
      const std::uint32_t choice=(e&f)^(~e&g);
      const std::uint32_t t1=h+s1+choice+constants[index]+words[index];
      const std::uint32_t s0=R(a,2)^R(a,13)^R(a,22);
      const std::uint32_t majority=(a&b)^(a&c)^(b&c);
      const std::uint32_t t2=s0+majority;
      h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
  }
  void Update(const void* raw, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(raw);
    total += size;
    while (size != 0) {
      const std::size_t take = std::min(size, sizeof(buffer)-buffered);
      std::memcpy(buffer+buffered,bytes,take);
      buffered+=take; bytes+=take; size-=take;
      if (buffered==sizeof(buffer)) { Transform(buffer); buffered=0; }
    }
  }
  std::array<std::uint8_t,32> Finish() {
    const std::uint64_t bits=total*8;
    const std::uint8_t marker=0x80,zero=0;
    Update(&marker,1);
    while (buffered!=56) Update(&zero,1);
    std::uint8_t length[8]{};
    for (int index=0; index<8; ++index)
      length[7-index]=static_cast<std::uint8_t>(bits>>(index*8));
    Update(length,sizeof(length));
    std::array<std::uint8_t,32> result{};
    for (std::size_t index=0; index<8; ++index) {
      result[index*4]=static_cast<std::uint8_t>(state[index]>>24);
      result[index*4+1]=static_cast<std::uint8_t>(state[index]>>16);
      result[index*4+2]=static_cast<std::uint8_t>(state[index]>>8);
      result[index*4+3]=static_cast<std::uint8_t>(state[index]);
    }
    return result;
  }
};

std::string Hex(const std::array<std::uint8_t,32>& digest) {
  constexpr char digits[]="0123456789abcdef";
  std::string output(64,'0');
  for (std::size_t index=0; index<digest.size(); ++index) {
    output[index*2]=digits[digest[index]>>4];
    output[index*2+1]=digits[digest[index]&15u];
  }
  return output;
}

bool ValidPayloadPath(const char* path) {
  if (!path || path[0]!='/' || std::strlen(path)>=1024) return false;
  const std::string value(path);
  constexpr char basename[]=A9TAS_STRING(A9TAS_NATIVE_ARM64_PAYLOAD_BASENAME);
  const std::size_t suffix=sizeof(basename)-1;
  if (value.size()<suffix || value.compare(value.size()-suffix,suffix,basename)!=0 ||
      (value.size()>suffix && value[value.size()-suffix-1]!='/')) return false;
  return value.find("//")==std::string::npos &&
         value.find("/../")==std::string::npos &&
         value.find("/./")==std::string::npos &&
         !value.ends_with("/..") && !value.ends_with("/.") &&
         value.find('\n')==std::string::npos && value.find('\r')==std::string::npos;
}

bool ReadPinnedPayload(const char* payload_path,PinnedFile* output) {
  if (!output || !ValidPayloadPath(payload_path) ||
      std::strlen(kPayloadSha256)!=64) return false;
  struct stat before{};
  if (lstat(payload_path,&before)!=0 || !S_ISREG(before.st_mode) ||
      S_ISLNK(before.st_mode) || before.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr)))
    return false;
  const int fd=open(payload_path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  if (fd<0) return false;
  struct stat opened{};
  bool ok=fstat(fd,&opened)==0 && opened.st_dev==before.st_dev &&
          opened.st_ino==before.st_ino && opened.st_size==before.st_size;
  Sha256 hash;
  Elf64_Ehdr header{};
  std::array<std::uint8_t,64*1024> bytes{};
  std::size_t total=0;
  while (ok) {
    const ssize_t count=read(fd,bytes.data(),bytes.size());
    if (count<0) { if (errno==EINTR) continue; ok=false; break; }
    if (count==0) break;
    if (total<sizeof(header)) {
      const std::size_t copy=std::min<std::size_t>(sizeof(header)-total,
                                                  static_cast<std::size_t>(count));
      std::memcpy(reinterpret_cast<std::uint8_t*>(&header)+total,bytes.data(),copy);
    }
    total+=static_cast<std::size_t>(count);
    hash.Update(bytes.data(),static_cast<std::size_t>(count));
  }
  struct stat after{};
  ok=ok && fstat(fd,&after)==0 && after.st_dev==opened.st_dev &&
     after.st_ino==opened.st_ino && after.st_size==opened.st_size;
  ok=close(fd)==0 && ok && total==static_cast<std::size_t>(opened.st_size);
  const std::string digest=ok ? Hex(hash.Finish()) : "";
  ok=ok && digest==kPayloadSha256 &&
     std::memcmp(header.e_ident,ELFMAG,SELFMAG)==0 &&
     header.e_ident[EI_CLASS]==ELFCLASS64 &&
     header.e_ident[EI_DATA]==ELFDATA2LSB && header.e_type==ET_DYN &&
     header.e_machine==EM_AARCH64;
  if (!ok) return false;
  output->status=after;
  output->sha256=digest;
  return true;
}

std::string Trim(std::string value) {
  while (!value.empty() && (value.front()==' ' || value.front()=='\t'))
    value.erase(value.begin());
  return value;
}

std::vector<Mapping> ReadMaps(pid_t pid) {
  std::ifstream input("/proc/"+std::to_string(pid)+"/maps");
  std::vector<Mapping> output;
  std::string line;
  while (std::getline(input,line)) {
    unsigned long long start=0,end=0,offset=0,inode=0;
    unsigned major_id=0,minor_id=0;
    char perms[5]{},path[2048]{};
    const int fields=std::sscanf(line.c_str(),
        "%llx-%llx %4s %llx %x:%x %llu %2047[^\n]",
        &start,&end,perms,&offset,&major_id,&minor_id,&inode,path);
    if (fields<7 || start>=end) continue;
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

const Mapping* FindMap(const std::vector<Mapping>& maps,
                       std::uintptr_t address,std::size_t size=1) {
  if (size==0 || address>UINTPTR_MAX-size) return nullptr;
  for (const Mapping& map:maps)
    if (address>=map.start && address+size<=map.end) return &map;
  return nullptr;
}

bool MapMatchesFile(const Mapping& map,const char* path,const struct stat& file) {
  return map.path==path && map.path.find("(deleted)")==std::string::npos &&
         map.private_mapping && map.inode==static_cast<std::uint64_t>(file.st_ino) &&
         map.dev_major==static_cast<unsigned>(major(file.st_dev)) &&
         map.dev_minor==static_cast<unsigned>(minor(file.st_dev));
}

bool PayloadPathMatches(const std::string& observed,const char* requested) {
  if (!requested) return false;
  const std::string expected(requested);
  if (observed==expected) return true;
  constexpr char user_zero[]="/data/user/0/";
  constexpr char legacy[]="/data/data/";
  if (!expected.starts_with(user_zero) || !observed.starts_with(legacy))
    return false;
  return expected.substr(sizeof(user_zero)-1)==
         observed.substr(sizeof(legacy)-1);
}

bool HasExactPayloadMap(const std::vector<Mapping>& maps,const char* payload_path,
                        const PinnedFile& payload) {
  int offset_zero=0;
  for (const Mapping& map:maps)
    if (map.offset==0 && map.readable &&
        PayloadPathMatches(map.path,payload_path) &&
        map.path.find("(deleted)")==std::string::npos &&
        map.private_mapping &&
        map.inode==static_cast<std::uint64_t>(payload.status.st_ino) &&
        map.dev_major==static_cast<unsigned>(major(payload.status.st_dev)) &&
        map.dev_minor==static_cast<unsigned>(minor(payload.status.st_dev)))
      ++offset_zero;
  return offset_zero==1;
}

std::uint64_t StartTicks(pid_t pid) {
  std::ifstream input("/proc/"+std::to_string(pid)+"/stat");
  std::string content((std::istreambuf_iterator<char>(input)),{});
  const std::size_t close=content.rfind(')');
  if (close==std::string::npos || close+2>=content.size()) return 0;
  std::istringstream fields(content.substr(close+2));
  std::string field;
  for (int index=3; index<=22; ++index) {
    if (!(fields>>field)) return 0;
    if (index==22) {
      char* end=nullptr; errno=0;
      const unsigned long long value=std::strtoull(field.c_str(),&end,10);
      return errno==0 && end && *end=='\0' ? value : 0;
    }
  }
  return 0;
}

int TracerPid(pid_t pid) {
  std::ifstream input("/proc/"+std::to_string(pid)+"/status");
  std::string line;
  while (std::getline(input,line)) {
    int value=-1;
    if (std::sscanf(line.c_str(),"TracerPid:%d",&value)==1) return value;
  }
  return -1;
}

bool ValidProcessName(const char* value) {
  if (!value) return false;
  const std::size_t length=std::strlen(value);
  if (length==0 || length>=192) return false;
  for (std::size_t index=0; index<length; ++index) {
    const unsigned char c=static_cast<unsigned char>(value[index]);
    if (!(std::isalnum(c) || c=='.' || c=='_' || c==':' || c=='-')) return false;
  }
  return true;
}

pid_t FindProcess(const char* process_name) {
  DIR* directory=opendir("/proc");
  if (!directory) return 0;
  std::vector<pid_t> matches;
  while (dirent* entry=readdir(directory)) {
    char* end=nullptr;
    const long raw=std::strtol(entry->d_name,&end,10);
    if (raw<=0 || end==entry->d_name || *end!='\0') continue;
    const std::string path=std::string("/proc/")+entry->d_name+"/cmdline";
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if (fd<0) continue;
    std::array<char,256> command{};
    const ssize_t count=read(fd,command.data(),command.size());
    (void)close(fd);
    const std::size_t expected=std::strlen(process_name);
    const std::size_t first=count>0 ? strnlen(command.data(),static_cast<std::size_t>(count)) : 0;
    if (first==expected && std::memcmp(command.data(),process_name,expected)==0)
      matches.push_back(static_cast<pid_t>(raw));
  }
  closedir(directory);
  return matches.size()==1 ? matches.front() : 0;
}

bool HasGameLibrary(const std::vector<Mapping>& maps) {
  return std::any_of(maps.begin(),maps.end(),[](const Mapping& map) {
    return map.path.find("/libAsphalt9.so")!=std::string::npos && map.readable;
  });
}

bool WaitForGame(const char* process_name,pid_t* pid,std::uint64_t* ticks,
                 std::vector<Mapping>* maps) {
  const auto deadline=std::chrono::steady_clock::now()+kProcessWait;
  while (std::chrono::steady_clock::now()<deadline) {
    const pid_t candidate=FindProcess(process_name);
    if (candidate>0 && TracerPid(candidate)==0) {
      const std::uint64_t identity=StartTicks(candidate);
      std::vector<Mapping> observed=ReadMaps(candidate);
      if (identity!=0 && HasGameLibrary(observed)) {
        *pid=candidate; *ticks=identity; *maps=std::move(observed); return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  errno=ETIMEDOUT;
  return false;
}

bool WaitStopped(pid_t tid,int* status) {
  const auto deadline=std::chrono::steady_clock::now()+kStopWait;
  while (std::chrono::steady_clock::now()<deadline) {
    int observed=0;
    const pid_t waited=waitpid(tid,&observed,WNOHANG|__WALL);
    if (waited==tid) { if (status) *status=observed; return WIFSTOPPED(observed); }
    if (waited==-1 && errno!=EINTR) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  errno=ETIMEDOUT;
  return false;
}

std::string ThreadName(pid_t pid,pid_t tid) {
  std::ifstream input("/proc/"+std::to_string(pid)+"/task/"+
                      std::to_string(tid)+"/comm");
  std::string name;
  std::getline(input,name);
  if (!name.empty() && name.back()=='\r') name.pop_back();
  return name;
}

pid_t ThreadGroupId(pid_t tid) {
  std::ifstream input("/proc/"+std::to_string(tid)+"/status");
  std::string line;
  while (std::getline(input,line)) {
    int value=0;
    if (std::sscanf(line.c_str(),"Tgid:%d",&value)==1) return value;
  }
  return 0;
}

pid_t UniqueSignalCatcher(pid_t pid) {
  const std::string path="/proc/"+std::to_string(pid)+"/task";
  DIR* directory=opendir(path.c_str());
  if (!directory) return 0;
  std::vector<pid_t> matches;
  while (dirent* entry=readdir(directory)) {
    char* end=nullptr;
    const long value=std::strtol(entry->d_name,&end,10);
    if (value<=0 || end==entry->d_name || *end!='\0') continue;
    const pid_t tid=static_cast<pid_t>(value);
    if (ThreadName(pid,tid)=="Signal Catcher" && ThreadGroupId(tid)==pid)
      matches.push_back(tid);
  }
  closedir(directory);
  if (matches.size()!=1) return 0;
  const pid_t tid=matches.front();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return ThreadName(pid,tid)=="Signal Catcher" && ThreadGroupId(tid)==pid
      ? tid : 0;
}

bool AttachSingle(pid_t tid) {
  if (ptrace(PTRACE_ATTACH,tid,nullptr,nullptr)==-1) return false;
  int status=0;
  return WaitStopped(tid,&status) && WSTOPSIG(status)==SIGSTOP &&
         ptrace(PTRACE_SETOPTIONS,tid,nullptr,
                reinterpret_cast<void*>(PTRACE_O_EXITKILL))!=-1;
}

bool KillUncertain(pid_t pid) {
  if (pid<=0) return false;
  (void)kill(pid,SIGKILL);
  for (int index=0; index<100; ++index) {
    std::ifstream stat("/proc/"+std::to_string(pid)+"/stat");
    std::string content((std::istreambuf_iterator<char>(stat)),{});
    const std::size_t close=content.rfind(')');
    if (content.empty() || close==std::string::npos || close+2>=content.size() ||
        content[close+2]=='Z') return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool ReadProcessMemory(pid_t pid,std::uintptr_t address,void* output,std::size_t size) {
  const std::string path="/proc/"+std::to_string(pid)+"/mem";
  const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);
  if (fd<0) return false;
  std::size_t done=0;
  auto* bytes=static_cast<std::uint8_t*>(output);
  while (done<size) {
    const ssize_t count=pread(fd,bytes+done,size-done,
                              static_cast<off_t>(address+done));
    if (count<=0) { close(fd); return false; }
    done+=static_cast<std::size_t>(count);
  }
  return close(fd)==0;
}

bool WriteProcessMemory(pid_t pid,std::uintptr_t address,const void* input,std::size_t size) {
  const std::string path="/proc/"+std::to_string(pid)+"/mem";
  const int fd=open(path.c_str(),O_WRONLY|O_CLOEXEC);
  if (fd<0) return false;
  std::size_t done=0;
  const auto* bytes=static_cast<const std::uint8_t*>(input);
  while (done<size) {
    const ssize_t count=pwrite(fd,bytes+done,size-done,
                               static_cast<off_t>(address+done));
    if (count<=0) { close(fd); return false; }
    done+=static_cast<std::size_t>(count);
  }
  return close(fd)==0;
}

bool WriteAndReadback(pid_t pid,std::uintptr_t address,const void* input,
                      std::size_t size) {
  std::vector<std::uint8_t> observed(size);
  return WriteProcessMemory(pid,address,input,size) &&
         ReadProcessMemory(pid,address,observed.data(),size) &&
         std::memcmp(observed.data(),input,size)==0;
}

bool AllowedResolverPath(const std::string& path) {
  return path.size()>9 &&
         (path.ends_with("/libdl.so") || path.ends_with("/linker64") ||
          path.ends_with("/libc.so"));
}

bool ResolveRemoteDlopen(const std::vector<Mapping>& remote_maps,
                         std::uintptr_t* output,std::string* module_path) {
  void* symbol=dlsym(RTLD_DEFAULT,"dlopen");
  if (!symbol || !output || !module_path) return false;
  const std::uintptr_t address=reinterpret_cast<std::uintptr_t>(symbol);
  const std::vector<Mapping> local_maps=ReadMaps(getpid());
  const Mapping* local=FindMap(local_maps,address);
  if (!local || !local->readable || !local->executable || local->writable ||
      !local->private_mapping || !AllowedResolverPath(local->path)) return false;
  struct stat file{};
  if (lstat(local->path.c_str(),&file)!=0 || !S_ISREG(file.st_mode) ||
      S_ISLNK(file.st_mode) || !MapMatchesFile(*local,local->path.c_str(),file))
    return false;
  const std::uintptr_t file_offset=local->offset+(address-local->start);
  std::vector<std::uintptr_t> matches;
  for (const Mapping& map:remote_maps) {
    if (!MapMatchesFile(map,local->path.c_str(),file) || !map.readable ||
        !map.executable || map.writable || file_offset<map.offset ||
        file_offset>=map.offset+(map.end-map.start)) continue;
    matches.push_back(map.start+(file_offset-map.offset));
  }
  if (matches.size()!=1) return false;
  *output=matches.front(); *module_path=local->path; return true;
}

int Fail(const char* stage,int code,bool killed=false) {
  std::fprintf(stderr,
      "NATIVE_ARM64_EARLY_LOADER passed=0 stage=%s code=%d errno=%d process_killed=%d\n",
      stage,code,errno,killed?1:0);
  return code;
}

int FailTransaction(const char* detail,int saved_errno,bool killed,
                    bool clean_detach,const call::CallReport& report,
                    const tx::MutationLedger& ledger,
                    bool payload_mapped,bool used_null_return) {
  std::fprintf(stderr,
      "NATIVE_ARM64_EARLY_LOADER passed=0 stage=transaction code=8 "
      "detail=%s errno=%d process_killed=%d clean_detach=%d "
      "payload_mapped=%d return_stop=%s call_stopped=%d expected_stop=%d "
      "rollback=%d stop_signal=%d signal_code=%d call_errno=%d "
      "return_value=0x%llx observed_pc=0x%llx "
      "ledger=%d%d%d%d%d%d%d%d\n",
      detail,saved_errno,killed?1:0,clean_detach?1:0,
      payload_mapped?1:0,used_null_return?"null_segv":"immutable_brk",
      report.tracee_stopped?1:0,report.expected_trap?1:0,
      report.rollback_succeeded?1:0,report.stop_signal,report.signal_code,
      report.error,static_cast<unsigned long long>(report.return_value),
      static_cast<unsigned long long>(report.observed_pc),
      ledger.required_threads_stopped?1:0,ledger.selected_thread_stopped?1:0,
      ledger.stack_mutated?1:0,ledger.stack_restored?1:0,
      ledger.registers_mutated?1:0,ledger.registers_restored?1:0,
      ledger.trap_mutated?1:0,ledger.trap_restored?1:0);
  return 8;
}

}  // namespace

int main(int argc,char** argv) {
  if ((argc!=2 && argc!=3) || !ValidProcessName(argv[1])) return Fail("usage",2);
  const char* payload_path=argc==3 ? argv[2] : kPayloadPath;
  if (!ValidPayloadPath(payload_path)) return Fail("payload_path",2);
  pid_t pid=0;
  std::uint64_t start_ticks=0;
  std::vector<Mapping> maps;
  if (!WaitForGame(argv[1],&pid,&start_ticks,&maps)) return Fail("game_window",4);

  // On some rooted/virtualized Android builds the su namespace cannot see the
  // app's /data/user mount, while the game process itself can.  Resolve the
  // immutable payload identity through the selected process root in that case,
  // but keep payload_path unchanged for the remote dlopen call and map receipt.
  // This binds both views to the same inode without leaking an old PID pathname
  // into the newly started game process.
  PinnedFile payload{};
  std::string host_payload_path(payload_path);
  if (!ReadPinnedPayload(host_payload_path.c_str(),&payload)) {
    host_payload_path="/proc/"+std::to_string(pid)+"/root"+payload_path;
    if (!ReadPinnedPayload(host_payload_path.c_str(),&payload))
      return Fail("payload_identity",3);
  }
  if (HasExactPayloadMap(maps,payload_path,payload)) return Fail("already_loaded",5);

  const pid_t tid=UniqueSignalCatcher(pid);
  if (tid<=0 || StartTicks(pid)!=start_ticks || TracerPid(pid)!=0)
    return Fail("signal_catcher",6);
  if (!AttachSingle(tid)) {
    const bool killed=KillUncertain(pid);
    return Fail("attach",7,killed);
  }

  tx::MutationLedger ledger{};
  ledger.required_threads_stopped=true;
  ledger.selected_thread_stopped=true;
  const char* transaction_detail="runtime_identity";
  int transaction_errno=0;
  bool uncertain=StartTicks(pid)!=start_ticks || ThreadGroupId(tid)!=pid ||
                  ThreadName(pid,tid)!="Signal Catcher";
  call::RegisterImage original{};
  std::uintptr_t remote_dlopen=0,immutable_trap=0;
  std::string resolver_module,trap_module;
  trap_resolver::Report trap_report{};
  if (!uncertain && !call::GetRegisters(tid,&original)) {
    transaction_detail="register_read"; transaction_errno=errno; uncertain=true;
  }
  if (!uncertain && !ResolveRemoteDlopen(maps,&remote_dlopen,&resolver_module)) {
    transaction_detail="dlopen_resolve"; transaction_errno=errno; uncertain=true;
  }
  bool used_null_return=false;
  if (!uncertain && !trap_resolver::Resolve(pid,&trap_report)) {
    // Android 7 vendor images may have no reusable BRK in the exact shared
    // system mappings. LR=0 gives an equally exact, non-mutating return stop.
    trap_report={};
    used_null_return=true;
  }
  if (!uncertain) {
    immutable_trap=trap_report.address;
    trap_module=used_null_return ? "null-return-fallback" : trap_report.module_path;
  }

  const Mapping* function_map=!uncertain ? FindMap(maps,remote_dlopen,4) : nullptr;
  const Mapping* trap_map=!uncertain ? FindMap(maps,immutable_trap,4) : nullptr;
  const Mapping* stack_map=!uncertain && original.sp>0 ? FindMap(maps,original.sp-1) : nullptr;
  tx::Layout layout{};
  const std::size_t path_size=std::strlen(payload_path)+1;
  if (!uncertain &&
      (!function_map || !function_map->readable || !function_map->executable ||
       function_map->writable || (!used_null_return &&
       (!trap_map || !trap_map->readable || !trap_map->executable ||
        trap_map->writable)) || !stack_map ||
       !stack_map->readable || !stack_map->writable ||
       !stack_map->private_mapping ||
       !tx::PrepareLayout(original.sp,stack_map->start,stack_map->end,
                           path_size,&layout)))
    { transaction_detail="stack_layout"; transaction_errno=errno; uncertain=true; }

  std::vector<std::uint8_t> stack_backup(tx::kTemporaryStackBytes);
  if (!uncertain && !ReadProcessMemory(pid,layout.backup_begin,
                                       stack_backup.data(),stack_backup.size()))
    { transaction_detail="stack_backup"; transaction_errno=errno; uncertain=true; }
  if (!uncertain) {
    if (!WriteAndReadback(pid,layout.path_address,payload_path,path_size))
      { transaction_detail="path_write"; transaction_errno=errno; uncertain=true; }
    else
      ledger.stack_mutated=true;
  }

  call::CallReport report{};
  const std::uint64_t arguments[6]{layout.path_address,RTLD_NOW|RTLD_LOCAL,0,0,0,0};
  call::RegisterImage call_origin=original;
  call_origin.sp=layout.call_sp;
  if (!uncertain) {
    // SetRegistersExact may have changed registers even if its readback fails,
    // so mutation is recorded before entering the remote-call helper.
    ledger.registers_mutated=true;
    const bool called=call::CallStoppedThread(tid,call_origin,remote_dlopen,
                                              immutable_trap,arguments,&report);
    ledger.selected_thread_stopped=report.tracee_stopped;
    if (!called || !report.expected_trap || !report.rollback_succeeded ||
        report.return_value==0) {
      transaction_detail=!called ? "remote_call" :
          (!report.expected_trap ? "remote_return_stop" :
           (!report.rollback_succeeded ? "call_register_rollback" :
            "dlopen_returned_null"));
      transaction_errno=report.error; uncertain=true;
    }
  }

  bool payload_mapped=false;
  if (!uncertain) {
    maps=ReadMaps(pid);
    payload_mapped=HasExactPayloadMap(maps,payload_path,payload);
    if (!payload_mapped) {
      transaction_detail="payload_map_receipt"; transaction_errno=errno; uncertain=true;
    } else if (StartTicks(pid)!=start_ticks || ThreadGroupId(tid)!=pid ||
               ThreadName(pid,tid)!="Signal Catcher") {
      transaction_detail="post_call_identity"; transaction_errno=errno; uncertain=true;
    }
  }

  if (ledger.stack_mutated && ledger.selected_thread_stopped) {
    ledger.stack_restored=WriteAndReadback(pid,layout.backup_begin,
                                          stack_backup.data(),stack_backup.size());
    if (!ledger.stack_restored) {
      transaction_detail="stack_restore"; transaction_errno=errno; uncertain=true;
    }
  }
  if (ledger.registers_mutated && ledger.selected_thread_stopped) {
    ledger.registers_restored=call::SetRegistersExact(tid,original);
    if (!ledger.registers_restored) {
      transaction_detail="register_restore"; transaction_errno=errno; uncertain=true;
    }
  }
  // The immutable BRK was never modified, so no trap rollback is required.
  ledger.trap_mutated=false;
  ledger.trap_restored=false;

  if (uncertain || !payload_mapped || !tx::DetachSafe(ledger)) {
    const bool rollback_safe=tx::DetachSafe(ledger) &&
        StartTicks(pid)==start_ticks && ThreadGroupId(tid)==pid;
    bool clean_detach=false;
    if (rollback_safe)
      clean_detach=ptrace(PTRACE_DETACH,tid,nullptr,nullptr)!=-1;
    const bool killed=clean_detach ? false : KillUncertain(pid);
    if (transaction_errno==0) transaction_errno=errno;
    return FailTransaction(transaction_detail,transaction_errno,killed,
                           clean_detach,report,ledger,payload_mapped,
                           used_null_return);
  }
  if (ptrace(PTRACE_DETACH,tid,nullptr,nullptr)==-1) {
    const bool killed=KillUncertain(pid);
    return Fail("detach",9,killed);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (StartTicks(pid)!=start_ticks || TracerPid(pid)!=0 ||
      !HasExactPayloadMap(ReadMaps(pid),payload_path,payload)) {
    const bool killed=KillUncertain(pid);
    return Fail("post_detach",10,killed);
  }

  std::printf(
      "NATIVE_ARM64_EARLY_LOADER passed=1 pid=%d tid=%d start_ticks=%llu "
      "return_stop=%s handle=0x%llx payload_sha256=%s resolver=%s "
      "trap_module=%s stack_rollback=1 register_rollback=1 detach=1\n",
      pid,tid,static_cast<unsigned long long>(start_ticks),
      used_null_return?"null_segv":"immutable_brk",
      static_cast<unsigned long long>(report.return_value),kPayloadSha256,
      resolver_module.c_str(),trap_module.c_str());
  return 0;
}
