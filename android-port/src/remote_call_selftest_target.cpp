// H0: RemoteCall self-test target
// This is a standalone executable that exports well-known functions
// for the controller to call via ptrace + RemoteCall.
// It must be compiled as a native x86-64 Linux executable for the host.

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

// ---- Exported functions for the controller to call ----

// These are the symbols the controller will resolve and call via RemoteCall.
// They must be visible and at known offsets.

extern "C" {

// Normal return: a + b + c + 7
std::uint64_t SelftestOk(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Trigger SIGSEGV
std::uint64_t SelftestSegv(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Trigger SIGILL
std::uint64_t SelftestIll(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Trigger SIGTRAP (wrong trap source)
std::uint64_t SelftestTrap(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Exit the calling thread
std::uint64_t SelftestExitThread(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Exit the whole process
std::uint64_t SelftestExitProcess(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Hang forever (for timeout test)
std::uint64_t SelftestHang(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Stress: 1000 iterations of OkCall
std::uint64_t SelftestStress(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Echo: return the first argument unchanged
std::uint64_t SelftestEcho(std::uint64_t a, std::uint64_t b, std::uint64_t c);

// Libc-like: uses a real stack frame and several register spills, so its
// prologue is representative of mmap/dlopen rather than a 2-instruction stub.
std::uint64_t SelftestLibcLike(std::uint64_t a, std::uint64_t b, std::uint64_t c);

}  // extern "C"

// ---- Implementation ----

namespace {

void SetThreadName(const char* name) {
#if defined(__linux__) && defined(PR_SET_NAME)
    prctl(PR_SET_NAME, name, 0, 0, 0);
#else
    (void)name;
#endif
}

}  // namespace

// Normal return: a + b + c + 7
extern "C" std::uint64_t SelftestOk(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return a + b + c + 7;
}

// Trigger SIGSEGV
extern "C" std::uint64_t SelftestSegv(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    (void)a; (void)b; (void)c;
    volatile std::uint64_t* p = nullptr;
    *p = 1;
    return 0;  // unreachable
}

// Trigger SIGILL
extern "C" std::uint64_t SelftestIll(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    (void)a; (void)b; (void)c;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#else
    std::raise(SIGILL);
#endif
    return 0;  // unreachable
}

// Trigger SIGTRAP (wrong trap source)
extern "C" std::uint64_t SelftestTrap(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    (void)a; (void)b; (void)c;
    std::raise(SIGTRAP);
    return 0;  // unreachable
}

// Exit the calling thread
extern "C" std::uint64_t SelftestExitThread(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    (void)a; (void)b; (void)c;
#if defined(__linux__)
    pthread_exit(nullptr);
#else
    std::exit(0);
#endif
    return 0;  // unreachable
}

// Exit the whole process
extern "C" std::uint64_t SelftestExitProcess(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    (void)a; (void)b; (void)c;
    std::exit(static_cast<int>(a));
    return 0;  // unreachable
}

// Hang forever (for timeout test)
extern "C" std::uint64_t SelftestHang(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    (void)a; (void)b; (void)c;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;  // unreachable
}

// Stress: 1000 iterations of OkCall
extern "C" std::uint64_t SelftestStress(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        sum += SelftestOk(i, i + 1, i + 2);
    }
    return sum;
}

// Echo: return the first argument unchanged
extern "C" std::uint64_t SelftestEcho(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return a;
}

// Libc-like: uses a real stack frame and several register spills, so its
// prologue is representative of mmap/dlopen rather than a 2-instruction stub.
extern "C" std::uint64_t SelftestLibcLike(
    std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    volatile std::uint64_t frame[8];
    frame[0] = a;
    frame[1] = b;
    frame[2] = c;
    for (int i = 3; i < 8; ++i) {
        frame[i] = frame[i - 1] * 3 + frame[i - 2] - frame[i - 3];
    }
    std::uint64_t sum = 0;
    for (int i = 0; i < 8; ++i) {
        sum += frame[i] * static_cast<std::uint64_t>(i + 1);
    }
    return sum ^ UINT64_C(0x9E3779B97F4A7C15);
}

// ---- Main: serve mode ----

// If started with --serve, the process blocks forever and waits for
// a ptrace-based RemoteCall from the controller.
// This is the mode used by the H0 self-test controller.

#ifndef RC_SELFTEST_CONTROLLER
int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--serve") == 0) {
        SetThreadName("rc-selftest-srv");
        std::thread worker([] {
            SetThreadName("rc-selftest-wrk");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
            }
        });
        worker.detach();
        // Busy user-space thread: RemoteCall targets this thread so the
        // tracee is never stopped inside a syscall restart window.
        std::thread spinner([] {
            SetThreadName("rc-selftest-spin");
            std::atomic<std::uint64_t> counter{0};
            for (;;) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
        spinner.detach();
        // Block forever; the controller will attach and call us via ptrace.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
        return 0;
    }

    // Legacy standalone modes (kept for compatibility with old scripts)
    const std::string mode = argc >= 2 ? argv[1] : "";
    SetThreadName("rc-selftest");

    if (mode == "ok") {
        const std::uint64_t r = SelftestOk(1, 2, 3);
        std::printf("ok result=%llu\n", static_cast<unsigned long long>(r));
        return r == 13 ? 0 : 1;
    }
    if (mode == "echo") {
        if (argc >= 3) {
            const std::uint64_t v = static_cast<std::uint64_t>(
                std::strtoull(argv[2], nullptr, 0));
            const std::uint64_t r = SelftestEcho(v, 0, 0);
            std::printf("echo result=%llu\n", static_cast<unsigned long long>(r));
            return r == v ? 0 : 1;
        }
        return 2;
    }
    if (mode == "segv") SelftestSegv(0, 0, 0);
    if (mode == "ill") SelftestIll(0, 0, 0);
    if (mode == "trap") SelftestTrap(0, 0, 0);
    if (mode == "exit-thread") SelftestExitThread(0, 0, 0);
    if (mode == "exit-process") SelftestExitProcess(0, 0, 0);
    if (mode == "hang") SelftestHang(0, 0, 0);
    if (mode == "stress") {
        const std::uint64_t r = SelftestStress(0, 0, 0);
        std::printf("stress sum=%llu\n", static_cast<unsigned long long>(r));
        return r != 0 ? 0 : 1;
    }

    std::fprintf(stderr,
                 "usage: %s --serve\n"
                 "       %s MODE [ARG]\n"
                 "modes: ok echo segv ill trap exit-thread exit-process hang stress\n",
                 argv[0], argv[0]);
    return 2;
}
#endif
