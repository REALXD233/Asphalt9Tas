#!/usr/bin/env python3
"""Strict validator for A9AST1 authoritative steering reports."""
from __future__ import annotations
import argparse, pathlib, struct, sys

MAGIC = b"A9AST1\0\0"
BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
HEADER = struct.Struct("<8s4I20sI13Q15Q8I")
FRAME = struct.Struct("<IIiiIIqq10QHH6Q")
FLAGS = 0x1FF
FIXED = 16667
MAX_FRAMES = 36000

def require(value: bool, message: str) -> None:
    if not value: raise ValueError(message)

def validate_blob(blob: bytes, diagnostic: bool = False) -> dict[str, int]:
    require(len(blob) >= HEADER.size, "short header")
    h = HEADER.unpack_from(blob)
    require((h[0],h[1],h[2],h[3]) == (MAGIC,1,HEADER.size,FRAME.size), "header ABI")
    require((h[4] & ~FLAGS) == 0, "unknown flags")
    if not diagnostic: require(h[4] == FLAGS, "incomplete flags")
    require(h[5] == BUILD_ID and h[6] == 0, "build/reserved")
    require(all(h[i] != 0 for i in range(7,20)), "zero identity/address")
    require(h[15] == h[14] + 4, "C9C is not C98+4")
    frames = h[38]
    bound = h[39]
    require(1 <= frames <= MAX_FRAMES, "recording frame count")
    require(len(blob) >= HEADER.size and (len(blob)-HEADER.size) % FRAME.size == 0, "report alignment")
    audit_count = (len(blob)-HEADER.size)//FRAME.size
    require(audit_count == bound <= frames, "audit/bound count")
    if not diagnostic:
        require(audit_count == frames, "report length")
        require(h[21] > 0 and h[41] < h[21], "search/matched cycle")
        require((h[23],h[24],h[25]) == (0,0,0), "runtime errors")
        require((h[26],h[27],h[28]) == (frames,frames,0), "delta counters")
        require((h[29],h[30],h[31]) == (frames*2,frames*2,0), "pair counters")
    else:
        require(h[26] <= frames and h[27] <= h[26], "partial delta counters")
        require(h[29] <= frames*2 and h[30] <= h[29], "partial pair counters")
    require((h[32],h[33]) == (0,0), "forbidden action/correction")
    if not diagnostic: require(h[36] == h[35] + h[34], "thread detach accounting")
    require(h[37] == FIXED and h[42] == 0, "shape counters")
    nonzero = 0
    last_world = 0
    for index in range(audit_count):
        f = FRAME.unpack_from(blob, HEADER.size + index * FRAME.size)
        require((f[0],f[1]) == (index,index), f"tick {index}")
        require(f[2] > 0 and f[3] > 0 and f[2] != f[3], f"thread {index}")
        require(0 < f[6] <= 1_000_000 and f[7] == FIXED, f"delta {index}")
        delta,c98,c9c,prefix,f64,callback,deferred,world = f[8:16]
        require(delta < c98 < c9c == prefix < f64 < callback < deferred < world, f"event order {index}")
        require(world > last_world, f"world monotonic {index}")
        last_world = world
        require(f[16] != f[17] and (f[18] & 0xFF) == 1 and f[19] == 0, f"prefix proof {index}")
        steer = f[4]
        nonzero += steer != 0
        for base in (20,23):
            before,intended,after = f[base:base+3]
            require(intended == after, f"pair verify {index}/{base}")
            require((intended & 0xFFFFFFFF) == (before & 0xFFFFFFFF), f"brake preservation {index}/{base}")
            require((intended >> 32) == steer, f"steering bits {index}/{base}")
    require(nonzero == h[40] and h[20] >= last_world, "nonzero/event total")
    return {"frames":frames,"bound":bound,"delta_writes":h[27],"pair_writes":h[30],"search_cycles":h[21],"rejected":h[22],"nonzero":nonzero,"diagnostic":int(diagnostic)}

def selftest_blob(frames_count: int = 7) -> bytes:
    addresses = (2667,0x100000,0x200000,0x300000,0x400000,0x500000,0x600000,0x700000,0x700004,0x800000,0x900000,0xA00000,0xB00000)
    frames = bytearray()
    last_world = 0
    nonzero = 0
    for i in range(frames_count):
        steer = 0 if i < frames_count//2 else 0xBF400000
        nonzero += steer != 0
        base = i * 10 + 1
        events = (base,base+1,base+2,base+2,base+3,base+4,base+5,base+6,i,i+1)
        last_world = base+6
        low1=(i+1)&0xFFFFFFFF; low2=(~i)&0xFFFFFFFF
        p1=(steer<<32)|low1; p2=(steer<<32)|low2
        frames += FRAME.pack(i,i,101,202,steer,0x1CD,16667,16667,*events,1,0,
                             0xAAAAAAAA00000000|low1,p1,p1,
                             0x5555555500000000|low2,p2,p2)
    counters_q = (last_world+1,2,0,0,0,0,frames_count,frames_count,0,frames_count*2,frames_count*2,0,0,0,0)
    tail_i = (2,2,FIXED,frames_count,frames_count,nonzero,1,0)
    return HEADER.pack(MAGIC,1,HEADER.size,FRAME.size,FLAGS,BUILD_ID,0,*addresses,*counters_q,*tail_i)+frames

def main() -> int:
    parser=argparse.ArgumentParser(); parser.add_argument("report",nargs="?"); parser.add_argument("--selftest",action="store_true"); parser.add_argument("--diagnostic",action="store_true"); args=parser.parse_args()
    if args.selftest:
        result=validate_blob(selftest_blob()); print("A9AST1_VALIDATOR_SELFTEST passed=1",result); return 0
    if not args.report: parser.error("report is required")
    result=validate_blob(pathlib.Path(args.report).read_bytes(), diagnostic=args.diagnostic); print("A9AST1_VALID passed=1",result); return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError,ValueError,struct.error) as error:
        print(f"A9AST1_VALID passed=0 error={error}",file=sys.stderr); raise SystemExit(1)
