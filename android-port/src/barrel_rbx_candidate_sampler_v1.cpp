// Read-only runtime sampler for the static BarrelRoll/RBX field candidate.
//
// Resolves the velocity-source owner whose +0x18 interface is the exact object
// used by the late stabilization chain, then polls owner +0x1968/+0x196C, the
// stunt-state word at +0x1D80, and the native angular vector.  It never attaches
// with ptrace, invokes a guest function, or opens game memory writable.

#include "vehicle_state_resolver_v1.h"

#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <time.h>
#include <unistd.h>

namespace {

constexpr std::uintptr_t kBarrelRbxCandidateOffset = 0x1968;
constexpr std::uintptr_t kVehicleStuntStateOffset = 0x1D80;
constexpr std::uint64_t kMaximumDurationMs = 30000;
constexpr std::uint64_t kMaximumChangeReceipts = 128;

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
    if (!text || !*text || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') return false;
    *output = value;
    return true;
}

std::uint64_t MonotonicNs() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

bool SameBits(const float* lhs, const float* rhs, std::size_t count) {
    return std::memcmp(lhs, rhs, count * sizeof(float)) == 0;
}

bool NonZero(const float* values, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        if (std::fabs(values[index]) > 0.000001f) return true;
    return false;
}

void UpdateRange(const float* values, std::size_t count, float* minimum,
                 float* maximum) {
    for (std::size_t index = 0; index < count; ++index) {
        if (values[index] < minimum[index]) minimum[index] = values[index];
        if (values[index] > maximum[index]) maximum[index] = values[index];
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS INTERVAL_US\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t duration_ms = 0;
    std::uint64_t interval_us = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_ms) ||
        !ParseUnsigned(argv[4], 10, &interval_us) || pid_value == 0 ||
        base_value == 0 || duration_ms < 100 ||
        duration_ms > kMaximumDurationMs || interval_us < 250 ||
        interval_us > 100000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 3;

    a9tas::vehicle_state_v1::Layout vehicle{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                       &backend) ||
        backend.angular_source_base == 0 ||
        backend.angular_source_base >
            UINTPTR_MAX - kVehicleStuntStateOffset - sizeof(std::uint32_t)) {
        std::fprintf(stderr, "vehicle/backend resolution failed stage=%u\n",
                     backend.failure_stage);
        close(mem);
        return 4;
    }
    const std::uintptr_t stabilization_owner = backend.angular_source_base;
    const std::uintptr_t values_address =
        stabilization_owner + kBarrelRbxCandidateOffset;
    const std::uintptr_t state_address =
        stabilization_owner + kVehicleStuntStateOffset;

    float previous[2]{};
    float minimum[2] = {std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
    float maximum[2] = {-std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()};
    float angular_minimum[3] = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    float angular_maximum[3] = {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    bool have_previous = false;
    std::uint64_t samples = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t nonzero_samples = 0;
    std::uint64_t nonzero_state_samples = 0;
    std::uint64_t changes = 0;
    std::uint64_t printed_changes = 0;
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + duration_ms * 1000000ULL;
    if (start_ns == 0) {
        close(mem);
        return 5;
    }

    std::printf(
        "BARREL_RBX_CANDIDATE_V1_ARMED pid=%d base=0x%" PRIxPTR
        " owner=0x%" PRIxPTR " values=0x%" PRIxPTR
        " state=0x%" PRIxPTR " angular=0x%" PRIxPTR
        " duration_ms=%" PRIu64 " interval_us=%" PRIu64
        " ptrace=0 game_writes=0\n",
        static_cast<int>(pid), base, stabilization_owner, values_address,
        state_address, backend.native_angular_address, duration_ms,
        interval_us);
    std::fflush(stdout);

    while (MonotonicNs() < deadline_ns) {
        float values[2]{};
        float angular[3]{};
        std::uint32_t state = 0;
        const bool read_ok =
            a9tas::vehicle_state_v1::ReadExact(
                mem, values_address, values, sizeof(values)) &&
            a9tas::vehicle_state_v1::ReadExact(
                mem, state_address, &state, sizeof(state)) &&
            a9tas::vehicle_state_v1::ReadExact(
                mem, backend.native_angular_address, angular,
                sizeof(angular)) &&
            a9tas::vehicle_state_v1::Finite(values, 2) &&
            a9tas::vehicle_state_v1::Finite(angular, 3);
        if (!read_ok) {
            ++read_errors;
            usleep(static_cast<useconds_t>(interval_us));
            continue;
        }
        ++samples;
        UpdateRange(values, 2, minimum, maximum);
        UpdateRange(angular, 3, angular_minimum, angular_maximum);
        if (NonZero(values, 2)) ++nonzero_samples;
        if (state != 0) ++nonzero_state_samples;
        const bool changed = have_previous && !SameBits(values, previous, 2);
        if (changed) {
            ++changes;
            if (printed_changes < kMaximumChangeReceipts) {
                const std::uint64_t elapsed_us =
                    (MonotonicNs() - start_ns) / 1000ULL;
                std::printf(
                    "CHANGE index=%" PRIu64 " elapsed_us=%" PRIu64
                    " state=%" PRIu32 " values=(%.9g,%.9g)"
                    " angular=(%.9g,%.9g,%.9g)\n",
                    samples - 1, elapsed_us, state, values[0], values[1],
                    angular[0], angular[1], angular[2]);
                ++printed_changes;
            }
        }
        std::memcpy(previous, values, sizeof(previous));
        have_previous = true;
        usleep(static_cast<useconds_t>(interval_us));
    }
    close(mem);

    std::printf(
        "BARREL_RBX_CANDIDATE_V1_DONE samples=%" PRIu64
        " read_errors=%" PRIu64 " nonzero=%" PRIu64
        " nonzero_state=%" PRIu64 " changes=%" PRIu64
        " printed_changes=%" PRIu64
        " min=(%.9g,%.9g) max=(%.9g,%.9g)"
        " angular_min=(%.9g,%.9g,%.9g)"
        " angular_max=(%.9g,%.9g,%.9g)\n",
        samples, read_errors, nonzero_samples, nonzero_state_samples, changes,
        printed_changes, minimum[0], minimum[1], maximum[0], maximum[1],
        angular_minimum[0], angular_minimum[1], angular_minimum[2],
        angular_maximum[0], angular_maximum[1], angular_maximum[2]);
    return samples > 0 && read_errors == 0 ? 0 : 6;
}
