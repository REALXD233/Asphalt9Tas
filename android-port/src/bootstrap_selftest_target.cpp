// Offline bootstrap-injector target: a dynamic x86_64 PIE that maps the
// normal /system/lib64/libc.so and libdl.so, exactly like app_process64.
// Used to validate the v8 int3 RemoteCall bootstrap sequence (dlerror ->
// mmap -> write path -> dlopen -> dlerror) WITHOUT touching the game.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace {
void SetThreadName(const char* name) {
#if defined(__linux__) && defined(PR_SET_NAME)
    prctl(PR_SET_NAME, name, 0, 0, 0);
#else
    (void)name;
#endif
}
}  // namespace

int main(int argc, char** argv) {
    SetThreadName("boot-selftest-srv");
    std::printf("bootstrap_selftest_target pid=%d ready\n", static_cast<int>(getpid()));
    std::fflush(stdout);

    std::thread worker([] {
        SetThreadName("boot-selftest-wrk");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    });
    worker.detach();

    std::thread spinner([] {
        SetThreadName("boot-selftest-spn");
        std::atomic<std::uint64_t> counter{0};
        for (;;) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });
    spinner.detach();

    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;
}
