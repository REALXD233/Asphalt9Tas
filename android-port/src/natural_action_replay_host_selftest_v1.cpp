#include "natural_action_replay_host_v1.h"
#include "natural_action_replay_transport_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

namespace host = a9tas::natural_action_replay_host_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace replay = a9tas::natural_action_replay_v1;
namespace recording = a9tas::unified_tick_v1;

struct Memory {
    alignas(64) mailbox::Mailbox box{};
    std::uint32_t writes{};
    std::uint32_t offsets[3]{};
};

bool Read(void* context, std::uintptr_t address, void* output,
          std::size_t size) {
    auto* memory = static_cast<Memory*>(context);
    const auto base = reinterpret_cast<std::uintptr_t>(&memory->box);
    if (address < base || address + size < address ||
        address + size > base + sizeof(memory->box))
        return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
}

bool Write(void* context, std::uintptr_t address, const void* input,
           std::size_t size) {
    auto* memory = static_cast<Memory*>(context);
    const auto base = reinterpret_cast<std::uintptr_t>(&memory->box);
    if (address < base || address + size < address ||
        address + size > base + sizeof(memory->box) || memory->writes >= 3)
        return false;
    memory->offsets[memory->writes++] =
        static_cast<std::uint32_t>(address - base);
    std::memcpy(reinterpret_cast<void*>(address), input, size);
    return std::memcmp(reinterpret_cast<const void*>(address), input, size) == 0;
}

bool Run() {
    Memory memory{};
    mailbox::Initialize(&memory.box);
    constexpr std::uint32_t kSession = 29;
    mailbox::StoreRelease(&memory.box.session_control,
                          (static_cast<std::uint64_t>(kSession) << 1) |
                              mailbox::kArmedBit);
    recording::RecordingFrameV1 frame{};
    frame.tick = 0;
    frame.flags = recording::kRequiredFrameFlags;
    frame.nitro_activation_count = 0;
    mailbox::Command command{};
    if (!replay::BuildFrameCommand(frame, 1, kSession, 0x11220000, 77,
                                   &command))
        return false;
    const host::Backend backend{&memory, &Read, &Write};
    const host::Layout layout{reinterpret_cast<std::uintptr_t>(&memory.box)};
    mailbox::Mailbox receipt{};
    if (host::PublishFrame(backend, layout, command, &receipt) !=
            host::Result::kOk ||
        memory.writes != 3)
        return false;
    const std::uint32_t slot_offset =
        offsetof(mailbox::Mailbox, slots) + sizeof(mailbox::Command);
    return memory.offsets[0] == slot_offset &&
           memory.offsets[1] == offsetof(mailbox::Mailbox, result) &&
           memory.offsets[2] ==
               offsetof(mailbox::Mailbox, published_selector) &&
           receipt.published_selector == 3 &&
           receipt.result ==
               static_cast<std::int32_t>(mailbox::Result::kPublished);
}

}  // namespace

int main() {
    const bool passed = Run();
    std::printf("NATURAL_ACTION_REPLAY_HOST_SELFTEST passed=%u "
                "slot_then_state_then_selector=1 selector_last=1 "
                "device_access=0\n",
                passed ? 1u : 0u);
    return passed ? 0 : 1;
}
