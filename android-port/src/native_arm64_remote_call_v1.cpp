#include "native_arm64_remote_call_v1.h"

#if defined(__aarch64__) && defined(__ANDROID__)

#include <asm/ptrace.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <linux/elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <thread>

namespace a9tas::native_arm64_remote_call_v1 {
namespace {

bool ToNative(const RegisterImage& source, user_pt_regs* output) {
  if (!output) return false;
  user_pt_regs result{};
  for (size_t index = 0; index < 31; ++index)
    result.regs[index] = source.x[index];
  result.sp = source.sp;
  result.pc = source.pc;
  result.pstate = source.pstate;
  *output = result;
  return true;
}

RegisterImage FromNative(const user_pt_regs& source) {
  RegisterImage result{};
  for (size_t index = 0; index < 31; ++index)
    result.x[index] = source.regs[index];
  result.sp = source.sp;
  result.pc = source.pc;
  result.pstate = source.pstate;
  return result;
}

bool WaitBounded(int tid, int* status, bool* stopped) {
  if (!status || !stopped) return false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    int observed = 0;
    const pid_t waited = waitpid(tid, &observed, WNOHANG | __WALL);
    if (waited == tid) {
      *status = observed;
      *stopped = WIFSTOPPED(observed);
      return true;
    }
    if (waited == -1 && errno != EINTR) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  (void)kill(tid, SIGSTOP);
  while (true) {
    const pid_t waited = waitpid(tid, status, __WALL);
    if (waited == tid) {
      *stopped = WIFSTOPPED(*status);
      errno = ETIMEDOUT;
      return false;
    }
    if (waited == -1 && errno != EINTR) return false;
  }
}

}  // namespace

bool GetRegisters(int tid, RegisterImage* output) {
  if (tid <= 0 || !output) return false;
  user_pt_regs native{};
  iovec vector{&native, sizeof(native)};
  if (ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS),
             &vector) == -1 || vector.iov_len != sizeof(native))
    return false;
  *output = FromNative(native);
  return true;
}

bool SetRegistersExact(int tid, const RegisterImage& value) {
  if (tid <= 0) return false;
  user_pt_regs native{};
  if (!ToNative(value, &native)) return false;
  iovec vector{&native, sizeof(native)};
  if (ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS),
             &vector) == -1)
    return false;
  RegisterImage observed{};
  return GetRegisters(tid, &observed) &&
         std::memcmp(&observed, &value, sizeof(value)) == 0;
}

bool CallStoppedThread(int tid, const RegisterImage& original,
                       uint64_t function, uint64_t trap,
                       const uint64_t arguments[6],
                       CallReport* report) {
  if (!report) return false;
  *report = {};
  PreparedCall prepared{};
  if (tid <= 0 || !Prepare(original, function, trap, arguments, &prepared) ||
      !SetRegistersExact(tid, prepared.registers))
    return false;

  bool stopped = true;
  bool continued = ptrace(PTRACE_CONT, tid, nullptr, nullptr) != -1;
  if (continued) stopped = false;
  int status = 0;
  bool waited = continued && WaitBounded(tid, &status, &stopped);
  report->tracee_stopped = stopped;
  report->wait_status = status;
  if (stopped && WIFSTOPPED(status)) report->stop_signal = WSTOPSIG(status);

  RegisterImage returned{};
  siginfo_t info{};
  const bool have_registers = stopped && GetRegisters(tid, &returned);
  const bool have_signal = stopped &&
      ptrace(PTRACE_GETSIGINFO, tid, nullptr, &info) != -1;
  if (have_registers) {
    report->return_value = returned.x[0];
    report->observed_pc = returned.pc;
  }
  if (have_signal) report->signal_code = info.si_code;
  const bool immutable_brk = trap != 0 && report->stop_signal == SIGTRAP &&
      info.si_signo == SIGTRAP &&
      (info.si_code == SI_KERNEL || info.si_code == TRAP_BRKPT);
  const bool null_return = trap == 0 && report->stop_signal == SIGSEGV &&
      info.si_signo == SIGSEGV && info.si_code == SEGV_MAPERR;
  report->expected_trap = waited && have_registers && have_signal &&
      (immutable_brk || null_return) && ReturnedAtExactTrap(returned, trap);

  if (!stopped) {
    report->error = errno;
    return false;
  }
  report->rollback_attempted = true;
  report->rollback_succeeded = SetRegistersExact(tid, original);
  report->detach_safe = report->rollback_succeeded;
  report->error = report->expected_trap && report->rollback_succeeded ? 0 : errno;
  return report->expected_trap && report->rollback_succeeded;
}

}  // namespace a9tas::native_arm64_remote_call_v1

#endif
