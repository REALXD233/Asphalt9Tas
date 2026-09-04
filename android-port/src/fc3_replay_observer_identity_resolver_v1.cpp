// FC-3 read-only identity resolver, review-only implementation.
//
// Default builds are inert.  The complete resolver is compiled only into an
// unlinked review object.  It performs untraced preflight reads and may scan
// readable/writable mappings to require a unique action owner.  It contains no
// target-memory write, ptrace attach, debug-register operation, game call,
// input injection, action submission, Nitro call, or physics correction.

#include <cstdio>

#include "fc3_identity_resolver_v1.h"

#ifndef A9TAS_FC3_IDENTITY_REVIEW
#define A9TAS_FC3_IDENTITY_REVIEW 0
#endif

#if A9TAS_FC3_IDENTITY_REVIEW != 0 && A9TAS_FC3_IDENTITY_REVIEW != 1
#error "A9TAS_FC3_IDENTITY_REVIEW must be 0 or 1"
#endif

#if A9TAS_FC3_IDENTITY_REVIEW == 1

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"
#include "fc2_payload_elf_resolver_v1.h"

#include <algorithm>
#include <climits>

namespace {

using a9tas::fc3_identity_v1::Layout;

constexpr std::uintptr_t kFc3CallbackListOffset = 0x180;
constexpr std::uintptr_t kFc3CallbackFlagsOffset = 0x1A0;
constexpr std::uintptr_t kFc3ReplayFixedDeltaOffset = 0x150;
constexpr std::uintptr_t kFc3PhaseWitnessOffset = 0x188;
constexpr std::uintptr_t kFc3CarPrimaryVtableRva = 0x7EE8D18;
constexpr std::uintptr_t kFc3ActionVectorOffset = 0x1360;
constexpr std::uintptr_t kFc3DirectModeOffset = 0x1378;
constexpr std::uintptr_t kFc3ActionFallbackVtableRva = 0x7EEA080;
constexpr std::uintptr_t kFc3ActionFallbackVfunc90Rva = 0x367B548;
constexpr std::uintptr_t kFc3ActionOwnerAdjustMetadataDelta = 0xA0;
constexpr std::uintptr_t kFc3ActionDispatchAdjustMetadataDelta = 0x230;
constexpr std::uintptr_t kFc3ActionDispatchVtableRva = 0x7EEFE68;
constexpr std::uintptr_t kFc3ActionDispatchVfunc158Rva = 0x36A9CAC;
constexpr std::uintptr_t kFc3NitroServiceOffset = 0xCB8;
constexpr std::uintptr_t kFc3NitroServiceVtableRva = 0x7EE8A90;
constexpr std::uintptr_t kFc3NitroActivateSlot = 0x108;
constexpr std::uintptr_t kFc3NitroActivateRva = 0x3674E50;
constexpr std::uintptr_t kFc3NitroStateOffset = 8;
constexpr std::uintptr_t kFc3NitroActiveOffset = 0x188;
constexpr std::uintptr_t kFc3NitroModeOffset = 0x18C;
constexpr std::size_t kFc3MaximumQueueCount = 4096;

struct CallbackListHeader {
  std::uintptr_t begin;
  std::uintptr_t end;
  std::uintptr_t capacity_end;
  std::uintptr_t active_end;
  std::uint8_t dispatching;
  std::uint8_t deferred;
  std::uint8_t padding[6];
  std::uint64_t member_function;
  std::uint64_t this_adjustment;
};

struct CallbackEntry {
  std::uint64_t reserved;
  std::uintptr_t object;
};

struct ActionQueue {
  std::uintptr_t begin;
  std::uintptr_t end;
  std::uintptr_t capacity;
  std::uint64_t count;
  std::uint8_t direct_mode;
};

static_assert(sizeof(CallbackListHeader) == 56, "FC-3 callback-list ABI");
static_assert(sizeof(CallbackEntry) == 16, "FC-3 callback-entry ABI");

bool CheckedAdd(std::uintptr_t base, std::uintptr_t offset,
                std::uintptr_t* output) {
  if (!output || base > UINTPTR_MAX - offset) return false;
  *output = base + offset;
  return true;
}

bool Fc3ReadableWritable(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
  const Mapping* mapping = FindMapping(maps, address, size);
  return mapping && mapping->perms[0] == 'r' && mapping->perms[1] == 'w';
}

bool ReadCallbackList(int mem, const std::vector<Mapping>& maps,
                      std::uintptr_t address, CallbackListHeader* output) {
  CallbackListHeader header{};
  if (!ReadExact(mem, address, &header, sizeof(header)) || header.begin == 0 ||
      header.begin > header.active_end || header.active_end > header.end ||
      header.end > header.capacity_end ||
      ((header.active_end - header.begin) % sizeof(CallbackEntry)) != 0 ||
      ((header.end - header.begin) % sizeof(CallbackEntry)) != 0 ||
      header.end - header.begin > 4096u * sizeof(CallbackEntry) ||
      !FindMapping(maps, header.begin,
                   static_cast<std::size_t>(header.end - header.begin)) ||
      header.dispatching != 0 || header.deferred != 0 ||
      header.member_function != 0x10 || header.this_adjustment != 1)
    return false;
  *output = header;
  return true;
}

bool CountCallbackObject(int mem, const CallbackListHeader& header,
                         std::uintptr_t object, bool active_only,
                         std::uint32_t* output) {
  const std::uintptr_t limit = active_only ? header.active_end : header.end;
  std::uint32_t count = 0;
  for (std::uintptr_t cursor = header.begin; cursor < limit;
       cursor += sizeof(CallbackEntry)) {
    CallbackEntry entry{};
    if (!ReadExact(mem, cursor, &entry, sizeof(entry))) return false;
    if (entry.object != object) continue;
    if (entry.reserved != 0) return false;
    ++count;
  }
  *output = count;
  return true;
}

bool ReadActionQueue(int mem, std::uintptr_t owner, ActionQueue* output) {
  ActionQueue queue{};
  std::uintptr_t vector = 0, vector_end = 0, vector_capacity = 0;
  std::uintptr_t direct_mode = 0;
  if (!CheckedAdd(owner, kFc3ActionVectorOffset, &vector) ||
      !CheckedAdd(vector, 8, &vector_end) ||
      !CheckedAdd(vector, 16, &vector_capacity) ||
      !CheckedAdd(owner, kFc3DirectModeOffset, &direct_mode) ||
      !ReadExact(mem, vector, &queue.begin,
                  sizeof(queue.begin)) ||
      !ReadExact(mem, vector_end, &queue.end,
                  sizeof(queue.end)) ||
      !ReadExact(mem, vector_capacity, &queue.capacity,
                  sizeof(queue.capacity)) ||
      !ReadExact(mem, direct_mode, &queue.direct_mode,
                  sizeof(queue.direct_mode)) ||
      queue.direct_mode > 1 || queue.begin > queue.end ||
      queue.end > queue.capacity || ((queue.end - queue.begin) & 7u) != 0 ||
      ((queue.capacity - queue.begin) & 7u) != 0)
    return false;
  queue.count = (queue.end - queue.begin) / 8u;
  if (queue.count > kFc3MaximumQueueCount) return false;
  *output = queue;
  return true;
}

bool ValidateActionOwner(int mem, const std::vector<Mapping>& maps,
                         std::uintptr_t base, std::uintptr_t owner,
                         ActionQueue* queue_out) {
  ActionQueue queue{};
  std::uintptr_t command_interface = 0;
  std::uintptr_t interface_head = 0;
  std::int64_t interface_adjustment = 0;
  std::uintptr_t command_interface_address = 0;
  if (owner == 0 || (owner & 7u) != 0 ||
      !CheckedAdd(owner, 0x30, &command_interface_address) ||
      !Fc3ReadableWritable(maps, command_interface_address,
                           sizeof(command_interface)) ||
      !ReadExact(mem, command_interface_address, &command_interface,
                  sizeof(command_interface)) ||
      !ReadActionQueue(mem, owner, &queue) ||
      !FindMapping(maps, command_interface, sizeof(interface_head)) ||
      !ReadExact(mem, command_interface, &interface_head,
                 sizeof(interface_head)) ||
      interface_head < kFc3ActionDispatchAdjustMetadataDelta ||
      !FindMapping(maps,
                   interface_head - kFc3ActionDispatchAdjustMetadataDelta,
                   sizeof(interface_adjustment)) ||
      !ReadExact(mem,
                 interface_head - kFc3ActionDispatchAdjustMetadataDelta,
                 &interface_adjustment, sizeof(interface_adjustment)))
    return false;

  const auto signed_interface = static_cast<std::intptr_t>(command_interface);
  if ((interface_adjustment > 0 &&
       signed_interface > INTPTR_MAX - interface_adjustment) ||
      (interface_adjustment < 0 &&
       signed_interface < INTPTR_MIN - interface_adjustment))
    return false;
  const auto signed_dispatch = signed_interface + interface_adjustment;
  if (signed_dispatch <= 0) return false;
  const auto dispatch_this = static_cast<std::uintptr_t>(signed_dispatch);
  std::uintptr_t dispatch_vtable = 0;
  std::uintptr_t dispatch_vfunc = 0;
  std::uintptr_t dispatch_slot = 0;
  std::uintptr_t expected_dispatch_vtable = 0;
  std::uintptr_t expected_dispatch_vfunc = 0;
  if (!FindMapping(maps, dispatch_this, sizeof(dispatch_vtable)) ||
      !ReadExact(mem, dispatch_this, &dispatch_vtable,
                  sizeof(dispatch_vtable)) ||
      !CheckedAdd(base, kFc3ActionDispatchVtableRva,
                  &expected_dispatch_vtable) ||
      dispatch_vtable != expected_dispatch_vtable ||
      !CheckedAdd(dispatch_vtable, 0x158, &dispatch_slot) ||
      !FindMapping(maps, dispatch_slot,
                   sizeof(dispatch_vfunc)) ||
      !ReadExact(mem, dispatch_slot, &dispatch_vfunc,
                  sizeof(dispatch_vfunc)) ||
      !CheckedAdd(base, kFc3ActionDispatchVfunc158Rva,
                  &expected_dispatch_vfunc) ||
      dispatch_vfunc != expected_dispatch_vfunc)
    return false;
  *queue_out = queue;
  return true;
}

bool ResolveUniqueActionOwner(int mem, const std::vector<Mapping>& maps,
                              std::uintptr_t base,
                              std::uintptr_t* owner_out,
                              ActionQueue* queue_out) {
  std::uintptr_t fallback_vtable = 0;
  std::uintptr_t fallback_slot = 0;
  std::uintptr_t expected_fallback_vfunc = 0;
  std::uintptr_t fallback_vfunc = 0;
  std::int64_t owner_adjustment = 0;
  if (!CheckedAdd(base, kFc3ActionFallbackVtableRva, &fallback_vtable) ||
      !CheckedAdd(fallback_vtable, 0x90, &fallback_slot) ||
      !CheckedAdd(base, kFc3ActionFallbackVfunc90Rva,
                  &expected_fallback_vfunc) ||
      !FindMapping(maps, fallback_slot,
                   sizeof(fallback_vfunc)) ||
      !ReadExact(mem, fallback_slot, &fallback_vfunc,
                  sizeof(fallback_vfunc)) ||
      fallback_vfunc != expected_fallback_vfunc ||
      fallback_vtable < kFc3ActionOwnerAdjustMetadataDelta ||
      !FindMapping(maps,
                   fallback_vtable - kFc3ActionOwnerAdjustMetadataDelta,
                   sizeof(owner_adjustment)) ||
      !ReadExact(mem,
                 fallback_vtable - kFc3ActionOwnerAdjustMetadataDelta,
                 &owner_adjustment, sizeof(owner_adjustment)))
    return false;

  std::vector<std::uintptr_t> candidates;
  std::vector<std::uint8_t> buffer(1u << 20);
  for (const auto& mapping : maps) {
    if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w' ||
        mapping.path == "[vvar]" || mapping.path == "[vdso]")
      continue;
    for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
      const std::size_t want = static_cast<std::size_t>(
          std::min<std::uintptr_t>(buffer.size(), mapping.end - cursor));
      const ssize_t got =
          pread(mem, buffer.data(), want, static_cast<off_t>(cursor));
      if (got <= 0) {
        cursor += want;
        continue;
      }
      for (std::size_t offset = 0;
           offset + sizeof(std::uintptr_t) <= static_cast<std::size_t>(got);
           offset += alignof(std::uintptr_t)) {
        std::uintptr_t value = 0;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        if (value != fallback_vtable) continue;
        const auto signed_fallback = static_cast<std::intptr_t>(
            cursor + static_cast<std::uintptr_t>(offset));
        if ((owner_adjustment > 0 &&
             signed_fallback > INTPTR_MAX - owner_adjustment) ||
            (owner_adjustment < 0 &&
             signed_fallback < INTPTR_MIN - owner_adjustment))
          continue;
        const auto signed_owner = signed_fallback + owner_adjustment;
        if (signed_owner <= 0) continue;
        const auto owner = static_cast<std::uintptr_t>(signed_owner);
        ActionQueue queue{};
        if (ValidateActionOwner(mem, maps, base, owner, &queue))
          candidates.push_back(owner);
      }
      cursor += static_cast<std::uintptr_t>(got);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  if (candidates.size() != 1) return false;
  ActionQueue queue{};
  if (!ValidateActionOwner(mem, maps, base, candidates.front(), &queue))
    return false;
  *owner_out = candidates.front();
  *queue_out = queue;
  return true;
}

bool ResolveNitroIdentity(int mem, const std::vector<Mapping>& maps,
                          std::uintptr_t base, std::uintptr_t physics_owner,
                          std::uintptr_t* service_out,
                          std::uintptr_t* state_out,
                          std::uint8_t* active_out,
                          std::uint32_t* mode_out) {
  std::uintptr_t service = 0;
  std::uintptr_t vtable = 0;
  std::uintptr_t activate = 0;
  std::uintptr_t service_address = 0;
  std::uintptr_t expected_vtable = 0;
  std::uintptr_t activate_slot = 0;
  std::uintptr_t expected_activate = 0;
  if (!CheckedAdd(physics_owner, kFc3NitroServiceOffset,
                  &service_address) ||
      !Fc3ReadableWritable(maps, service_address,
                           sizeof(service)) ||
      !ReadExact(mem, service_address, &service,
                  sizeof(service)) ||
      service == 0 || (service & 7u) != 0 ||
      !Fc3ReadableWritable(maps, service, sizeof(vtable)) ||
      !ReadExact(mem, service, &vtable, sizeof(vtable)) ||
      !CheckedAdd(base, kFc3NitroServiceVtableRva, &expected_vtable) ||
      vtable != expected_vtable ||
      !CheckedAdd(vtable, kFc3NitroActivateSlot, &activate_slot) ||
      !FindMapping(maps, activate_slot, sizeof(activate)) ||
      !ReadExact(mem, activate_slot, &activate,
                  sizeof(activate)) ||
      !CheckedAdd(base, kFc3NitroActivateRva, &expected_activate) ||
      activate != expected_activate)
    return false;
  std::uintptr_t state = 0;
  std::uintptr_t active_address = 0;
  std::uintptr_t mode_address = 0;
  std::uint8_t active = 0;
  std::uint32_t mode = 0;
  if (!CheckedAdd(service, kFc3NitroStateOffset, &state) ||
      !CheckedAdd(state, kFc3NitroActiveOffset, &active_address) ||
      !CheckedAdd(state, kFc3NitroModeOffset, &mode_address) ||
      !Fc3ReadableWritable(maps, active_address, 1) ||
      !Fc3ReadableWritable(maps, mode_address, 4) ||
      !ReadExact(mem, active_address, &active,
                  sizeof(active)) ||
      !ReadExact(mem, mode_address, &mode, sizeof(mode)) ||
      active > 1)
    return false;
  *service_out = service;
  *state_out = state;
  *active_out = active;
  *mode_out = mode;
  return true;
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) int
a9tas_fc3_resolve_identities_review_v1(pid_t pid, int process_mem,
                                       std::uintptr_t base, Layout* output) {
  if (pid <= 0 || process_mem < 0 || base == 0 || output == nullptr ||
      !VerifyTargetBuild(pid, base))
    return -1;

  a9tas::fc2_payload_elf_v1::Layout payload{};
  std::uintptr_t main_object = 0;
  std::uintptr_t physics_owner = 0;
  std::uintptr_t context = 0;
  std::uintptr_t adapter = 0;
  std::uintptr_t world = 0;
  a9tas::vehicle_state_v1::Layout vehicle{};
  if (!a9tas::fc2_payload_elf_v1::Resolve(pid, process_mem, &payload) ||
      !ResolveMainObject(pid, base, 0, &main_object) ||
      !ResolveFinalOwner(pid, base, 0, &physics_owner) ||
      !ResolvePhysicsContext(pid, process_mem, base, 0, &context, &adapter,
                             &world) ||
      !a9tas::vehicle_state_v1::Resolve(pid, process_mem, base, &vehicle))
    return -2;

  std::vector<Mapping> maps;
  if (!ReadMaps(pid, &maps)) return -3;
  std::uintptr_t callback_list = 0, callback_flags = 0;
  std::uintptr_t replay_fixed_delta = 0, phase_witness = 0;
  std::uintptr_t expected_car_vptr = 0;
  if (!CheckedAdd(context, kFc3CallbackListOffset, &callback_list) ||
      !CheckedAdd(context, kFc3CallbackFlagsOffset, &callback_flags) ||
      !CheckedAdd(main_object, kFc3ReplayFixedDeltaOffset,
                  &replay_fixed_delta) ||
      !CheckedAdd(world, kFc3PhaseWitnessOffset, &phase_witness) ||
      !CheckedAdd(base, kFc3CarPrimaryVtableRva, &expected_car_vptr) ||
      !Fc3ReadableWritable(maps, replay_fixed_delta, 8) ||
      !Fc3ReadableWritable(maps, phase_witness, 4) ||
      !Fc3ReadableWritable(maps, callback_flags, 2))
    return -4;

  std::uintptr_t car_vptr = 0;
  CallbackListHeader callback_header{};
  std::uint32_t car_active = 0;
  std::uint32_t car_full = 0;
  std::uint32_t dedicated_full = 0;
  if (!ReadExact(process_mem, vehicle.physics_base, &car_vptr,
                  sizeof(car_vptr)) ||
      car_vptr != expected_car_vptr ||
      !ReadCallbackList(process_mem, maps, callback_list, &callback_header) ||
      !CountCallbackObject(process_mem, callback_header,
                           vehicle.physics_base, true, &car_active) ||
      !CountCallbackObject(process_mem, callback_header,
                           vehicle.physics_base, false, &car_full) ||
      !CountCallbackObject(process_mem, callback_header,
                           payload.dedicated_object, false,
                           &dedicated_full) ||
      car_active != 1 || car_full != 1 || dedicated_full != 0)
    return -5;

  std::uintptr_t action_owner = 0;
  ActionQueue queue{};
  if (!ResolveUniqueActionOwner(process_mem, maps, base, &action_owner,
                                &queue))
    return -6;

  std::uintptr_t nitro_service = 0;
  std::uintptr_t nitro_state = 0;
  std::uint8_t nitro_active = 0;
  std::uint32_t nitro_mode = 0;
  if (!ResolveNitroIdentity(process_mem, maps, base, physics_owner,
                            &nitro_service, &nitro_state, &nitro_active,
                            &nitro_mode))
    return -7;

  Layout layout{};
  const char magic[8] = {'A', '9', 'F', 'C', '3', 'I', '1', 0};
  std::memcpy(layout.magic, magic, sizeof(magic));
  layout.version = 1;
  layout.size = sizeof(layout);
  layout.pid = static_cast<std::uint64_t>(pid);
  layout.library_base = base;
  layout.main_time_source_owner = main_object;
  layout.replay_fixed_delta_input = replay_fixed_delta;
  layout.physics_owner = physics_owner;
  layout.physics_context = context;
  layout.callback_list = callback_list;
  layout.callback_flags = callback_flags;
  layout.car_physics_state = vehicle.physics_base;
  layout.physics_backend_world = world;
  layout.phase_witness_accumulator = phase_witness;
  layout.payload_load_bias = payload.load_bias;
  layout.payload_evidence = payload.evidence;
  layout.dedicated_object = payload.dedicated_object;
  layout.action_owner = action_owner;
  layout.action_queue_begin = queue.begin;
  layout.action_queue_end = queue.end;
  layout.action_queue_capacity = queue.capacity;
  layout.nitro_service = nitro_service;
  layout.nitro_state = nitro_state;
  if (!CheckedAdd(nitro_state, kFc3NitroActiveOffset,
                  &layout.nitro_active_address) ||
      !CheckedAdd(nitro_state, kFc3NitroModeOffset,
                  &layout.nitro_mode_address))
    return -8;
  layout.initial_action_queue_count = queue.count;
  layout.initial_nitro_mode = nitro_mode;
  layout.initial_direct_mode = queue.direct_mode;
  layout.initial_nitro_active = nitro_active;
  *output = layout;
  return 0;
}

#else

int main() {
  std::puts("FC3_IDENTITY_BUILD_ONLY runtime=disabled return=-100 "
            "device_access=0 process_reads=0 attached=0 game_writes=0");
  return 100;
}

#endif
