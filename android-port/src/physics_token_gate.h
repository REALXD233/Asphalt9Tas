#pragma once

#include <atomic>
#include <cstdint>

namespace a9tas {

// Pure token-level state machine for a future in-guest ARM64 hook at
// PhysicsContext_execute_simulation_token.  It owns no game pointers and does
// not install hooks.  The default state is a complete pass-through.
class PhysicsTokenGate {
public:
    enum class Mode : std::uint32_t {
        kPassThrough = 0,
        kFrozen = 1,
    };

    struct Decision {
        std::uint64_t token_us;
        bool substituted;
        bool released_step;
    };

    Decision Filter(std::uint64_t real_token_us) {
        std::uint64_t control = control_.load(std::memory_order_acquire);
        if (!IsFrozen(control)) return {real_token_us, false, false};

        while (PendingSteps(control) != 0) {
            const std::uint64_t next = control - kPendingUnit;
            if (control_.compare_exchange_weak(
                    control, next, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return {NextFixedToken(), true, true};
            }
            if (!IsFrozen(control)) return {real_token_us, false, false};
        }
        // The original executor must still be called with this zero token so
        // worker-queue completion and surrounding callbacks retain game-owned
        // ordering while the inner world takes its proven dt==0 early return.
        return {0, true, false};
    }

    void Freeze() {
        control_.store(kFrozenBit, std::memory_order_release);
    }

    void Resume() {
        control_.store(0, std::memory_order_release);
    }

    bool RequestSteps(std::uint32_t count = 1) {
        if (count == 0) return false;
        std::uint64_t current = control_.load(std::memory_order_acquire);
        for (;;) {
            if (!IsFrozen(current)) return false;
            const std::uint32_t pending = PendingSteps(current);
            const std::uint32_t available = kMaxPendingSteps - pending;
            const std::uint32_t add = count < available ? count : available;
            if (add == 0) return false;
            const std::uint64_t next = current +
                static_cast<std::uint64_t>(add) * kPendingUnit;
            if (control_.compare_exchange_weak(
                    current, next, std::memory_order_acq_rel,
                    std::memory_order_acquire)) return true;
        }
    }

    // A deterministic recording/restoration session must persist this phase
    // with its snapshot, or explicitly reset it at the start of a new timeline.
    void ResetTimeline() {
        fixed_remainder_.store(0, std::memory_order_release);
    }

    Mode mode() const {
        return IsFrozen(control_.load(std::memory_order_acquire))
            ? Mode::kFrozen : Mode::kPassThrough;
    }
    std::uint32_t pending_steps() const {
        return PendingSteps(control_.load(std::memory_order_acquire));
    }
    std::uint32_t fixed_remainder() const {
        return fixed_remainder_.load(std::memory_order_acquire);
    }
    bool control_consistent() const {
        const std::uint64_t control = control_.load(std::memory_order_acquire);
        return IsFrozen(control) || PendingSteps(control) == 0;
    }

private:
    std::uint64_t NextFixedToken() {
        // Exact rational 1,000,000 / 60 microseconds.  The sequence begins
        // 16666, 16667, 16667 and sums to exactly 1,000,000 every 60 steps.
        std::uint32_t remainder = fixed_remainder_.load(std::memory_order_relaxed);
        for (;;) {
            const std::uint32_t total = remainder + 1000000U;
            const std::uint64_t token = total / 60U;
            const std::uint32_t next = total % 60U;
            if (fixed_remainder_.compare_exchange_weak(
                    remainder, next, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) return token;
        }
    }

    static constexpr std::uint64_t kFrozenBit = 1;
    static constexpr std::uint64_t kPendingUnit = 2;
    static constexpr std::uint32_t kMaxPendingSteps = 1024;

    static bool IsFrozen(std::uint64_t control) {
        return (control & kFrozenBit) != 0;
    }
    static std::uint32_t PendingSteps(std::uint64_t control) {
        return static_cast<std::uint32_t>(control / kPendingUnit);
    }

    // Mode and quota share one atomic word. This prevents a concurrent
    // Resume/Freeze transition from leaving a quota visible in pass-through or
    // allowing a stale RequestSteps CAS to publish quota after Resume.
    std::atomic<std::uint64_t> control_{0};
    std::atomic<std::uint32_t> fixed_remainder_{0};
};

}  // namespace a9tas
