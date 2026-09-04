// Review-only composition which binds final-writer replay tick 0 to the
// authoritative central race lifecycle transition (phase 2 -> 3).

#define A9TAS_FINAL_WRITER_LIVE_CANDIDATE 1
#define A9TAS_RACE_LIFECYCLE_START_V1 1
#include "hwbp_final_writer_unified_replay_v1.cpp"

