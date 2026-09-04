#define A9TAS_ENABLE_SAME_THREAD_PROBE 1
#define A9TAS_PAYLOAD_PATH \
    "/data/local/tmp/liba9tas_producer_action_observe_v1.so"
#define A9TAS_SAME_THREAD_PROBE_SYMBOL \
    "a9tas_producer_action_observe_v1"
// JNI shorty: jlong return plus one explicit jlong vehicle-owner argument.
#define A9TAS_SAME_THREAD_PROBE_SHORTY "JJ"

#include "bootstrap.cpp"
