# IDAPython read-only query for DeepSeek V4 Pro override-architecture evidence.
# Runs under idat -A; writes a text report, never calls save_database().
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_override_targets_20260815.txt"

f = open(OUT, "w", encoding="utf-8")


def w(s=""):
    f.write(s + "\n")


def fn_name(ea):
    n = ida_name.get_name(ea)
    if n:
        return n
    fn = ida_funcs.get_func(ea)
    if fn:
        n = ida_name.get_name(fn.start_ea)
        if n:
            return n
    return "sub_%X" % ea


def dump_func(ea, max_insns=250):
    w("=" * 100)
    w("FUNC_AT %X name=%s" % (ea, fn_name(ea)))
    fn = ida_funcs.get_func(ea)
    start = fn.start_ea if fn else ea
    end = fn.end_ea if fn else ea + 0x400
    w("RANGE %X..%X size=%X" % (start, end, end - start))
    count = 0
    for head in idautils.Heads(start, end):
        if count >= max_insns:
            w("... truncated at %d instructions" % max_insns)
            break
        w("%08X  %s" % (head, idc.generate_disasm_line(head, 0)))
        count += 1
    w("")


def dump_xrefs_to(ea, max_refs=40):
    w("-" * 80)
    w("XREFS_TO %X (%s)" % (ea, fn_name(ea)))
    count = 0
    for xref in idautils.CodeRefsTo(ea, 0):
        if count >= max_refs:
            w("... truncated")
            break
        fn = ida_funcs.get_func(xref)
        w("  %X in %s" % (xref, fn_name(xref) if fn else "(no func)"))
        count += 1
    if count == 0:
        w("  <no code xrefs>")
    w("")


def dump_names(eas):
    w("=" * 100)
    w("NAMES")
    for ea in eas:
        w("%X -> %s (func=%s)" % (ea, ida_name.get_name(ea) or "<none>",
                                   fn_name(ea)))
    w("")


def dump_calls_in_func(ea, max_calls=120):
    w("=" * 100)
    w("CALLS_IN %X (%s)" % (ea, fn_name(ea)))
    fn = ida_funcs.get_func(ea)
    if fn is None:
        w("  no function")
        w("")
        return
    count = 0
    for head in idautils.Heads(fn.start_ea, fn.end_ea):
        if idc.print_insn_mnem(head) in ("BL", "BLR"):
            count += 1
            if count > max_calls:
                w("... truncated")
                break
            target = idc.get_operand_value(head, 0)
            w("  %X %s -> %X (%s)" % (head, idc.generate_disasm_line(head, 0),
                                       target, fn_name(target)))
    w("")


# Key addresses from HANDOFF sections 4, 7 and current evidence.
addrs = [
    0x385A0E8, 0x386A854, 0x386A464, 0x386B1E0, 0x386B380,
    0x386B4C8, 0x386B500, 0x386B6C4, 0x38F2340,
    0x385A7AC, 0x385A7D0, 0x385A7EC, 0x367B1F0, 0x367B21C,
    0x367B3E4, 0x367B548, 0x3A519D8,
    0x3687BE0, 0x3687BC8, 0x386B748, 0x386B798, 0x386B7E4,
    0x386B858, 0x386B86C, 0x386B884, 0x385A574,
]

dump_names(addrs)

for ea in (0x385A7AC, 0x385A7D0, 0x385A7EC):
    dump_func(ea, 80)
for ea in (0x367B1F0, 0x367B21C, 0x367B3E4, 0x367B548):
    dump_func(ea, 120)
    dump_xrefs_to(ea)

dump_func(0x386B1E0, 350)
dump_calls_in_func(0x386B1E0)

dump_func(0x3687BE0, 160)
dump_xrefs_to(0x3687BE0)
dump_func(0x3687BC8, 160)
dump_xrefs_to(0x3687BC8)

dump_func(0x385A574, 220)

f.close()
print("WROTE " + OUT)
idc.qexit(0)
