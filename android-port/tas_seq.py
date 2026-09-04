#!/usr/bin/env python3
"""
A9TAS replay sequence editor and converter.

Converts between human-readable TAS scripts and the inject-seq format
consumed by the payload.

Inject-seq format (one step per line):
    keycode,value,frames
    e.g. 8,1.0,0      # A press, immediate
         8,0.0,300     # A release, wait 300 frames (5s at 60Hz)

Human-readable format (.tas files):
    # Comments start with #
    # Keywords: A D S NITRO
    # Press:    A press
    # Release:  A release
    # Wait:     wait 60        (60 frames = 1 second)
    # Raw:      raw 8 1.0 0

Limitations:
    - Maximum 256 steps (payload hard limit)
    - Wait frames must be non-negative
    - Unknown key names produce an error, not a silent default

Examples:
    python tas_seq.py convert input.tas output.seq
    python tas_seq.py explain recording.seq
    python tas_seq.py validate recording.seq
    python tas_seq.py keys
"""

import sys

KEY_MAP = {
    'A':     0x08,
    'D':     0x09,
    'S':      0x06,
    'NITRO': 0x01,
    'SPACE': 0x01,
}

REVERSE_KEY_MAP = {v: k for k, v in KEY_MAP.items()}
# 0x01 has two aliases; prefer NITRO in explain output
REVERSE_KEY_MAP[0x01] = 'NITRO'

MAX_STEPS = 256
VALID_ACTIONS = {'PRESS', 'RELEASE'}


def parse_human_readable(lines):
    """Parse .tas lines into (keycode, value, frames) tuples."""
    steps = []
    pending_wait = 0

    for lineno, raw_line in enumerate(lines, 1):
        line = raw_line.strip()
        if not line or line.startswith('#'):
            continue

        parts = line.split()
        cmd = parts[0].upper()

        if cmd == 'WAIT':
            if len(parts) < 2:
                raise ValueError(f"Line {lineno}: 'wait' requires a frame count")
            try:
                frames = int(parts[1])
            except ValueError:
                raise ValueError(f"Line {lineno}: invalid wait frame count '{parts[1]}'")
            if frames < 0:
                raise ValueError(f"Line {lineno}: wait frames must be non-negative, got {frames}")
            pending_wait += frames
            continue

        if cmd == 'RAW':
            if len(parts) < 3:
                raise ValueError(f"Line {lineno}: 'raw' requires keycode and value")
            try:
                kc = int(parts[1], 0)
            except ValueError:
                raise ValueError(f"Line {lineno}: invalid keycode '{parts[1]}'")
            if kc < 0 or kc > 255:
                raise ValueError(f"Line {lineno}: keycode out of range 0-255: {kc}")
            try:
                val = float(parts[2])
            except ValueError:
                raise ValueError(f"Line {lineno}: invalid value '{parts[2]}'")
            if val != 0.0 and val != 1.0:
                raise ValueError(f"Line {lineno}: value must be 0.0 or 1.0, got {val}")
            frames = int(parts[3]) if len(parts) > 3 else pending_wait
            if frames < 0:
                raise ValueError(f"Line {lineno}: frames must be non-negative, got {frames}")
            if len(steps) >= MAX_STEPS:
                raise ValueError(f"Line {lineno}: exceeded maximum {MAX_STEPS} steps")
            steps.append((kc, val, frames))
            pending_wait = 0
            continue

        if cmd in KEY_MAP:
            kc = KEY_MAP[cmd]
            if len(parts) < 2:
                raise ValueError(f"Line {lineno}: '{cmd}' requires press/release")
            action = parts[1].upper()
            if action not in VALID_ACTIONS:
                raise ValueError(
                    f"Line {lineno}: unknown action '{parts[1]}', must be 'press' or 'release'")
            val = 1.0 if action == 'PRESS' else 0.0
            frames = pending_wait
            if len(steps) >= MAX_STEPS:
                raise ValueError(f"Line {lineno}: exceeded maximum {MAX_STEPS} steps")
            steps.append((kc, val, frames))
            pending_wait = 0
            continue

        raise ValueError(f"Line {lineno}: unknown command '{parts[0]}'")

    return steps


