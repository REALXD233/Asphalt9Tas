#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::fc3_identity_v1 {

#pragma pack(push, 1)
struct Layout {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t pid;
  std::uint64_t library_base;
  std::uint64_t main_time_source_owner;
  std::uint64_t replay_fixed_delta_input;
  std::uint64_t physics_owner;
  std::uint64_t physics_context;
  std::uint64_t callback_list;
  std::uint64_t callback_flags;
  std::uint64_t car_physics_state;
  std::uint64_t physics_backend_world;
  std::uint64_t phase_witness_accumulator;
  std::uint64_t payload_load_bias;
  std::uint64_t payload_evidence;
  std::uint64_t dedicated_object;
  std::uint64_t action_owner;
  std::uint64_t action_queue_begin;
  std::uint64_t action_queue_end;
  std::uint64_t action_queue_capacity;
  std::uint64_t nitro_service;
  std::uint64_t nitro_state;
  std::uint64_t nitro_active_address;
  std::uint64_t nitro_mode_address;
  std::uint64_t initial_action_queue_count;
  std::uint32_t initial_nitro_mode;
  std::uint8_t initial_direct_mode;
  std::uint8_t initial_nitro_active;
  std::uint8_t reserved[2];
};
#pragma pack(pop)

static_assert(sizeof(Layout) == 208, "FC-3 identity layout ABI");
static_assert(offsetof(Layout, replay_fixed_delta_input) == 40,
              "FC-3 fixed-delta identity offset");
static_assert(offsetof(Layout, phase_witness_accumulator) == 96,
              "FC-3 phase-witness identity offset");
static_assert(offsetof(Layout, action_owner) == 128,
              "FC-3 action identity offset");

}  // namespace a9tas::fc3_identity_v1
