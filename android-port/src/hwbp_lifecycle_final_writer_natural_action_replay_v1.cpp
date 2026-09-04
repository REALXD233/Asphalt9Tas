// Review-only composition of the lifecycle-bound final writer and the
// hash-pinned AluTasV2-style per-frame natural-action mailbox.

#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 2
#define A9TAS_FINAL_WRITER_NATURAL_ACTION_V1 1
#define A9TAS_COMPLETION_WRITE_CERTIFICATE_V1 1
#include "hwbp_lifecycle_final_writer_replay_v1.cpp"
