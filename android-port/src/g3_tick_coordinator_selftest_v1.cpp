#include "g3_tick_coordinator_v1.h"

#include <cstdint>
#include <cstring>

namespace g3 = a9tas::g3_tick_coordinator_v1;
namespace recording = a9tas::unified_tick_v1;

namespace {

void Frame(recording::RecordingFrameV1* frame, std::uint64_t tick) {
  std::memset(frame, 0, sizeof(*frame));
  frame->tick = tick;
  frame->flags = recording::kRequiredFrameFlags;
  frame->skip_override_flags = recording::kSupportedSkipMask;
}

bool CompleteTick(g3::State* state, std::uint32_t generation,
                  std::uint64_t tick, g3::TickScratch* published) {
  return g3::OnPrePhysics(state, generation, tick, 20) ==
             g3::Result::kPrePhysicsObserved &&
         g3::OnFinalWriter(state, generation, tick, 30) ==
             g3::Result::kFinalWriterObserved &&
         g3::OnTickEnd(state, generation, tick, 40, published) ==
             g3::Result::kTickPublished;
}

bool FiveFrameReplayAlsoRecords() {
  recording::RecordingFrameV1 frames[5]{};
  for (std::uint64_t index = 0; index < 5; ++index)
    Frame(&frames[index], index);
  g3::State state{};
  if (g3::Arm(&state, 0x12340001u, 1) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 1, 2, 3) != g3::Result::kRaceStarted)
    return false;
  const g3::ReplayQueueView queue{g3::ReplayMode::kActiveBlock, true,
                                  true, frames, 5};
  for (std::uint64_t tick = 0; tick < 5; ++tick) {
    if (g3::BeginTick(&state, 1, 10, false, queue) !=
        g3::Result::kTickBegun)
      return false;
    const recording::RecordingFrameV1* selected = nullptr;
    if (!g3::ActivePacket(state, 1, tick, &selected) ||
        selected == nullptr || selected->tick != tick)
      return false;
    g3::TickScratch published{};
    if (!CompleteTick(&state, 1, tick, &published) ||
        published.session_id != 0x12340001u ||
        published.generation != 1 || published.tick != tick ||
        published.selected_packet_index != tick ||
        published.boundary_flags != g3::kCompleteTickBoundaryMask)
      return false;
  }
  return state.tick == 5 && state.ticks_begun == 5 &&
         state.ticks_published == 5 && state.packet_selections == 5 &&
         state.pre_physics_events == 5 && state.final_writer_events == 5 &&
         state.lifecycle == g3::Lifecycle::kInRace &&
         state.tick_phase == g3::TickPhase::kClosed;
}

bool EmptyBlockPauseAndCancelDoNotMisadvance() {
  g3::State state{};
  if (g3::Arm(&state, 2, 7) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 7, 2, 3) != g3::Result::kRaceStarted)
    return false;
  const g3::ReplayQueueView blocked{g3::ReplayMode::kActiveBlock, true,
                                    true, nullptr, 0};
  if (g3::BeginTick(&state, 7, 11, true, blocked) !=
          g3::Result::kPausedNoAdvance ||
      state.tick != 0 || state.tick_phase != g3::TickPhase::kClosed ||
      g3::BeginTick(&state, 7, 11, false, blocked) !=
          g3::Result::kNeedReplayData ||
      state.tick != 0 || state.tick_phase != g3::TickPhase::kClosed)
    return false;
  const g3::ReplayQueueView cancelled{g3::ReplayMode::kActiveBlock, true,
                                      false, nullptr, 0};
  if (g3::BeginTick(&state, 7, 11, false, cancelled) !=
          g3::Result::kTickBegunWithoutReplay ||
      state.active_packet_present)
    return false;
  g3::TickScratch published{};
  return CompleteTick(&state, 7, 0, &published) && state.tick == 1 &&
         state.paused_noops == 1 && state.blocked_empty == 1 &&
         state.cancelled_blocks == 1 && state.packet_selections == 0 &&
         published.selected_packet_index == g3::kNoPacket;
}

