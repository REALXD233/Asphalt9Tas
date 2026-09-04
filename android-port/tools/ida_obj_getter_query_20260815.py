# IDAPython read-only query: resolve object getter sub_36AA578 used by the
# downstream dispatch wrappers.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_obj_getter_36AA578_20260815.txt"
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


def dump_func(ea, max_insns=200):
    w("=" * 100)
    w("FUNC_AT %X name=%s" % (ea, fn_name(ea)))
    fn = ida_funcs.get_func(ea)
    start = fn.start_ea if fn else ea
    end = fn.end_ea if fn else ea + 0x300
    w("RANGE %X..%X size=%X" % (start, end, end - start))
    count = 0
    for head in idautils.Heads(start, end):
        if count >= max_insns:
            w("... truncated")
            break
        w("%08X  %s" % (head, idc.generate_disasm_line(head, 0)))
        count += 1
    w("")


def dump_xrefs_to(ea):
    w("-" * 80)
    w("XREFS_TO %X (%s)" % (ea, fn_name(ea)))
    seen = set()
    for xref in idautils.CodeRefsTo(ea, 0):
        if xref in seen:
            continue
        seen.add(xref)
        fn = ida_funcs.get_func(xref)
        if fn:
            w("  code %X in %X..%X %s" %
              (xref, fn.start_ea, fn.end_ea,
               ida_name.get_name(fn.start_ea) or "<none>"))
        else:
            w("  code %X (no func)" % xref)
    for xref in idautils.DataRefsTo(ea):
        if xref in seen:
            continue
        seen.add(xref)
        w("  data %X" % xref)
    w("")


dump_func(0x36AA578)
dump_xrefs_to(0x36AA578)

try:
    import ida_hexrays
    fn = ida_funcs.get_func(0x36AA578)
    if fn:
        cf = ida_hexrays.decompile(fn.start_ea)
        if cf:
            w("=" * 100)
            w("DECOMPILE 36AA578")
            w(str(cf))
except Exception as exc:
    w("hexrays failed: %s" % exc)

f.close()
print("WROTE " + OUT)
idc.qexit(0)
