#include "../src/g4_multi_hook_runtime_v1.h"
namespace p = a9tas::g4_multi_hook_runtime_v1;
constexpr auto record = static_cast<std::uint32_t>(p::RunMode::kRecord);
constexpr auto replay = static_cast<std::uint32_t>(p::RunMode::kReplay);
static_assert(p::RecordSlowmoDivisor(record, 8) == 8);
static_assert(p::RecordSlowmoDivisor(record, 90) == 10);
static_assert(p::RecordSlowmoNumerator(record, 90) == 9);
static_assert(p::RecordSlowmoDivisor(replay, 90) == 1);
static_assert(p::RecordSlowmoNumerator(replay, 90) == 1);
static_assert(p::RecordSlowmoDivisor(record, 75) == 4);
static_assert(p::RecordSlowmoNumerator(record, 75) == 3);
static_assert(p::RecordSlowmoNumerator(replay, 75) == 1);
static_assert(p::RecordSlowmoDivisor(replay, 75) == 1);
static_assert(p::RecordSlowmoDivisor(replay, 8) == 1);
static_assert(p::RecordSlowmoDivisor(record, 0) == 1);
static_assert(!p::ValidRecordSlowmo(3));
static_assert(sizeof(p::Control) == 640);
int main() { return 0; }
