#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>

namespace {
using StatusFn = int (*)();
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /absolute/bootstrap.so\n", argv[0]);
        return 2;
    }

    // Mirror the game host's prerequisite: the NativeBridge callback table is
    // already resident before the bootstrap arrives. This is a separate x86_64
    // process and never attaches to the game.
    void* native_bridge =
        dlopen("/system/lib64/libnb.so", RTLD_NOW | RTLD_GLOBAL);
    if (native_bridge == nullptr) {
        std::fprintf(stderr, "FAIL preload_libnb error=%s\n", dlerror());
        return 1;
    }

    void* bootstrap = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "FAIL load_bootstrap error=%s\n", dlerror());
        return 1;
    }
    auto status = reinterpret_cast<StatusFn>(
        dlsym(bootstrap, "a9tas_bootstrap_status"));
    auto stage = reinterpret_cast<StatusFn>(
        dlsym(bootstrap, "a9tas_bootstrap_stage"));
    if (status == nullptr || stage == nullptr) {
        std::fprintf(stderr, "FAIL exported_status_api\n");
        return 1;
    }

    const int initial = stage();
    std::printf("INFO loaded=%d initial_stage=%d\n", status(), initial);
    const bool armed = initial == 2;
    std::printf("%s one_shot_callbacks_armed\n", armed ? "PASS" : "FAIL");

    sleep(16);
    const int final = stage();
    std::printf("INFO final_stage=%d\n", final);
    const bool restored = final == 6;
    std::printf("%s watchdog_restored_callbacks\n",
                restored ? "PASS" : "FAIL");

    // Deliberately keep both libraries resident until process exit. Calling
    // dlclose while a detached watchdog could still be running would make the
    // self-test itself invalid.
    return armed && restored ? 0 : 1;
}
