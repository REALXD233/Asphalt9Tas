#include "g4_g3_adapter_v1.h"
#include <cstdio>
#include <memory>
namespace b = a9tas::g4_g3_adapter_v1;
namespace g3 = a9tas::g3_boundary_adapter_v1;
namespace c = a9tas::g3_tick_coordinator_v1;
namespace g4 = a9tas::g4_input_action_v1;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line=%d\n", __LINE__); return 1; } } while (0)
int main() {
  b::ConfigV1 cfg{};
  cfg.tick.session_id=1; cfg.tick.generation=1; cfg.tick.frame_limit=2;
  cfg.tick.game_base=0x10000000;
  cfg.tick.expected_begin_owner=0x20000000;
  cfg.tick.expected_begin_owner_vptr=cfg.tick.game_base+g3::kPhysicsContextVtableRva;
  cfg.tick.expected_interval_owner=0x30000000;
  cfg.tick.expected_interval_owner_vptr=cfg.tick.game_base+g3::kPhysicsImplementationVtableRva;
  cfg.tick.expected_player=0x40000000; cfg.tick.expected_player_vptr=0x50000000;
  cfg.fixed_delta_us=16667;
  auto state=std::make_unique<b::StateV1>();
  CHECK(b::Initialize(cfg,state.get())==b::Result::kObserved);
  b::SubmitBeforeReceiptV1 submit{};
  auto begin=[&](unsigned phase) { return b::ObservePhysicsSubmitBeforeOriginal(cfg,state.get(),
      cfg.tick.expected_begin_owner,cfg.tick.expected_begin_owner_vptr,phase,10,0,&submit); };
  CHECK(begin(2)==b::Result::kIgnored);
  for(unsigned tick=0;tick<2;++tick) {
    CHECK(begin(3)==b::Result::kObserved);
    auto incomplete=std::make_unique<g3::State>(state->tick);
    CHECK(g3::ObserveFrameEventReturn(cfg.tick,incomplete.get(),
      cfg.tick.game_base+cfg.tick.frame_event_scheduler_return_rva,11)==g3::Result::kCoreFault);
    CHECK(incomplete->coordinator.last_result==c::Result::kWrongOrder);
    CHECK(incomplete->receipt_count==tick);
    for(unsigned i=0;i<3;++i) {
      CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
          cfg.tick.expected_player_vptr,11)==b::Result::kIgnored);
      CHECK(state->tick.coordinator.tick_phase==c::TickPhase::kBegun);
      CHECK(state->tick.coordinator.tick==tick && state->receipt_count==tick);
      CHECK(state->tick.coordinator.failures==0 && state->input_action.tick_open);
    }
    b::IntervalBeforeReceiptV1 before{}; g4::IntervalDecisionV1 after{};
    CHECK(b::ObserveIntervalBeforeOriginal(cfg,state.get(),cfg.tick.expected_interval_owner,
        cfg.tick.expected_interval_owner_vptr,3,10,0,&before)==b::Result::kObserved);
    CHECK(b::ObserveIntervalAfterOriginal(cfg,state.get(),g4::FloatBits(1.f/60),&after)==b::Result::kObserved);
    CHECK(b::ObserveFinalWriterReturn(cfg,state.get(),cfg.tick.expected_player,
        cfg.tick.expected_player_vptr,11)==b::Result::kObserved);
    CHECK(b::ObserveTickEndReturn(cfg,state.get(),
        cfg.tick.game_base+cfg.tick.frame_event_scheduler_return_rva,11)==
        (tick==1?b::Result::kComplete:b::Result::kObserved));
  }
  CHECK(state->receipt_count==2 && state->tick.coordinator.physics_interval_calls==2);
  CHECK(state->tick.coordinator.final_writer_events==2);
  std::puts("EARLY_FINAL_BOUNDARY passed=1 ticks=2 early_returns=6 missing_interval_rejected=2");
}
