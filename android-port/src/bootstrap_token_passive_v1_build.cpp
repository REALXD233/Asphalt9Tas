#define A9TAS_PAYLOAD_PATH \
    "/data/local/tmp/liba9tas_token_passive_v1.so"
// Negative experiment only. Do not rebuild/deploy this wrapper: on LDPlayer9
// the x86-thread late load crashed inside libhoudini at 0xdead0000.
#define A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD 1
#include "bootstrap.cpp"
