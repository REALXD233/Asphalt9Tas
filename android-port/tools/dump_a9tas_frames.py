#!/usr/bin/env python3
"""Read-only A9TAS1 frame export, using the product's existing strict decoder."""
import argparse
import dataclasses
import hashlib
import json
import struct
from pathlib import Path

from a9tas_recording_v1 import (
    read_archive, A9G4R2_HEADER, A9G4R2_FRAME, A9G4R2_INTERVAL,
)

SKIPS = ('steering', 'brake', 'nitro_activation', 'accelerator',
         'barrel_angular', 'barrel_rbx', 'respawn', 'transform_linear')
NOTES = [
    'Tick 从 0 开始，是逻辑帧，不等于屏幕显示帧。',
    'logical_time_seconds 来自存档逻辑时间，不是实测墙钟时间或冲线成绩。',
    'nitro_activation_count 是本 Tick 的氮气激活调用次数，不表示黄蓝紫红状态或剩余氮气量。',
    'brake 是原始控制输入，不能单凭该字段断言车辆处于持续漂移状态。',
    'barrel_angular 与 barrel_rbx 为存档中的特技字段；不据此编造特技分类或计数。',
    'transform_float32 为 64 字节变换区按内存顺序解码的 16 个浮点数；未假定矩阵行列、坐标轴或单位。',
    'linear_velocity_float32 为三个原始线速度分量；未换算成游戏仪表 km/h。',
    'skip_override_fields 标识回放不覆盖的字段，例如 accelerator/respawn 的零值不表示游戏不加速或未重生。',
    'physics_intervals 保留每个 Tick 的所有 getter 调用，含 ordinal；不是强制物理步长。',
    'raw_frame_hex 和 float32_bits_le 保留原始位模式，包括负零；不向存档写入任何内容。',
    '本格式不包含完整 HUD、氮气颜色/存量、摄像机、全部游戏对象状态；不能从不存在的数据恢复这些字段。',
]


def decode_frames(archive):
    data = archive.recording
    intervals = [[] for _ in range(archive.source.frame_count)]
    offset = A9G4R2_HEADER.size + archive.source.frame_count * A9G4R2_FRAME.size
    for i in range(archive.source.interval_count):
        tick, ordinal, bits = A9G4R2_INTERVAL.unpack_from(data, offset + i * 16)
        intervals[tick].append(dict(ordinal=ordinal, output_bits=f'0x{bits:08x}',
            seconds=struct.unpack('<f', struct.pack('<I', bits))[0]))
    for i in range(archive.source.frame_count):
        offset = A9G4R2_HEADER.size + i * A9G4R2_FRAME.size
        raw = data[offset:offset + A9G4R2_FRAME.size]
        (tick, ns, steering, brake, accelerator, nitro, skip, respawn, padding,
         ax, ay, az, rbx0, rbx1, transform, linear, flags, reserved) = A9G4R2_FRAME.unpack(raw)
        # Hex strings avoid precision loss in consumers that parse numbers as doubles.
        yield dict(tick=tick, logical_time_ns=str(ns), logical_time_seconds=ns / 1e9,
            steering=steering, brake=brake, accelerator=accelerator,
            nitro_activation_count=nitro, respawn_button_press=respawn,
            skip_override_flags=f'0x{skip:02x}',
            skip_override_fields=[name for bit, name in enumerate(SKIPS) if skip & (1 << bit)],
            barrel_angular=[ax, ay, az], barrel_rbx=[rbx0, rbx1],
            transform_float32=list(struct.unpack('<16f', transform)),
            linear_velocity_float32=list(struct.unpack('<3f', linear)),
            frame_flags=f'0x{flags:x}', reserved=reserved, padding_hex=padding.hex(),
            float32_bits_le={
                'controls': [f'0x{x:08x}' for x in struct.unpack_from('<3I', raw, 16)],
                'barrel': [f'0x{x:08x}' for x in struct.unpack_from('<5I', raw, 40)],
                'transform': [f'0x{x:08x}' for x in struct.unpack('<16I', transform)],
                'linear_velocity': [f'0x{x:08x}' for x in struct.unpack('<3I', linear)]},
            physics_intervals=intervals[tick], raw_frame_hex=raw.hex())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--out', type=Path, required=True, help='new output directory')
    args = parser.parse_args()
    archive = read_archive(args.archive)
    # Do not overwrite earlier exports or the input.
    args.out.mkdir(parents=True, exist_ok=False)
    source = dataclasses.asdict(archive.source)
    source['session_id'] = str(source['session_id'])
    frames = list(decode_frames(archive))
    document = dict(schema='A9TAS_FRAME_DUMP_V1',
        input_sha256=hashlib.sha256(args.archive.read_bytes()).hexdigest(),
        notes=NOTES, source=source, manifest=archive.manifest, frames=frames)
    with (args.out / 'frames.json').open('x', encoding='utf-8') as out:
        json.dump(document, out, ensure_ascii=False, indent=2, allow_nan=False)
    with (args.out / '逐帧数据.txt').open('x', encoding='utf-8') as out:
        out.write('A9TAS 逐帧解析（完整字段及原始位模式见 frames.json）\n')
        out.write('\n'.join(NOTES) + '\n\n')
        out.write(json.dumps(source, ensure_ascii=False) + '\n\n')
        for frame in frames:
            out.write(f"Tick {frame['tick']} | 逻辑时间 {frame['logical_time_seconds']:.6f}s | "
                f"转向 {frame['steering']:.9g} | 刹车 {frame['brake']:.9g} | "
                f"氮气调用 {frame['nitro_activation_count']}\n")
            for key in ('barrel_angular', 'barrel_rbx', 'transform_float32',
                        'linear_velocity_float32', 'physics_intervals', 'skip_override_fields'):
                out.write(f'  {key}: {frame[key]}\n')
    print(json.dumps(dict(output=str(args.out.resolve()), **source), ensure_ascii=False))


if __name__ == '__main__':
    main()
