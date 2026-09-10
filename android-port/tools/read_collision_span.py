"""Bounded diagnostic reads through an O_RDONLY helper; never writes the game."""
import subprocess
import struct
import sys

ADB = 'D:/AsphaltTAS/android-port/.toolchains/android-sdk/platform-tools/adb.exe'

def read(pid, address, length):
    result = subprocess.run([ADB, '-s', 'emulator-5554', 'shell', 'su', '0',
        '/data/local/tmp/a9collision_span_20260909', str(pid), f'{address:x}', str(length)],
        capture_output=True, text=True, timeout=15, check=True)
    value = bytes.fromhex(result.stdout.strip())
    if len(value) != length:
        raise ValueError('short read')
    return value

if __name__ == '__main__':
    data = read(int(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3]))
    for offset in range(0, len(data)-15, 16):
        print(f'{offset:03x}', ' '.join(f'{v:016x}' for v in struct.unpack_from('<2Q', data, offset)),
              struct.unpack_from('<4f', data, offset))
