// Build-only wrapper for the next permitted marker-only normal guest-load
// handshake. This does not enable the unsafe x86-thread late ARM fallback.
#define A9TAS_PAYLOAD_PATH \
    "/data/local/tmp/liba9tas_marker_handshake_v1.so"
#define A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD 0
#include "bootstrap.cpp"

