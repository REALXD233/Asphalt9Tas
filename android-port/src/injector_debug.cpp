#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

pid_t FindPidByCmdline(const char* expected) {
    DIR* proc = opendir("/proc");
    if (proc == nullptr) return 0;
    pid_t result = 0;
    while (dirent* entry = readdir(proc)) {
        char* end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (value <= 0 || end == entry->d_name || *end != '\0') continue;
        std::ifstream cmdline(std::string("/proc/") + entry->d_name + "/cmdline",
                              std::ios::binary);
        std::string command;
        std::getline(cmdline, command, '\0');
        if (command == expected) {
            result = static_cast<pid_t>(value);
            break;
        }
    }
    closedir(proc);
    return result;
}

bool ProcessHasMapping(pid_t pid, const char* needle) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(input, line)) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

struct Mapping {
    std::uintptr_t start{};
    std::uintptr_t offset{};
    std::string path;
};

bool FindMapping(pid_t pid, std::uintptr_t address, Mapping* out) {
    const std::string maps = pid == getpid()
        ? "/proc/self/maps"
        : "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream input(maps);
    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, path);
        if (fields >= 4 && address >= start && address < end) {
            out->start = static_cast<std::uintptr_t>(start);
            out->offset = static_cast<std::uintptr_t>(offset);
            if (fields == 5) {
                out->path = path;
                while (!out->path.empty() && out->path.front() == ' ') out->path.erase(0, 1);
            }
            return true;
        }
    }
    return false;
}

bool FindModuleMapping(pid_t pid, const std::string& path, std::uintptr_t file_offset,
                       Mapping* out) {
    const std::string maps = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream input(maps);
    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char candidate[1024]{};
        const int fields = std::sscanf(line.c_str(), "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, candidate);
        std::string candidate_path = fields == 5 ? candidate : "";
        while (!candidate_path.empty() && candidate_path.front() == ' ') candidate_path.erase(0, 1);
        if (fields == 5 && candidate_path == path && offset == file_offset) {
            out->start = static_cast<std::uintptr_t>(start);
            out->offset = static_cast<std::uintptr_t>(offset);
            out->path = candidate_path;
            return true;
        }
    }
    return false;
}

std::uintptr_t RemoteSymbol(pid_t pid, void* local_symbol) {
    Mapping local{};
    if (!FindMapping(getpid(), reinterpret_cast<std::uintptr_t>(local_symbol), &local) ||
        local.path.empty()) {
        return 0;
    }
    Mapping remote{};
    if (!FindModuleMapping(pid, local.path, local.offset, &remote)) {
        return 0;
    }
    return remote.start + (reinterpret_cast<std::uintptr_t>(local_symbol) - local.start);
}

bool WriteRemote(pid_t pid, std::uintptr_t address, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t done = 0; done < size; done += sizeof(long)) {
        const size_t chunk = size - done < sizeof(long) ? size - done : sizeof(long);
        long word = 0;
        if (chunk != sizeof(long)) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid, address + done, nullptr);
            if (word == -1 && errno != 0) return false;
        }
        std::memcpy(&word, bytes + done, chunk);
        if (ptrace(PTRACE_POKEDATA, pid, address + done, word) == -1) return false;
    }
    return true;
}

