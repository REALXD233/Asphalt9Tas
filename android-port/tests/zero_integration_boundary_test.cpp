#include "g4_g3_adapter_v1.h"
#include <cstdio>
#include <memory>
#include <vector>
namespace b = a9tas::g4_g3_adapter_v1;
namespace g3 = a9tas::g3_boundary_adapter_v1;
namespace c = a9tas::g3_tick_coordinator_v1;
namespace g4 = a9tas::g4_input_action_v1;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line=%d\n", __LINE__); return false; } } while (0)

static bool run(unsigned hz, unsigned delta, unsigned frames) {
  b::ConfigV1 cfg{};
  cfg.tick.session_id=1; cfg.tick.generation=1; cfg.tick.frame_limit=frames;
  cfg.tick.game_base=0x10000000;
  cfg.tick.expected_begin_owner=0x20000000;
  cfg.tick.expected_begin_owner_vptr=cfg.tick.game_base+g3::kPhysicsContextVtableRva;
  cfg.tick.expected_interval_owner=0x30000000;
  cfg.tick.expected_interval_owner_vptr=cfg.tick.game_base+g3::kPhysicsImplementationVtableRva;
  cfg.tick.expected_player=0x40000000; cfg.tick.expected_player_vptr=0x50000000;
  cfg.fixed_delta_us=delta;
  cfg.allow_zero_integration_updates=true;
  auto state=std::make_unique<b::StateV1>();
  CHECK(b::Initialize(cfg,state.get())==b::Result::kObserved);
  b::SubmitBeforeReceiptV1 submit{};
  CHECK(b::ObservePhysicsSubmitBeforeOriginal(cfg,state.get(),cfg.tick.expected_begin_owner,
    cfg.tick.expected_begin_owner_vptr,2,10,0,&submit)==b::Result::kIgnored);
  double residual=0;
  unsigned total_intervals=0, zero_frames=0;
  std::vector<g4::IntervalSampleV1> samples;
  std::vector<unsigned> counts;
  std::vector<a9tas::unified_tick_v1::RecordingFrameV1> packets;
  for(unsigned tick=0;tick<frames;++tick) {
    CHECK(b::ObservePhysicsSubmitBeforeOriginal(cfg,state.get(),cfg.tick.expected_begin_owner,
      cfg.tick.expected_begin_owner_vptr,3,10,0,&submit)==b::Result::kObserved);
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11)==b::Result::kIgnored);
    b::CompletedFrameScopeV1 scope{cfg.tick.game_base+cfg.tick.frame_event_scheduler_return_rva,1,11,tick};
    auto legacy=cfg; legacy.allow_zero_integration_updates=false;
    CHECK(b::ObserveFinalWriterReturn(legacy,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&scope)==b::Result::kIgnored);
    auto wrong=scope; wrong.generation++;
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&wrong)==b::Result::kIgnored);
    wrong=scope; wrong.caller_return++;
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&wrong)==b::Result::kIgnored);
    wrong=scope; wrong.tid++;
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&wrong)==b::Result::kIgnored);
    wrong=scope; wrong.tick++;
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&wrong)==b::Result::kIgnored);
    // Native accumulator contract: positive delta; zero, one or more steps.
    // Include dynamic 120Hz integration episodes, independently of logic rate.
    residual += delta / 1000000.;
    unsigned count=0;
    while(residual>0) {
      float step=(tick%97<9 ? 1.f/120 : 1.f/60);
      b::IntervalBeforeReceiptV1 before{}; g4::IntervalDecisionV1 after{};
      CHECK(b::ObserveIntervalBeforeOriginal(cfg,state.get(),cfg.tick.expected_interval_owner,
        cfg.tick.expected_interval_owner_vptr,3,10,0,&before)==
        (count==0 ? b::Result::kObserved : b::Result::kRepeatedInterval));
      CHECK(b::ObserveIntervalAfterOriginal(cfg,state.get(),g4::FloatBits(step),&after)==b::Result::kObserved);
      CHECK(!after.override_after_original);
      samples.push_back({tick,count,g4::FloatBits(step)});
      residual-=step; ++count; ++total_intervals;
    }
    counts.push_back(count);
    if(count==0) ++zero_frames;
    for (unsigned event=0; event<tick%3; ++event) {
      bool forward=false;
      CHECK(b::ObserveNaturalNitroCall(state.get(), &forward)==b::Result::kObserved);
      CHECK(forward);
    }
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&scope)==b::Result::kObserved);
    CHECK(b::ObserveTickEndReturn(cfg,state.get(),scope.caller_return,11)==
      (tick+1==frames?b::Result::kComplete:b::Result::kObserved));
    const auto& receipt=state->receipts[tick];
    CHECK(receipt.physics_interval_calls==count);
    a9tas::unified_tick_v1::RecordingFrameV1 packet{};
    CHECK(g4::BuildActionRecordingFrame(receipt,tick*static_cast<std::uint64_t>(delta)*1000,&packet,true)==g4::Result::kReady);
    if(!count) CHECK(g4::BuildActionRecordingFrame(receipt,0,&packet)==g4::Result::kInvalidArgument);
    g4::PhysicsSnapshotV1 snapshot{};
    snapshot.tick=tick; snapshot.captured=true;
    CHECK(g4::BuildPhysicsRecordingFrame(receipt,snapshot,
      tick*static_cast<std::uint64_t>(delta)*1000,&packet,true)==g4::Result::kReady);
    CHECK(g4::PhysicsRecordingFrameValid(packet));
    packets.push_back(packet);
  }
  CHECK(state->receipt_count==frames);
  CHECK(state->tick.coordinator.physics_interval_calls==total_intervals);
  CHECK(state->tick.coordinator.failures==0);
  if(hz>60) CHECK(zero_frames>0);
  // Replay diagnostic cursor must skip absent groups without consuming a
  // later tick's samples. Exercise all groups, including a zero-call tail.
  g4::StateV1 replay{};
  g4::IntervalReplayViewV1 view{samples.data(),samples.size()};
  CHECK(g4::IntervalSequenceValid(view,frames,true));
  CHECK(g4::IntervalSequenceValid(view,frames)==(zero_frames==0));
  for(unsigned tick=0;tick<frames;++tick) {
    replay.tick=tick; replay.tick_open=true; replay.replay_packet_present=true;
    replay.receipt.tick=tick; replay.receipt.physics_interval_calls=counts[tick];
    g4::TickReceiptV1 receipt{};
    CHECK(g4::EndTick(&replay,view,&receipt,true)==g4::Result::kReady);
  }
  CHECK(replay.interval_cursor==samples.size());
  // Replay the produced packets through the real G3/G4 bridge, not just the
  // low-level cursor. Source diagnostics do not force native integration.
  cfg.tick.replay_queue.mode=c::ReplayMode::kActiveBlock;
  cfg.tick.replay_queue.packets=packets.data();
  cfg.tick.replay_queue.packet_count=packets.size();
  cfg.interval_replay=view;
  state=std::make_unique<b::StateV1>();
  CHECK(b::Initialize(cfg,state.get())==b::Result::kObserved);
  CHECK(b::ObservePhysicsSubmitBeforeOriginal(cfg,state.get(),cfg.tick.expected_begin_owner,
    cfg.tick.expected_begin_owner_vptr,2,10,0,&submit)==b::Result::kIgnored);
  for(unsigned tick=0;tick<frames;++tick) {
    CHECK(b::ObservePhysicsSubmitBeforeOriginal(cfg,state.get(),cfg.tick.expected_begin_owner,
      cfg.tick.expected_begin_owner_vptr,3,10,0,&submit)==b::Result::kObserved);
    CHECK(state->input_action.replay_packet_present);
    for(unsigned ordinal=0;ordinal<counts[tick];++ordinal) {
      b::IntervalBeforeReceiptV1 before{}; g4::IntervalDecisionV1 after{};
      CHECK(b::ObserveIntervalBeforeOriginal(cfg,state.get(),cfg.tick.expected_interval_owner,
        cfg.tick.expected_interval_owner_vptr,3,10,0,&before)==
        (ordinal==0?b::Result::kObserved:b::Result::kRepeatedInterval));
      if (ordinal==0)
        CHECK(b::AccountInjectedNitroCalls(state.get(),tick%3)==b::Result::kObserved);
      CHECK(b::ObserveIntervalAfterOriginal(cfg,state.get(),g4::FloatBits(1.f/60),&after)==b::Result::kObserved);
      CHECK(!after.override_after_original);
    }
    b::CompletedFrameScopeV1 scope{cfg.tick.game_base+cfg.tick.frame_event_scheduler_return_rva,1,11,tick};
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
      cfg.tick.expected_player_vptr,11,&scope)==b::Result::kObserved);
    if (counts[tick]==0)
      CHECK(b::AccountInjectedNitroCalls(state.get(),tick%3)==b::Result::kObserved);
    CHECK(b::ObserveTickEndReturn(cfg,state.get(),scope.caller_return,11)==
      (tick+1==frames?b::Result::kComplete:b::Result::kObserved));
    CHECK(state->receipts[tick].injected_nitro_calls==tick%3);
  }
  CHECK(state->receipt_count==frames && state->tick.coordinator.replay_head==frames);
  CHECK(state->input_action.interval_cursor==samples.size());
  std::printf("ZERO_INTEGRATION hz=%u packets=%u steps=%u zero=%u passed=1\n",hz,frames,total_intervals,zero_frames);
  return true;
}
static bool malformed_groups() {
  // Reproduce the missing-Interval bug: nonzero planned event, zero native
  // calls. Do not consume the tick until the event is actually accounted.
  for (unsigned planned : {1u,2u}) {
    g4::StateV1 missed{};
    missed.tick_open=true; missed.replay_packet_present=true;
    missed.packet.nitro_activation_count=planned;
    g4::TickReceiptV1 done{};
    CHECK(g4::EndTick(&missed,{},&done,true)==g4::Result::kNitroCountMismatch);
    CHECK(missed.tick_open);
    CHECK(g4::AccountInjectedNitroCalls(&missed,planned)==g4::Result::kReady);
    CHECK(g4::EndTick(&missed,{},&done,true)==g4::Result::kReady);
    CHECK(done.recorded_nitro_calls==planned);
  }
  g4::IntervalSampleV1 samples[]={{1,0,g4::FloatBits(1.f/60)}, {3,0,g4::FloatBits(1.f/120)}};
  g4::IntervalReplayViewV1 view{samples,2};
  CHECK(g4::IntervalSequenceValid(view,5,true)); // leading, middle and trailing gaps
  CHECK(!g4::IntervalSequenceValid(view,5));
  CHECK(g4::IntervalSequenceValid({},5,true));
  CHECK(!g4::IntervalSequenceValid({},5));
  samples[1].ordinal=1; CHECK(!g4::IntervalSequenceValid(view,5,true));
  samples[1].ordinal=0; samples[1].tick=0; CHECK(!g4::IntervalSequenceValid(view,5,true));
  samples[1].tick=5; CHECK(!g4::IntervalSequenceValid(view,5,true));
  g4::StateV1 replay{}; replay.tick_open=true; replay.replay_packet_present=true;
  replay.interval_cursor=3; g4::TickReceiptV1 receipt{};
  CHECK(g4::EndTick(&replay,view,&receipt,true)==g4::Result::kIntervalMismatch);
  replay.interval_cursor=0; replay.tick=0;
  CHECK(g4::EndTick(&replay,{},&receipt,true)==g4::Result::kReady);
  std::puts("SPARSE_GROUPS leading_middle_tail_empty malformed_bounds passed=1");
  return true;
}
int main() { return malformed_groups()&&run(60,16667,9000)&&run(120,8333,18000)&&run(144,6944,21600) ? 0 : 1; }