bool FutureGapAndStalePacketsAreDeterministic() {
  recording::RecordingFrameV1 frames[3]{};
  Frame(&frames[0], 0);
  Frame(&frames[1], 1);
  Frame(&frames[2], 3);
  g3::State state{};
  if (g3::Arm(&state, 3, 9) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 9, 2, 3) != g3::Result::kRaceStarted)
    return false;
  // Simulate a late queue head: packet 0 is stale when tick 1 begins.
  state.tick = 1;
  const g3::ReplayQueueView queue{g3::ReplayMode::kActiveNoBlock, true,
                                  true, frames, 3};
  if (g3::BeginTick(&state, 9, 12, false, queue) !=
          g3::Result::kTickBegun ||
      state.replay_head != 2 || state.stale_packets_dropped != 1)
    return false;
  g3::TickScratch published{};
  if (!CompleteTick(&state, 9, 1, &published)) return false;
  if (g3::BeginTick(&state, 9, 12, false, queue) !=
          g3::Result::kTickBegunWithoutReplay ||
      state.replay_head != 2 || state.future_gaps != 1)
    return false;
  return CompleteTick(&state, 9, 2, &published) && state.tick == 3;
}

bool ActiveBlockFutureGapDoesNotAdvance() {
  recording::RecordingFrameV1 future{};
  Frame(&future, 1);
  g3::State state{};
  if (g3::Arm(&state, 0x3001, 19) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 19, 2, 3) != g3::Result::kRaceStarted)
    return false;
  const g3::ReplayQueueView queue{g3::ReplayMode::kActiveBlock, true,
                                  true, &future, 1};
  return g3::BeginTick(&state, 19, 12, false, queue) ==
             g3::Result::kNeedReplayData &&
         state.tick == 0 && state.tick_phase == g3::TickPhase::kClosed &&
         !state.active_packet_present && state.replay_head == 0 &&
         state.future_gaps == 1 && state.blocked_empty == 1 &&
         state.ticks_begun == 0;
}

bool RepeatedPhysicsIntervalsDoNotOpenAnotherTick() {
  g3::State state{};
  if (g3::Arm(&state, 0x3002, 20) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 20, 2, 3) != g3::Result::kRaceStarted ||
      g3::BeginTick(&state, 20, 15, false, {}) !=
          g3::Result::kTickBegunWithoutReplay ||
      g3::OnPrePhysics(&state, 20, 0, 15) !=
          g3::Result::kPrePhysicsObserved ||
      g3::OnPrePhysics(&state, 20, 0, 15) !=
          g3::Result::kPhysicsIntervalRepeated ||
      g3::OnFinalWriter(&state, 20, 0, 16) !=
          g3::Result::kFinalWriterObserved ||
      g3::OnPrePhysics(&state, 20, 0, 15) !=
          g3::Result::kPhysicsIntervalRepeated)
    return false;
  g3::TickScratch published{};
  return g3::OnTickEnd(&state, 20, 0, 17, &published) ==
             g3::Result::kTickPublished &&
         state.tick == 1 && state.ticks_begun == 1 &&
         state.pre_physics_events == 1 &&
         state.physics_interval_calls == 3 &&
         published.physics_interval_calls == 3 &&
         state.failures == 0;
}

bool RaceEndAndRetryRequireNewGeneration() {
  g3::State state{};
  if (g3::Arm(&state, 4, 10) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 10, 2, 3) != g3::Result::kRaceStarted ||
      g3::BeginRaceEnd(&state, 10, 3, 4) != g3::Result::kRaceEnding ||
      g3::FinishRaceEnd(&state, 10) != g3::Result::kInactive ||
      state.lifecycle != g3::Lifecycle::kInactive || state.race_ends != 1)
    return false;
  g3::State stale = state;
  if (g3::Arm(&stale, 5, 10) != g3::Result::kWrongGeneration ||
      stale.lifecycle != g3::Lifecycle::kFaulted)
    return false;
  return g3::Arm(&state, 5, 11) == g3::Result::kArmed &&
         g3::OnRaceStart(&state, 11, 2, 3) == g3::Result::kRaceStarted &&
         state.generation == 11 && state.tick == 0 &&
         state.race_starts == 2;
}

