#pragma once

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

namespace a9tas::ptrace_stable_freeze_v1 {

inline constexpr std::uint32_t kMaximumPasses = 8;

struct FrozenSet {
  pid_t call_tid{};
  bool call_attached{};
  std::vector<pid_t> other_tids;
  std::uint32_t passes{};
};

inline std::vector<pid_t> ListThreads(pid_t pid) {
  std::vector<pid_t> result;
  const std::string path="/proc/"+std::to_string(pid)+"/task";
  DIR* directory=opendir(path.c_str());
  if (!directory) return result;
  while (dirent* entry=readdir(directory)) {
    char* end=nullptr;
    const long value=std::strtol(entry->d_name,&end,10);
    if (value>0 && end!=entry->d_name && *end=='\0')
      result.push_back(static_cast<pid_t>(value));
  }
  closedir(directory);
  std::sort(result.begin(),result.end());
  result.erase(std::unique(result.begin(),result.end()),result.end());
  return result;
}

inline bool Contains(const std::vector<pid_t>& values,pid_t value) {
  return std::find(values.begin(),values.end(),value)!=values.end();
}

inline bool WaitForInterruptStop(pid_t tid) {
  const auto deadline=std::chrono::steady_clock::now()+
                      std::chrono::seconds(2);
  while (std::chrono::steady_clock::now()<deadline) {
    int status=0;
    const pid_t waited=waitpid(tid,&status,__WALL|WNOHANG);
    if (waited==0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (waited==-1) {
      if (errno==EINTR) continue;
      return false;
    }
    if (!WIFSTOPPED(status)) return false;
    const unsigned event=static_cast<unsigned>(status)>>16;
    const int signal=WSTOPSIG(status);
    if (event==PTRACE_EVENT_STOP) return true;
    if (ptrace(PTRACE_CONT,tid,nullptr,
               reinterpret_cast<void*>(static_cast<intptr_t>(signal)))==-1 ||
        ptrace(PTRACE_INTERRUPT,tid,nullptr,nullptr)==-1)
      return false;
  }
  errno=ETIMEDOUT;
  return false;
}

inline bool SeizeAndStop(pid_t tid,bool* retained) {
  if (!retained) return false;
  *retained=false;
  if (ptrace(PTRACE_SEIZE,tid,nullptr,
             reinterpret_cast<void*>(PTRACE_O_EXITKILL))==-1)
    return false;
  *retained=true;
  if (ptrace(PTRACE_INTERRUPT,tid,nullptr,nullptr)!=-1 &&
      WaitForInterruptStop(tid)) return true;
  if (ptrace(PTRACE_DETACH,tid,nullptr,nullptr)!=-1 || errno==ESRCH)
    *retained=false;
  return false;
}

inline bool FreezeStable(pid_t pid,pid_t call_tid,FrozenSet* frozen) {
  if (!frozen || pid<=0 || call_tid<=0 || frozen->call_attached ||
      !frozen->other_tids.empty()) return false;
  frozen->call_tid=call_tid;
  std::uint32_t stable_passes=0;
  for (std::uint32_t pass=1; pass<=kMaximumPasses; ++pass) {
    std::uint32_t additions=0;
    const std::vector<pid_t> tids=ListThreads(pid);
    if (tids.empty() || !Contains(tids,call_tid)) return false;
    for (pid_t tid:tids) {
      if (tid==call_tid || Contains(frozen->other_tids,tid)) continue;
      bool retained=false;
      if (!SeizeAndStop(tid,&retained)) {
        if (retained) frozen->other_tids.push_back(tid);
        if (kill(tid,0)==-1 && errno==ESRCH) continue;
        return false;
      }
      frozen->other_tids.push_back(tid);
      ++additions;
    }
    frozen->passes=pass;
    stable_passes=additions==0 ? stable_passes+1 : 0;
    if (stable_passes>=2) break;
  }
  if (stable_passes<2) return false;
  if (ptrace(PTRACE_ATTACH,call_tid,nullptr,nullptr)==-1) return false;
  frozen->call_attached=true;
  int status=0;
  if (waitpid(call_tid,&status,__WALL)!=call_tid || !WIFSTOPPED(status))
    return false;
  if (ptrace(PTRACE_SETOPTIONS,call_tid,nullptr,
             reinterpret_cast<void*>(PTRACE_O_EXITKILL))==-1)
    return false;

  for (pid_t tid:ListThreads(pid)) {
    if (tid==call_tid || Contains(frozen->other_tids,tid)) continue;
    bool retained=false;
    if (!SeizeAndStop(tid,&retained)) {
      if (retained) frozen->other_tids.push_back(tid);
      return false;
    }
    frozen->other_tids.push_back(tid);
  }
  const std::vector<pid_t> final=ListThreads(pid);
  if (!Contains(final,call_tid)) return false;
  for (pid_t tid:final)
    if (tid!=call_tid && !Contains(frozen->other_tids,tid)) return false;
  return true;
}

inline bool DetachAll(FrozenSet* frozen) {
  if (!frozen) return false;
  bool ok=true;
  for (auto it=frozen->other_tids.rbegin();it!=frozen->other_tids.rend();++it)
    if (ptrace(PTRACE_DETACH,*it,nullptr,nullptr)==-1 && errno!=ESRCH)
      ok=false;
  frozen->other_tids.clear();
  if (frozen->call_attached) {
    if (ptrace(PTRACE_DETACH,frozen->call_tid,nullptr,nullptr)==-1 &&
        errno!=ESRCH) ok=false;
    frozen->call_attached=false;
  }
  return ok;
}

inline bool Complete(const FrozenSet& frozen,pid_t pid) {
  if (!frozen.call_attached || frozen.call_tid<=0) return false;
  const std::vector<pid_t> live=ListThreads(pid);
  if (!Contains(live,frozen.call_tid)) return false;
  for (pid_t tid:live)
    if (tid!=frozen.call_tid && !Contains(frozen.other_tids,tid)) return false;
  return true;
}

}  // namespace a9tas::ptrace_stable_freeze_v1
