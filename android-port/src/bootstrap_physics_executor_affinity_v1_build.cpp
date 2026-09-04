// Isolated normal-load bootstrap configuration for the P1 build-only payload.
// The payload's arm export is constant -100, so even a successfully created
// NativeBridge trampoline cannot install the observer in this build.
#define A9TAS_ENABLE_SAME_THREAD_PROBE 1
#define A9TAS_PAYLOAD_PATH \
    "/data/local/tmp/liba9tas_physics_executor_affinity_v1_build_only.so"
#define A9TAS_SAME_THREAD_PROBE_SYMBOL \
    "a9tas_physics_executor_affinity_v1_arm"
#define A9TAS_SAME_THREAD_PROBE_SHORTY "I"

#include "bootstrap.cpp"
