#include "physics_token_gate.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
}  // namespace

int main() {
    a9tas::PhysicsTokenGate gate;

    auto d = gate.Filter(17321);
    Check(d.token_us == 17321 && !d.substituted && !d.released_step,
          "default_is_full_passthrough");
    Check(!gate.RequestSteps(), "step_rejected_while_passthrough");

    gate.Freeze();
    d = gate.Filter(18000);
    Check(d.token_us == 0 && d.substituted && !d.released_step,
          "frozen_consumes_as_zero_token");
    for (int i = 0; i < 1000; ++i) {
        d = gate.Filter(100000);
        if (d.token_us != 0 || d.released_step) ++failures;
    }
    Check(gate.pending_steps() == 0, "indefinite_freeze_has_no_catchup_queue");

    Check(gate.RequestSteps(1), "single_step_request_accepted");
    d = gate.Filter(99999);
    Check(d.token_us == 16666 && d.substituted && d.released_step,
          "first_request_releases_one_fixed_token");
    d = gate.Filter(99999);
    Check(d.token_us == 0 && !d.released_step,
          "single_step_immediately_refreezes");

    gate.ResetTimeline();
    Check(gate.RequestSteps(60), "sixty_step_batch_accepted");
    std::uint64_t sum = 0;
    int released = 0;
    for (int i = 0; i < 60; ++i) {
        d = gate.Filter(1);
        sum += d.token_us;
        released += d.released_step ? 1 : 0;
    }
    Check(sum == 1000000 && released == 60 && gate.fixed_remainder() == 0,
          "sixty_steps_equal_exactly_one_second");
    Check(gate.Filter(50000).token_us == 0,
          "batch_exhaustion_returns_to_freeze");

    gate.Resume();
    d = gate.Filter(19234);
    Check(d.token_us == 19234 && !d.substituted,
          "resume_restores_unmodified_real_token");

    gate.Freeze();
    gate.ResetTimeline();
    constexpr int kThreads = 8;
    constexpr int kRequestsPerThread = 50;
    std::thread requesters[kThreads];
    for (auto& requester : requesters) {
        requester = std::thread([&gate] {
            for (int i = 0; i < kRequestsPerThread; ++i)
                gate.RequestSteps();
        });
    }
    for (auto& requester : requesters) requester.join();
    int concurrent_releases = 0;
    std::uint64_t concurrent_sum = 0;
    for (int i = 0; i < kThreads * kRequestsPerThread + 10; ++i) {
        d = gate.Filter(77777);
        if (d.released_step) {
            ++concurrent_releases;
            concurrent_sum += d.token_us;
        }
    }
    Check(concurrent_releases == kThreads * kRequestsPerThread,
          "concurrent_requests_are_not_lost");
    Check(concurrent_sum == 6666666,
          "four_hundred_steps_use_exact_rational_time");

    // Race control transitions against requests. The combined control word
    // must never expose pending quota while pass-through is visible.
    std::atomic<bool> stop_race{false};
    std::atomic<bool> invalid_control{false};
    std::thread controller([&] {
        for (int i = 0; i < 100000; ++i) {
            gate.Freeze();
            gate.Resume();
        }
        stop_race.store(true, std::memory_order_release);
    });
    std::thread racer([&] {
        while (!stop_race.load(std::memory_order_acquire)) {
            gate.RequestSteps();
            if (!gate.control_consistent())
                invalid_control.store(true, std::memory_order_release);
        }
    });
    controller.join();
    racer.join();
    gate.Resume();
    Check(!invalid_control.load(std::memory_order_acquire) &&
              gate.pending_steps() == 0,
          "resume_request_race_leaves_no_passthrough_quota");

    // Mirror the proven inner-world loop: add float dt, then subtract a fixed
    // 1/60 quantum while the accumulator is positive.  A normal completed
    // frame leaves its remainder in (-1/60, 0].  Verify that the rational token
    // sequence yields exactly one substep for every request throughout that
    // entire practical remainder range.
    constexpr float starts[] = {-0.016f, -0.010f, -0.001f, -0.000001f, 0.0f};
    bool model_exact = true;
    for (float start : starts) {
        gate.Freeze();
        gate.ResetTimeline();
        gate.RequestSteps(600);
        float accumulator = start;
        for (int i = 0; i < 600; ++i) {
            d = gate.Filter(500000);
            accumulator += static_cast<float>(d.token_us) / 1000000.0f;
            int substeps = 0;
            while (accumulator > 0.0f && substeps < 4) {
                accumulator -= 1.0f / 60.0f;
                ++substeps;
            }
            if (substeps != 1 || !(accumulator > -(1.0f / 60.0f) &&
                                   accumulator <= 0.0f)) {
                model_exact = false;
                break;
            }
        }
    }
    Check(model_exact,
          "rational_tokens_produce_exactly_one_world_substep_from_valid_remainders");

    std::printf("SUMMARY failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
