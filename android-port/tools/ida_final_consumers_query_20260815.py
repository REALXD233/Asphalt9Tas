# IDAPython read-only query: final consumers vtable+0x2A8/0x2B0 on the
# runtime-resolved inner object (vtable 0x7EED9E0).
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_final_consumers_20260815.txt"
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


def dump_func(ea, max_insns=180):
    w("=" * 100)
    w("FUNC_AT %X name=%s" % (ea, fn_name(ea)))
    fn = ida_funcs.get_func(ea)
    if fn is None:
        start, end = ea, ea + 0x200
        w("NO BOUNDARY")
    else:
        start, end = fn.start_ea, fn.end_ea
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


for ea in (0x36934B8, 0x36934E0):
    dump_func(ea)
    dump_xrefs_to(ea)

w("VTABLE_SLOTS around 7EED9E0")
for slot in range(0x2A0, 0x2C0, 8):
    ea = 0x7EED9E0 + slot
    val = idc.get_qword(ea)
    w("%X +%X = %X (%s)" % (0x7EED9E0, slot, val, fn_name(val)))

try:
    import ida_hexrays
    for ea in (0x36934B8, 0x36934E0):
        fn = ida_funcs.get_func(ea)
        if fn:
            try:
                cf = ida_hexrays.decompile(fn.start_ea)
                if cf:
                    w("=" * 100)
                    w("DECOMPILE %X" % ea)
                    w(str(cf))
            except Exception as exc:
                w("decompile %X failed: %s" % (ea, exc))
except Exception as exc:
    w("hexrays failed: %s" % exc)

f.close()
print("WROTE " + OUT)
idc.qexit(0)
