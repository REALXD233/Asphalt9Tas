# IDAPython read-only query: contexts of extra writes to final C98/C9C.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_final_offset_writers_context_20260816.txt"
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


def dump_range(start, end, label):
    w("=" * 100)
    w("RANGE %X..%X %s" % (start, end, label))
    for head in idautils.Heads(start, end):
        w("%08X  %s" % (head, idc.generate_disasm_line(head, 0)))
    w("")


def dump_xrefs_to(ea):
    w("-" * 80)
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
    for x in idautils.DataRefsTo(ea):
        if x in seen:
            continue
        seen.add(x)
        w("  data %X" % x)
    w("")


dump_range(0x3691C80, 0x3691D20, "C98 zero writer A")
dump_range(0x3692770, 0x3692810, "C98 zero writer B")
dump_range(0x36A6700, 0x36A67C0, "C9C extra writer context")
dump_range(0x36934A0, 0x3693510, "direct C98/C9C setters context")

for ea in (0x3691CC8, 0x36927B0, 0x36A6768, 0x36934B0, 0x36934D8):
    dump_xrefs_to(ea)

# Try to find function boundaries containing the extra writer.
for ea in (0x36A6768, 0x3691CC8, 0x36927B0):
    fn = ida_funcs.get_func(ea)
    if fn:
        w("FUNC_CONTAINING %X: %X..%X %s" %
          (ea, fn.start_ea, fn.end_ea,
           ida_name.get_name(fn.start_ea) or "<none>"))
    else:
        prev = ida_funcs.get_prev_func(ea)
        nxt = ida_funcs.get_next_func(ea)
        w("NO_FUNC_CONTAINING %X prev=%s next=%s" %
          (ea, ("%X..%X" % (prev.start_ea, prev.end_ea)) if prev else "none",
           ("%X..%X" % (nxt.start_ea, nxt.end_ea)) if nxt else "none"))

f.close()
print("WROTE " + OUT)
idc.qexit(0)
