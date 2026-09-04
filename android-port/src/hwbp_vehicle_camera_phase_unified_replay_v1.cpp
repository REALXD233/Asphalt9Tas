// Diagnostic build of the proven final-writer unified executor.  Only payload
// resolution changes: the installed final-writer slot points to the outer
// phase-observer wrapper contained in the hash-pinned combined ELF.

// Preload both resolver definitions before redirecting the resolver namespace.
// The production integration header remains byte-for-byte untouched; in this
// diagnostic translation unit only, its Layout/Resolve references bind to the
// combined payload resolver.  The resolver exposes the same base Layout ABI,
// with the outer vehicle observer as Layout::wrapper.
#include "vehicle_camera_phase_observer_elf_resolver_v1.h"
#define final_writer_replay_elf_v1 vehicle_camera_phase_observer_elf_v1
#include "hwbp_lifecycle_final_writer_replay_v1.cpp"
#undef final_writer_replay_elf_v1
