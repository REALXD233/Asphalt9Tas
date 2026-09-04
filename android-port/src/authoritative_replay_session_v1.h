#pragma once

#include "authoritative_tick_state_machine_v1.h"
#include "unified_tick_recording_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace a9tas::authoritative_session_v1 {

inline constexpr std::size_t kReplayQueueCapacity = 1000;

enum class QueueReplayOutcome : std::uint32_t {
    kAccepted = 0,
    kInvalidReplay = 1,
    kUnfinishedInRaceReplay = 2,
};

struct BeginTickResultV1 {
    authoritative_tick_v1::SelectionResultV1 selection{};
    unified_tick_v1::RecordingFrameV1 packet{};
};

// Source-faithful, transport-free owner of the AluTasV2 replay control plane
// and DLL-side tick selector. The caller keeps `frames` alive for the complete
// queued-session lifetime. This core performs no process access or game write.
class ReplaySessionV1 {
  public:
    QueueReplayOutcome QueueReplay(
        const unified_tick_v1::RecordingFrameV1* frames,
        std::size_t frame_count,
        std::uint32_t fixed_interval_us,
        std::uint32_t target_tick,
        bool in_race,
        std::uint32_t current_race_tick) noexcept;

    bool ChangeTargetTick(std::uint32_t target_tick) noexcept;
    void ClearQueuedReplay() noexcept;
    void OnRaceEnded() noexcept;

    // Mirrors tool-side OnUpdate: completion is tested before refill.
    void OnUpdate(std::uint32_t current_race_tick) noexcept;

    // Mirrors DLL-side OnNewTick. kBlockedEmpty means the adapter must pump
    // commands/refill and retry while block mode remains active.
    BeginTickResultV1 BeginTick(
        std::uint32_t current_race_tick,
        bool in_race,
        bool communication_version_matches,
        bool block_mode_still_active) noexcept;

    bool EndTick(authoritative_tick_v1::TickInputStateV1* current,
                 bool in_race,
                 authoritative_tick_v1::TickInputStateV1* published) noexcept;

    [[nodiscard]] bool has_replay() const noexcept { return has_replay_; }
    [[nodiscard]] authoritative_tick_v1::ReplayMode replay_mode() const noexcept {
        return replay_mode_;
    }
    [[nodiscard]] std::uint32_t queued_fixed_interval_us() const noexcept {
        return queued_fixed_interval_us_;
    }
    [[nodiscard]] std::uint32_t installed_fixed_interval_us() const noexcept {
        return installed_fixed_interval_us_;
    }
    [[nodiscard]] std::uint32_t final_tick() const noexcept {
        return final_tick_;
    }
    [[nodiscard]] std::size_t playback_index() const noexcept {
        return playback_index_;
    }
    [[nodiscard]] std::size_t queued_packets() const noexcept {
        return queue_size_;
    }
    [[nodiscard]] bool has_selected_packet() const noexcept {
        return has_selected_packet_;
    }
    [[nodiscard]] const unified_tick_v1::RecordingFrameV1*
    selected_packet() const noexcept {
        return has_selected_packet_ ? &selected_packet_ : nullptr;
    }
    bool QueueTickAt(std::size_t index, std::uint64_t* tick) const noexcept;

  private:
    void InitializeQueuedReplay() noexcept;
    void ClearInputQueue() noexcept;
    void ConsumeQueuePrefix(std::size_t count) noexcept;
    bool ReplayShapeValid(
        const unified_tick_v1::RecordingFrameV1* frames,
        std::size_t frame_count,
        std::uint32_t fixed_interval_us) const noexcept;

    const unified_tick_v1::RecordingFrameV1* frames_ = nullptr;
    std::size_t frame_count_ = 0;
    std::uint32_t final_tick_ = 0;
    std::size_t playback_index_ = 0;
    bool has_replay_ = false;
    authoritative_tick_v1::ReplayMode replay_mode_ =
        authoritative_tick_v1::ReplayMode::kInactive;
    std::uint32_t queued_fixed_interval_us_ = 0;
    std::uint32_t installed_fixed_interval_us_ = 0;

    std::array<unified_tick_v1::RecordingFrameV1, kReplayQueueCapacity>
        queue_{};
    std::size_t queue_size_ = 0;

    bool has_selected_packet_ = false;
    unified_tick_v1::RecordingFrameV1 selected_packet_{};
};

}  // namespace a9tas::authoritative_session_v1