// RemoteCall v2: software-breakpoint (INT3) on the target's OWN private
// stack, no hardware debug registers. Rationale: the game's anti-debug
// clears DR0-DR7 while the main thread runs, so the DR0 execute breakpoint
// never fires (observed: thread runs past saved.rip and SEGV's). An INT3 in
// the target's private scratch stack region is invisible to code scanning and
// does not touch shared code pages (the abandoned INT3-in-text approach).
//
// Layout (16-byte aligned scratch below rsp):
//   [stack-8]  = 0xCC (INT3), placed as a full word whose first byte is 0xCC
//   [stack]    = stack-8  (the return address the callee's `ret` pops)
// The callee runs, `ret` jumps to stack-8, the INT3 fires -> SIGTRAP with
// rip == stack-7. The injector then restores both slots and saved.rip.
bool RemoteCall(pid_t pid, std::uintptr_t function, const std::uint64_t args[6],
                std::uint64_t* result) {
    user_regs_struct saved{};
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &saved) == -1) return false;

    const std::uintptr_t call_stack =
        ((static_cast<std::uintptr_t>(saved.rsp) - 0x8000) & ~std::uintptr_t{0xf}) - 8;
    const std::uintptr_t int3_addr = call_stack - 8;

    std::uint64_t original_int3_word = 0;
    std::uint64_t original_ret_slot = 0;
    bool ok = WriteRemote(pid, int3_addr, &original_int3_word, sizeof(original_int3_word));
    ok = ok && WriteRemote(pid, call_stack, &original_ret_slot, sizeof(original_ret_slot));
    if (!ok) {
        std::fprintf(stderr, "rc: scratch read failed\n");
        return false;
    }
    std::uint64_t int3_word = 0xCC;  // first byte 0xCC = INT3; rest never executed
    std::uint64_t ret_slot = int3_addr;
    ok = WriteRemote(pid, int3_addr, &int3_word, sizeof(int3_word));
    ok = ok && WriteRemote(pid, call_stack, &ret_slot, sizeof(ret_slot));
    if (!ok) {
        std::fprintf(stderr, "rc: scratch write failed\n");
        return false;
    }

    user_regs_struct call = saved;
    call.rip = function;
    call.rsp = call_stack;
    call.rdi = args[0];
    call.rsi = args[1];
    call.rdx = args[2];
    call.rcx = args[3];
    call.r8 = args[4];
    call.r9 = args[5];
    ok = ptrace(PTRACE_SETREGS, pid, nullptr, &call) != -1;
    if (!ok) std::fprintf(stderr, "rc: SETREGS failed errno=%d\n", errno);
    if (ok) {
        ok = ptrace(PTRACE_CONT, pid, nullptr, nullptr) != -1;
        if (!ok) std::fprintf(stderr, "rc: CONT failed errno=%d\n", errno);
    }

    int status = 0;
    if (ok) {
        const pid_t w = waitpid(pid, &status, __WALL);
        ok = w == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP;
        if (!ok) {
            std::fprintf(stderr,
                         "rc: waitpid w=%d status=0x%x stopped=%d sig=%d errno=%d\n",
                         static_cast<int>(w), status,
                         WIFSTOPPED(status) ? 1 : 0,
                         WIFSTOPPED(status) ? WSTOPSIG(status) : -1, errno);
        }
    }
    if (ok) {
        user_regs_struct returned{};
        ok = ptrace(PTRACE_GETREGS, pid, nullptr, &returned) != -1;
        ok = ok && returned.rip == int3_addr + 1;
        if (ok) {
            *result = returned.rax;
        } else {
            std::fprintf(stderr,
                         "rc: unexpected trap rip=0x%llx expected=0x%llx\n",
                         static_cast<unsigned long long>(returned.rip),
                         static_cast<unsigned long long>(int3_addr + 1));
        }
    }

    // restore scratch + registers
    WriteRemote(pid, int3_addr, &original_int3_word, sizeof(original_int3_word));
    WriteRemote(pid, call_stack, &original_ret_slot, sizeof(original_ret_slot));
    const bool regs_restored = ptrace(PTRACE_SETREGS, pid, nullptr, &saved) != -1;
    return ok && regs_restored;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr,
                     "usage: %s PID /absolute/library.so\n"
                     "       %s --wait PROCESS_NAME /absolute/library.so\n",
                     argv[0], argv[0]);
        return 2;
    }

    pid_t pid = 0;
    const char* library = nullptr;
    if (argc == 4 && std::strcmp(argv[1], "--wait") == 0) {
        library = argv[3];
        for (int attempt = 0; attempt < 30000 && pid == 0; ++attempt) {
            const pid_t candidate = FindPidByCmdline(argv[2]);
            if (candidate != 0 && ProcessHasMapping(candidate, "/system/lib64/libc.so") &&
                ProcessHasMapping(candidate, "/system/lib64/libdl.so")) {
                pid = candidate;
                break;
            }
            usleep(1000);
        }
        if (pid == 0) {
            std::fprintf(stderr, "timed out waiting for %s\n", argv[2]);
            return 1;
        }
        std::printf("found process=%s pid=%d\n", argv[2], pid);
    } else if (argc == 3) {
        pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
        library = argv[2];
    } else {
        return 2;
    }
    if (pid <= 0 || library[0] != '/') return 2;

    void* mmap_symbol = dlsym(RTLD_DEFAULT, "mmap");
    void* dlopen_symbol = dlsym(RTLD_DEFAULT, "dlopen");
    const auto remote_mmap = RemoteSymbol(pid, mmap_symbol);
    const auto remote_dlopen = RemoteSymbol(pid, dlopen_symbol);
    if (remote_mmap == 0 || remote_dlopen == 0) {
        std::fprintf(stderr, "symbol resolution failed mmap=%p dlopen=%p\n",
                     reinterpret_cast<void*>(remote_mmap), reinterpret_cast<void*>(remote_dlopen));
        return 1;
    }

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "attach failed: %s\n", std::strerror(errno));
        return 1;
    }
    int attach_status = 0;
    const bool attached = waitpid(pid, &attach_status, __WALL) == pid && WIFSTOPPED(attach_status);
    if (!attached) {
        std::fprintf(stderr, "wait attach failed status=0x%x error=%s\n",
                     attach_status, std::strerror(errno));
    }

    bool success = false;
    // Retry the attach+remote-call cycle: during the game's startup other
    // threads keep running while the main thread is stopped, so waitpid may
    // observe an unrelated stop (e.g. another thread's crash) instead of the
    // hardware-breakpoint trap. A failed attempt leaves the target intact
    // (detach with signal 0 discards any pending stop), so retrying is safe.
    for (int attempt = 0; attempt < 4 && !success; ++attempt) {
        if (attempt > 0) {
            usleep(500000);
            if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
                std::fprintf(stderr, "re-attach failed: %s\n", std::strerror(errno));
                break;
            }
            int re_status = 0;
            const bool re_attached =
                waitpid(pid, &re_status, __WALL) == pid && WIFSTOPPED(re_status);
            if (!re_attached) {
                std::fprintf(stderr, "re-attach wait failed status=0x%x\n", re_status);
                break;
            }
        }
        const std::uint64_t mmap_args[6] = {
            0, 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, static_cast<std::uint64_t>(-1), 0
        };
        std::uint64_t scratch = 0;
        const bool mmap_ok = RemoteCall(pid, remote_mmap, mmap_args, &scratch);
        if (!mmap_ok) {
            std::fprintf(stderr, "remote mmap call failed (attempt %d)\n", attempt);
            continue;
        }
        if (scratch == 0 || scratch == static_cast<std::uint64_t>(-1)) {
            std::fprintf(stderr, "remote mmap returned bad scratch (attempt %d)\n", attempt);
            continue;
        }
        if (!WriteRemote(pid, scratch, library, std::strlen(library) + 1)) {
            std::fprintf(stderr, "remote path write failed (attempt %d)\n", attempt);
            continue;
        }
        const std::uint64_t dlopen_args[6] = {scratch, RTLD_NOW | RTLD_LOCAL, 0, 0, 0, 0};
        std::uint64_t handle = 0;
        const bool dlopen_ok = RemoteCall(pid, remote_dlopen, dlopen_args, &handle);
        std::printf("pid=%d mmap=%p dlopen=%p scratch=%p handle=%p\n", pid,
                    reinterpret_cast<void*>(remote_mmap),
                    reinterpret_cast<void*>(remote_dlopen),
                    reinterpret_cast<void*>(scratch), reinterpret_cast<void*>(handle));
        if (dlopen_ok && handle != 0) {
            success = true;
        } else {
            std::fprintf(stderr, "remote dlopen failed (attempt %d)\n", attempt);
        }
        if (!success) {
            if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1) {
                std::fprintf(stderr, "mid-retry detach failed: %s\n", std::strerror(errno));
                break;
            }
        }
    }
    if (!success) std::fprintf(stderr, "remote injection did not complete\n");

    if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "detach failed: %s\n", std::strerror(errno));
        return 1;
    }
    return success ? 0 : 1;
}
