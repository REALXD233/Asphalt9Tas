# IDAPython read-only query: callers of the extra C9C writer sub_36A6758
# and the direct C98/C9C setters.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_c9c_extra_writer_callers_20260816.txt"
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
    return "(no func)"


def dump_xrefs_to(ea):
    w("=" * 100)
    w("XREFS_TO %X (%s)" % (ea, fn_name(ea)))
    seen = set()
    for x in idautils.CodeRefsTo(ea, 0):
        if x in seen:
            continue
        seen.add(x)
        fn = ida_funcs.get_func(x)
        if fn:
            w("  code %X in %X..%X %s" %
              (x, fn.start_ea, fn.end_ea,
               ida_name.get_name(fn.start_ea) or "<none>"))
        else:
            w("  code %X (no func)" % x)
        # Print 4 instructions around the call site.
        w("    context:")
        for h in idautils.Heads(x - 0x10, x + 0x10):
            w("      %08X  %s" % (h, idc.generate_disasm_line(h, 0)))
    for x in idautils.DataRefsTo(ea):
        if x in seen:
            continue
        seen.add(x)
        w("  data %X" % x)
    w("")


for ea in (0x36A6758, 0x36934AC, 0x36934D4, 0x36A7658):
    dump_xrefs_to(ea)

# Is there any caller that runs from the two zero-writer paths?
w("ZERO-WRITER PATH CALLERS AROUND 3691CBC and 36927A4")
for start in (0x3691CA0, 0x3692788):
    for h in idautils.Heads(start, start + 0x60):
        w("%08X  %s" % (h, idc.generate_disasm_line(h, 0)))
    w("")

f.close()
print("WROTE " + OUT)
idc.qexit(0)
