// Offline marker library for the x86_64 bootstrap injector test.
// Its constructor writes a marker file, proving that the remote
// mmap -> dlopen sequence actually executed inside the target process.
// It also exports the same status/stage verification symbols as the real
// liba9tas_bootstrap.so so the injector's full verification path is tested.
#include <cstdio>

extern "C" __attribute__((visibility("default"))) int a9tas_bootstrap_status() {
    return 1;
}

extern "C" __attribute__((visibility("default"))) int a9tas_bootstrap_stage() {
    return 0x58;  // offline marker stage
}

__attribute__((constructor)) static void bootstrap_marker_init() {
    FILE* file = std::fopen("/data/local/tmp/a9tas_bootstrap_marker.txt", "w");
    if (file != nullptr) {
        std::fputs("x86_64 bootstrap ok\n", file);
        std::fclose(file);
    }
}
