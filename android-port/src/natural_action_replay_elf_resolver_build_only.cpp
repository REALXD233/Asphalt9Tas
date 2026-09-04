#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 2
#include "natural_action_lifecycle_elf_resolver_v1.h"

namespace resolver = a9tas::natural_action_lifecycle_elf_v1;

static_assert(sizeof(resolver::kExpectedSha256) == 32);
static_assert(sizeof(resolver::kExpectedBuildId) == 20);
static_assert(resolver::kPayloadBasename[0] == 'l');

extern "C" int a9tas_natural_action_replay_resolver_build_only_v1() {
    return -100;
}
