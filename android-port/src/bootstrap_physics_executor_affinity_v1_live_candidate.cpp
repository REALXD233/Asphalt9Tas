// Explicit live-candidate bootstrap configuration for the P1 observer.
// Loading remains passive; invoking the published NativeBridge trampoline is
// the only operation that can arm the guest hook.
#define A9TAS_ENABLE_SAME_THREAD_PROBE 1
#define A9TAS_PAYLOAD_PATH \
    "/data/local/tmp/liba9tas_physics_executor_affinity_v1_live_candidate.so"
#define A9TAS_SAME_THREAD_PROBE_SYMBOL \
    "a9tas_physics_executor_affinity_v1_arm"
#define A9TAS_SAME_THREAD_PROBE_SHORTY "I"

#include "bootstrap.cpp"
