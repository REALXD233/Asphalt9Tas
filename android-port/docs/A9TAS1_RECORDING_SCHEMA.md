# A9TAS1 canonical recording container

Status: G9 minimum product schema v1. The live runtime remains A9G4R2; A9TAS1
wraps it without changing a single source byte.

## Goals

- one file suitable for Android app-private storage and SAF import/export;
- exact game build/Profile, map, car, control mode and target tick metadata;
- deterministic canonical serialization and SHA-256 integrity;
- byte-exact A9G4R2 extraction, preserving every raw float bit;
- bounded lengths and strict rejection of unknown or inconsistent fields.

Structural validity is not a live replay pass. Playback must still select a
matching BuildProfile and use the G4-G8 runtime receipts.

## Binary layout

All integers are little-endian. The fixed header is 160 bytes, followed by one
canonical UTF-8 JSON manifest and one complete A9G4R2 recording.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic `A9TAS1\0\0` |
| 8 | 4 | version `1` |
| 12 | 4 | header size `160` |
| 16 | 4 | flags `0x7` |
| 20 | 4 | manifest byte length, maximum 64 KiB |
| 24 | 8 | embedded recording byte length, maximum 512 MiB |
| 32 | 4 | frame count |
| 36 | 4 | Interval count |
| 40 | 4 | fixed delta in microseconds |
| 44 | 4 | embedded A9G4R2 version |
| 48 | 8 | source session id |
| 56 | 4 | source generation |
| 60 | 4 | inclusive target tick |
| 64 | 32 | SHA-256 of exact manifest bytes |
| 96 | 32 | SHA-256 of exact A9G4R2 bytes |
| 128 | 32 | zero reserved bytes |

The total file size must equal exactly `160 + manifest_size + recording_size`.

## Canonical manifest

JSON is UTF-8, with sorted keys, no insignificant whitespace, and no trailing
newline. Required top-level keys are:

- `schema`: exactly `a9tas.recording.v1`;
- `recording_id`: canonical UUID;
- `title`, `created_utc`, `notes`;
- `game`: package, version, native SHA-256, GNU Build ID, BuildProfile SHA-256;
- `race`: map, car, `manual|touchdrive|unknown` control mode;
- `recording`: exact A9G4R2 identity and inclusive target tick.

Header, manifest and embedded A9G4R2 identity fields must agree. Unknown keys,
wrong JSON types, noncanonical JSON, missing/duplicate ticks, non-finite values,
hash mismatch, trailing bytes and target ticks outside the recording all fail.

## Library operations

`tools/a9tas_library_v1.py` provides atomic `pack`, `inspect`, `rename`,
`set-target`, `unpack`, `list`, and explicit `delete --yes`. Rename and target
changes rewrite only the canonical manifest/header; the embedded recording SHA
and bytes stay unchanged. Library listing quarantines invalid `.a9tas` files in
an `invalid` result instead of making valid recordings disappear.

Upstream `.REPLAY` JSON v1-v3 import/export remains a separate compatibility
adapter. It must never reduce the precision of this canonical Android format.
