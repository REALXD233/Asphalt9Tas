#pragma once

// External-memory transport for the persistent natural-action mailbox.
// The caller supplies audited read and verified-write functions.  This layer
// performs no process discovery, attachment, game-state write, or action call.

#include "natural_action_callback_mailbox_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::natural_action_replay_host_v1 {

namespace mailbox = a9tas::natural_action_callback_v1;

using ReadFn = bool (*)(void*, std::uintptr_t, void*, std::size_t);
using WriteVerifiedFn = bool (*)(void*, std::uintptr_t, const void*,
                                std::size_t);

struct Backend {
    void* context{};
    ReadFn read{};
    WriteVerifiedFn write_verified{};
};

struct Layout {
    std::uintptr_t mailbox_address{};
};

enum class Result : std::int32_t {
    kOk = 0,
    kInvalidArgument = -1,
    kReadFailed = -2,
    kMailboxInvalid = -3,
    kSessionMismatch = -4,
    kPreviousFramePending = -5,
    kSequenceMismatch = -6,
    kMailboxFault = -7,
    kWriteSlotFailed = -8,
    kWriteStateFailed = -9,
    kPublishSelectorFailed = -10,
    kPublicationVerificationFailed = -11,
};

inline bool Valid(const Backend& backend, const Layout& layout) {
    return backend.context != nullptr && backend.read != nullptr &&
           backend.write_verified != nullptr && layout.mailbox_address != 0 &&
           (layout.mailbox_address & (alignof(mailbox::Mailbox) - 1u)) == 0;
}

inline Result ReadSnapshot(const Backend& backend, const Layout& layout,
                           mailbox::Mailbox* output) {
    if (!Valid(backend, layout) || output == nullptr)
        return Result::kInvalidArgument;
    mailbox::Mailbox snapshot{};
    if (!backend.read(backend.context, layout.mailbox_address, &snapshot,
                      sizeof(snapshot)))
        return Result::kReadFailed;
    if (!mailbox::MailboxValid(snapshot)) return Result::kMailboxInvalid;
    *output = snapshot;
    return Result::kOk;
}

inline Result PublishFrame(const Backend& backend, const Layout& layout,
                           const mailbox::Command& command,
                           mailbox::Mailbox* receipt) {
    if (!Valid(backend, layout) || !mailbox::CommandValid(command))
        return Result::kInvalidArgument;
    mailbox::Mailbox before{};
    Result read = ReadSnapshot(backend, layout, &before);
    if (read != Result::kOk) return read;
    const std::uint64_t session = mailbox::LoadAcquire(
        &before.session_control);
    if (!mailbox::Armed(session) ||
        mailbox::SessionId(session) != command.session_id)
        return Result::kSessionMismatch;
    const std::uint64_t claimed = mailbox::LoadAcquire(
        &before.claimed_sequence);
    const std::uint64_t completed = mailbox::LoadAcquire(
        &before.completed_sequence);
    if (claimed != completed) return Result::kPreviousFramePending;
    if (command.sequence != completed + 1 ||
        command.replay_frame != completed)
        return Result::kSequenceMismatch;
    if (__atomic_load_n(&before.result, __ATOMIC_ACQUIRE) < 0)
        return Result::kMailboxFault;

    const std::uint32_t slot = static_cast<std::uint32_t>(command.sequence & 1u);
    const std::uintptr_t slot_address =
        layout.mailbox_address + offsetof(mailbox::Mailbox, slots) +
        static_cast<std::uintptr_t>(slot) * sizeof(mailbox::Command);
    const std::int32_t published =
        static_cast<std::int32_t>(mailbox::Result::kPublished);
    const std::uint64_t selector = (command.sequence << 1) | slot;
    if (!backend.write_verified(backend.context, slot_address, &command,
                                sizeof(command)))
        return Result::kWriteSlotFailed;
    // State is prepared before selector publication.  The selector is written
    // last, preventing a fast consumer from having its completion overwritten.
    if (!backend.write_verified(
            backend.context,
            layout.mailbox_address + offsetof(mailbox::Mailbox, result),
            &published, sizeof(published)))
        return Result::kWriteStateFailed;
    if (!backend.write_verified(
            backend.context,
            layout.mailbox_address +
                offsetof(mailbox::Mailbox, published_selector),
            &selector, sizeof(selector)))
        return Result::kPublishSelectorFailed;

    mailbox::Mailbox after{};
    read = ReadSnapshot(backend, layout, &after);
    if (read != Result::kOk) return read;
    const std::uint64_t observed_selector = mailbox::LoadAcquire(
        &after.published_selector);
    if (observed_selector != selector ||
        std::memcmp(&after.slots[slot], &command, sizeof(command)) != 0)
        return Result::kPublicationVerificationFailed;
    if (receipt != nullptr) *receipt = after;
    return Result::kOk;
}

}  // namespace a9tas::natural_action_replay_host_v1
