#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace a9tas::physics_initial_phase_v1 {

// Exact float bits, not an estimate from interval counts. Captured before the
// first Submit in a segment; replay must apply them at that same boundary.
struct Snapshot {
  std::uint32_t residual_bits{};
  std::uint32_t last_interval_bits{};
};
static_assert(sizeof(Snapshot) == 8);

inline float Float(std::uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
inline bool Valid(const Snapshot& phase) {
  const float residual = Float(phase.residual_bits);
  const float interval = Float(phase.last_interval_bits);
  return std::isfinite(residual) && std::isfinite(interval) && interval > 0 &&
      residual <= 0 && residual >= -interval;
}

struct Binding {
  std::uintptr_t context{};
  std::uintptr_t world{};
  std::uintptr_t update{};
  std::uintptr_t phase_address{};
  Snapshot phase{};
};

template <typename Read, typename T>
bool At(Read& read, std::uintptr_t object, std::size_t offset, T* value) {
  return object != 0 && object <= UINTPTR_MAX - offset &&
      object + offset <= UINTPTR_MAX - sizeof(T) &&
      read(object + offset, value, sizeof(T));
}

// Match instructions through live vtables, not channel names or old RVAs.
// These blocks prove both phase field offsets in the current update method.
inline bool UpdateLayoutProven(const std::uint32_t (&code)[52]) {
  bool add = false, subtract = false;
  for (std::size_t i = 0; i + 2 < 52; ++i) {
    add |= code[i] == 0xbd418a61 && code[i+1] == 0x1e212800 &&
           code[i+2] == 0xbd018a60;
    subtract |= code[i] == 0x1e213800 && code[i+1] == 0xb9018e68 &&
                code[i+2] == 0xbd018a60;
  }
  return add && subtract;
}

template <typename Read>
bool Resolve(Read& read, std::uintptr_t context, Binding* result) {
  if (!result) return false;
  std::uintptr_t object{};
  if (!At(read, context, 0x120, &object)) return false;
  for (unsigned depth = 0; depth != 3; ++depth) {
    std::uintptr_t vtable{}, update{};
    if (!At(read, object, 0, &vtable) || !At(read, vtable, 0x60, &update))
      return false;
    std::uint32_t code[52]{};
    if (!At(read, update, 0, &code)) return false;
    if (UpdateLayoutProven(code)) {
      Snapshot phase{};
      if (!At(read, object, 0x188, &phase) || !Valid(phase)) return false;
      *result = {context, object, update, object + 0x188, phase};
      return true;
    }
    // ldr x0,[x0,#0x120]; ldr x8,[x0]; ldr x3,[x8,#0x60]; br x3
    if (code[0] != 0xf9409000 || code[1] != 0xf9400008 ||
        code[2] != 0xf9403103 || code[3] != 0xd61f0060)
      return false;
    std::uintptr_t next{};
    if (!At(read, object, 0x120, &next) || next == object) return false;
    object = next;
  }
  return false;
}

// Call only at a physics-owner boundary; this checks bit-exact readback but
// does not itself provide thread synchronization or authorize a game write.
template <typename Read, typename Write>
bool Restore(Read& read, Write& write, const Binding& bound,
             const Snapshot& source) {
  if (!Valid(source)) return false;
  Binding current{};
  if (!Resolve(read, bound.context, &current) || current.world != bound.world ||
      current.update != bound.update || current.phase_address != bound.phase_address)
    return false;
  if (!write(current.phase_address, &source, sizeof(source))) return false;
  Snapshot after{};
  return At(read, current.world, 0x188, &after) &&
      std::memcmp(&after, &source, sizeof(source)) == 0;
}
}  // namespace a9tas::physics_initial_phase_v1