def format_human_readable(steps):
    """Convert (keycode, value, frames) tuples to human-readable lines."""
    lines = []
    lines.append("# A9TAS replay sequence")
    lines.append(f"# Total steps: {len(steps)}")
    total_frames = sum(s[2] for s in steps)
    lines.append(f"# Total wait frames: {total_frames} ({total_frames/60:.1f}s at 60Hz)")
    lines.append("")

    for kc, val, frames in steps:
        key_name = REVERSE_KEY_MAP.get(kc)
        if key_name is None:
            key_name = f"RAW 0x{kc:02x}"
            action = f"{val}"
            if frames > 0:
                lines.append(f"wait {frames}")
            lines.append(f"{key_name} {action} {0}")
        else:
            action = "press" if val >= 0.5 else "release"
            if frames > 0:
                lines.append(f"wait {frames}")
            lines.append(f"{key_name} {action}")

    return lines


def format_inject_seq(steps):
    """Convert (keycode, value, frames) tuples to inject-seq format."""
    lines = []
    for kc, val, frames in steps:
        lines.append(f"{kc},{val},{frames}")
    return lines


def cmd_convert(args):
    if len(args) < 2:
        print("Usage: convert <input.tas> <output.seq>", file=sys.stderr)
        return 1
    with open(args[0], 'r') as f:
        lines = f.readlines()
    try:
        steps = parse_human_readable(lines)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    out_lines = format_inject_seq(steps)
    with open(args[1], 'w') as f:
        f.write('\n'.join(out_lines) + '\n')
    print(f"Converted {len(steps)} steps -> {args[1]}")
    return 0


def parse_inject_seq(lines):
    """Parse inject-seq format lines into (keycode, value, frames) tuples.

    Raises ValueError on malformed lines.  Used by explain and validate.
    """
    steps = []
    for lineno, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(',')
        if len(parts) < 3:
            raise ValueError(f"Line {lineno}: malformed (need keycode,value,frames): {line}")
        try:
            kc = int(parts[0])
        except ValueError:
            raise ValueError(f"Line {lineno}: invalid keycode '{parts[0]}'")
        if kc < 0 or kc > 255:
            raise ValueError(f"Line {lineno}: keycode out of range 0-255: {kc}")
        try:
            val = float(parts[1])
        except ValueError:
            raise ValueError(f"Line {lineno}: invalid value '{parts[1]}'")
        if val != 0.0 and val != 1.0:
            raise ValueError(f"Line {lineno}: value must be 0.0 or 1.0, got {val}")
        try:
            frames = int(parts[2])
        except ValueError:
            raise ValueError(f"Line {lineno}: invalid frames '{parts[2]}'")
        if frames < 0:
            raise ValueError(f"Line {lineno}: frames must be non-negative, got {frames}")
        if len(steps) >= MAX_STEPS:
            raise ValueError(f"Line {lineno}: exceeded maximum {MAX_STEPS} steps")
        steps.append((kc, val, frames))
    return steps


def cmd_explain(args):
    if len(args) < 1:
        print("Usage: explain <input.seq>", file=sys.stderr)
        return 1
    with open(args[0], 'r') as f:
        lines = f.readlines()
    try:
        steps = parse_inject_seq(lines)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    out_lines = format_human_readable(steps)
    print('\n'.join(out_lines))
    return 0


def cmd_validate(args):
    if len(args) < 1:
        print("Usage: validate <input.seq>", file=sys.stderr)
        return 1
    with open(args[0], 'r') as f:
        lines = f.readlines()
    try:
        steps = parse_inject_seq(lines)
    except ValueError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    total_frames = sum(s[2] for s in steps)
    print(f"OK: {len(steps)} steps, {total_frames} wait frames ({total_frames/60:.1f}s at 60Hz)")
    return 0


def cmd_keys(args):
    print("Key mapping:")
    for name, code in sorted(KEY_MAP.items()):
        print(f"  {name:8s} = 0x{code:02x} ({code})")
    print()
    print("Value semantics: press=1.0, release=0.0")
    print("Frame rate: 60 frames = 1 second")
    print(f"Maximum steps: {MAX_STEPS}")
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    cmd = sys.argv[1].lower()
    args = sys.argv[2:]

    if cmd == 'convert':
        return cmd_convert(args)
    elif cmd == 'explain':
        return cmd_explain(args)
    elif cmd == 'validate':
        return cmd_validate(args)
    elif cmd == 'keys':
        return cmd_keys(args)
    else:
        print(f"Unknown command: {cmd}", file=sys.stderr)
        print(__doc__)
        return 1


if __name__ == '__main__':
    sys.exit(main())
