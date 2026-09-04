#!/usr/bin/env python3
"""Policy for external natural-action mailbox publication."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "natural_action_replay_host_v1.h"
MAILBOX = ROOT / "src" / "natural_action_callback_mailbox_v1.h"
CONTROLLER = ROOT / "src" / "natural_action_lifecycle_controller_v1.cpp"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def ordered(text: str, *tokens: str) -> bool:
    positions = [text.find(token) for token in tokens]
    return all(position >= 0 for position in positions) and positions == sorted(positions)


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    mailbox_text = MAILBOX.read_text(encoding="utf-8")
    controller = CONTROLLER.read_text(encoding="utf-8")
    for token in (
        "ReadSnapshot",
        "PublishFrame",
        "claimed != completed",
        "command.sequence != completed + 1",
        "kWriteSlotFailed",
        "kWriteStateFailed",
        "kPublishSelectorFailed",
        "kPublicationVerificationFailed",
    ):
        require(token in text, f"host publication invariant missing: {token}")
    require(ordered(text, "kWriteSlotFailed", "kWriteStateFailed",
                    "kPublishSelectorFailed"),
            "host selector must be written last")
    publish_body = mailbox_text.split("inline Result Publish(", 1)[1].split(
        "inline Result ClaimAtNaturalCallback", 1
    )[0]
    require(publish_body.find("&mailbox->result") <
            publish_body.find("&mailbox->published_selector"),
            "in-process selector must be the final publication edge")
    controller_body = controller.split("bool PublishZeroCall", 1)[1].split(
        "bool RequestCleanRemoval", 1
    )[0]
    require(controller_body.find("offsetof(natural_mailbox::Mailbox, result)") <
            controller_body.find(
                "offsetof(natural_mailbox::Mailbox, published_selector)"
            ), "legacy Gate controller selector must be written last")
    for forbidden in ("NitroService", "kYellow", "kPerfectNitro", "adb"):
        require(forbidden.lower() not in text.lower(),
                f"host transport must not derive Nitro state: {forbidden}")
    if len(sys.argv) == 2:
        require(pathlib.Path(sys.argv[1]).is_file(),
                "compiled host transport selftest object missing")
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [selftest]")
    print("NATURAL_ACTION_REPLAY_HOST_POLICY passed=1 selector_last=1 "
          "completion_overwrite_race=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
