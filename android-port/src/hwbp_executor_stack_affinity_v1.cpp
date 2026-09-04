// Host-only, observation-only proof that the game-owned simulation-token
// executor naturally runs on the selected FrameThread 0 task.
//
// This build reuses the audited worker stack observer transport, but watches
// only the low-frequency native pose publication and captures 256 stack words.
// The parser uses one exact PhysicsWorld phase return to select the integrated-
// state publisher from adjacent writers, then requires the exact
// PhysicsContext_execute_simulation_token BLR return PC on every captured pose
// writer, including adjacent copy/sync writers. It patches no guest instruction
// and writes no game value.

#define A9TAS_EXECUTOR_AFFINITY_STACK 1
#include "hwbp_worker_stack_scope_observer_v1.cpp"
