#include "final_writer_natural_action_runtime_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

namespace runtime = a9tas::final_writer_natural_action_runtime_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer_protocol = a9tas::final_writer_replay_v1;

struct Memory {
    alignas(64) mailbox::Mailbox box{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
    auto* memory = static_cast<Memory*>(context);
    if (!memory || !output ||
        address != reinterpret_cast<std::uintptr_t>(&memory->box) ||
        size != sizeof(memory->box))
        return false;
    std::memcpy(output, &memory->box, size);
    return true;
}

bool Write(void* context, std::uintptr_t address, const void* input,
           std::size_t size) {
    auto* memory = static_cast<Memory*>(context);
    if (!memory || !input) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(&memory->box);
    const auto end = begin + sizeof(memory->box);
    if (address < begin || address > end || size > end - address)
        return false;
    std::memcpy(reinterpret_cast<void*>(address), input, size);
    return std::memcmp(reinterpret_cast<const void*>(address), input, size) ==
           0;
}

recording::RecordingFrameV1 Frame(std::uint32_t index,
                                  std::uint32_t activations) {
    recording::RecordingFrameV1 frame{};
    frame.tick = index;
    frame.flags = recording::kRequiredFrameFlags;
    frame.nitro_activation_count = activations;
    return frame;
}

void AdvanceWriter(std::uint32_t processed, std::uint32_t total,
                   writer_protocol::Evidence* evidence) {
    evidence->wrapper_entries = processed;
    evidence->original_calls = processed;
    evidence->clean_returns = processed;
    evidence->equal_frames = processed;
    evidence->processed_frames = processed;
    evidence->last_status = processed == total
                                ? writer_protocol::kStatusComplete
                                : writer_protocol::kStatusPassive;
}

bool RunHappyPath() {
    constexpr std::uint32_t kFrames = 5;
    constexpr std::uint32_t kSession = 0x5151;
    constexpr std::uint32_t kTid = 313;
    Memory memory{};
    mailbox::Initialize(&memory.box);
    mailbox::StoreRelease(&memory.box.session_control,
                          (static_cast<std::uint64_t>(kSession) << 1) |
                              mailbox::kArmedBit);
    const a9tas::natural_action_replay_host_v1::Backend backend{
        &memory, &Read, &Write};
    const a9tas::natural_action_replay_host_v1::Layout layout{
        reinterpret_cast<std::uintptr_t>(&memory.box)};
    runtime::Runtime replay(kFrames, kSession, 0x12345000, kTid, layout);
    mailbox::RuntimeState consumer{};
    writer_protocol::Evidence writer{};
    writer.last_status = writer_protocol::kStatusPassive;
    const std::uint32_t counts[kFrames] = {0, 1, 0, 2, 0};
    for (std::uint32_t index = 0; index < kFrames; ++index) {
        if (replay.BeginAndPublish(backend, index, Frame(index, counts[index]),
                                   writer) != runtime::Result::kOk)
            return false;
        mailbox::Command command{};
        if (mailbox::ClaimAtNaturalCallback(&memory.box, &consumer, kTid,
                                             &command) !=
                mailbox::Result::kClaimed ||
            mailbox::CompleteNaturalCallback(
                &memory.box, &consumer, command.sequence, counts[index],
                true) != mailbox::Result::kCompleted)
            return false;
        AdvanceWriter(index + 1, kFrames, &writer);
        if (replay.ObserveCallbackClose(backend, index, writer) !=
                runtime::Result::kOk ||
            replay.CommitWorld(index, writer) != runtime::Result::kOk)
            return false;
    }
    return replay.complete() && replay.next_index() == kFrames;
}

bool RejectsMissingNaturalReceipt() {
    constexpr std::uint32_t kSession = 9;
    Memory memory{};
    mailbox::Initialize(&memory.box);
    mailbox::StoreRelease(&memory.box.session_control,
                          (static_cast<std::uint64_t>(kSession) << 1) |
                              mailbox::kArmedBit);
    const a9tas::natural_action_replay_host_v1::Backend backend{
        &memory, &Read, &Write};
    const a9tas::natural_action_replay_host_v1::Layout layout{
        reinterpret_cast<std::uintptr_t>(&memory.box)};
    runtime::Runtime replay(2, kSession, 0x22345000, 17, layout);
    writer_protocol::Evidence writer{};
    writer.last_status = writer_protocol::kStatusPassive;
    if (replay.BeginAndPublish(backend, 0, Frame(0, 1), writer) !=
        runtime::Result::kOk)
        return false;
    AdvanceWriter(1, 2, &writer);
    return replay.ObserveCallbackClose(backend, 0, writer) ==
               runtime::Result::kActionReceiptRejected &&
           replay.faulted();
}

}  // namespace

int main() {
    const bool happy = RunHappyPath();
    const bool missing = RejectsMissingNaturalReceipt();
    std::printf("FINAL_WRITER_NATURAL_ACTION_RUNTIME_SELFTEST passed=%u "
                "frames=5 sequence=0_1_0_2_0 exact_callback_close=1 "
                "missing_receipt_rejected=%u device_access=0\n",
                happy && missing ? 1u : 0u, missing ? 1u : 0u);
    return happy && missing ? 0 : 1;
}