bool PausedReplayCanHandOffToSuffixTickZero() {
  g3::State state{};
  if (g3::Arm(&state, 0x7001, 30) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 30, 2, 3) != g3::Result::kRaceStarted ||
      g3::BeginTick(&state, 30, 11, false, {}) !=
          g3::Result::kTickBegunWithoutReplay)
    return false;
  g3::TickScratch published{};
  if (!CompleteTick(&state, 30, 0, &published) || state.tick != 1)
    return false;
  if (g3::RearmAtPausedRace(&state, 0x7002, 31) !=
          g3::Result::kRaceStarted)
    return false;
  return state.lifecycle == g3::Lifecycle::kInRace &&
         state.tick_phase == g3::TickPhase::kClosed && state.tick == 0 &&
         state.generation == 31 && state.session_id == 0x7002 &&
         state.race_starts == 1 && state.race_ends == 0 &&
         state.ticks_begun == 0 && state.ticks_published == 0 &&
         !state.active_packet_present;
}

bool ReplayCanContinueOnOneRecordingTimeline() {
  recording::RecordingFrameV1 frames[2]{};
  Frame(&frames[0], 0);
  Frame(&frames[1], 1);
  const g3::ReplayQueueView replay{g3::ReplayMode::kActiveBlock, true,
                                   true, frames, 2};
  g3::State state{};
  if (g3::Arm(&state, 0x8001, 40) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 40, 2, 3) != g3::Result::kRaceStarted)
    return false;
  g3::TickScratch published{};
  for (std::uint64_t tick = 0; tick < 2; ++tick) {
    if (g3::BeginTick(&state, 40, 10, false, replay) !=
            g3::Result::kTickBegun ||
        !CompleteTick(&state, 40, tick, &published))
      return false;
  }
  if (g3::ContinueReplayAsRecordAtClosedBoundary(&state, 0x8002, 41) !=
          g3::Result::kRaceStarted ||
      state.tick != 2 || state.ticks_begun != 2 ||
      state.ticks_published != 2 || state.packet_selections != 0 ||
      state.replay_head != 0 || state.session_id != 0x8002 ||
      state.generation != 41)
    return false;
  if (g3::BeginTick(&state, 41, 10, false, {}) !=
          g3::Result::kTickBegunWithoutReplay ||
      !CompleteTick(&state, 41, 2, &published))
    return false;
  return published.tick == 2 && published.session_id == 0x8002 &&
         published.generation == 41 &&
         published.selected_packet_index == g3::kNoPacket &&
         state.tick == 3 && state.ticks_begun == 3 &&
         state.ticks_published == 3;
}

bool DuplicateOrOutOfOrderBoundaryFaults() {
  g3::State state{};
  if (g3::Arm(&state, 6, 12) != g3::Result::kArmed ||
      g3::OnRaceStart(&state, 12, 2, 3) != g3::Result::kRaceStarted)
    return false;
  const g3::ReplayQueueView inactive{};
  if (g3::BeginTick(&state, 12, 13, false, inactive) !=
      g3::Result::kTickBegunWithoutReplay)
    return false;
  return g3::OnFinalWriter(&state, 12, 0, 14) ==
             g3::Result::kWrongOrder &&
         state.lifecycle == g3::Lifecycle::kFaulted &&
         state.tick == 0 && state.ticks_published == 0 &&
         state.failures == 1;
}

}  // namespace

int main() {
  return FiveFrameReplayAlsoRecords() &&
                 EmptyBlockPauseAndCancelDoNotMisadvance() &&
                 FutureGapAndStalePacketsAreDeterministic() &&
                 ActiveBlockFutureGapDoesNotAdvance() &&
                 RepeatedPhysicsIntervalsDoNotOpenAnotherTick() &&
                 RaceEndAndRetryRequireNewGeneration() &&
                 PausedReplayCanHandOffToSuffixTickZero() &&
                 ReplayCanContinueOnOneRecordingTimeline() &&
                 DuplicateOrOutOfOrderBoundaryFaults()
             ? 0
             : 1;
}
