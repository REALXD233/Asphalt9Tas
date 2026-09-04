// Isolated build wrapper for the exact game-owned action scheduling route.
// Nonzero requests remain compile-time disabled.  Keeping this as a separate
// translation unit gives the action prototype a distinct output and transport
// identity.  The proven Gate 11 runner remains hash-pinned and must reject any
// newly rebuilt default payload until it is reviewed separately.

#define A9TAS_GAME_ACTION_RPC_V1 1
#define A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE 0
#include "payload_nitro_rpc_v1.cpp"
