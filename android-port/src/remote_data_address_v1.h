#pragma once
#include <cstdint>

namespace a9tas::remote_data_address_v1 {
// For external /proc/PID/mem offsets and map lookup only. Never rewrite a
// target-owned pointer: its original tag is needed by native/MTE accesses.
constexpr std::uintptr_t Untag(std::uintptr_t value) {
    return value & static_cast<std::uintptr_t>(0x00ffffffffffffffULL);
}
static_assert(Untag(0xb400006edd5f4e00ULL) == 0x6edd5f4e00ULL);
static_assert(Untag(0x6edd5f4e00ULL) == 0x6edd5f4e00ULL);
static_assert(Untag(0) == 0);
}
