#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s PID\n", argv[0]);
        return 2;
    }

    const pid_t pid = static_cast<pid_t>(std::strtol(argv[1], nullptr, 10));
    if (pid <= 0) {
        std::fprintf(stderr, "invalid pid\n");
        return 2;
    }

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "attach failed: %s\n", std::strerror(errno));
        return 1;
    }

    int status = 0;
    if (waitpid(pid, &status, __WALL) == -1) {
        std::fprintf(stderr, "wait failed: %s\n", std::strerror(errno));
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }

    user_regs_struct regs{};
    const bool registers_ok = ptrace(PTRACE_GETREGS, pid, nullptr, &regs) != -1;
    if (registers_ok) {
        std::printf("attached pid=%d rip=0x%llx rsp=0x%llx\n", pid,
                    static_cast<unsigned long long>(regs.rip),
                    static_cast<unsigned long long>(regs.rsp));
    } else {
        std::fprintf(stderr, "getregs failed: %s\n", std::strerror(errno));
    }

    if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1) {
        std::fprintf(stderr, "detach failed: %s\n", std::strerror(errno));
        return 1;
    }

    return registers_ok ? 0 : 1;
}
