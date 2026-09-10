#include "../src/g4_multi_hook_runtime_v1.h"
namespace p = a9tas::g4_multi_hook_runtime_v1;
static_assert(p::kMaximumFrames == 24000);
static_assert((p::kMaximumFrames - 1ULL) * 6944 > 150000000ULL);
static_assert(p::kMaximumIntervalSamples >= p::kMaximumFrames * 4);
static_assert(p::ArmFrameLimitValid(24000, p::RunMode::kRecord,
    p::CompletionPolicy::kRaceLifecycle, false));
static_assert(p::ArmFrameLimitValid(24000, p::RunMode::kReplay,
    p::CompletionPolicy::kFixedFrameLimit, false));
static_assert(!p::ArmFrameLimitValid(24001, p::RunMode::kReplay,
    p::CompletionPolicy::kFixedFrameLimit, false));
int main() { return 0; }
