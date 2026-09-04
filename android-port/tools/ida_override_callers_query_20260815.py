# IDAPython read-only query #2: callers of vtable+0x2A8/0x2B0 thunks and
# sink-common tails. Writes a report only, never saves the database.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_override_callers_20260815.txt"
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


def dump_range(start, end, label=""):
    w("=" * 100)
    w("RANGE %X..%X %s" % (start, end, label))
    for head in idautils.Heads(start, end):
        w("%08X  %s" % (head, idc.generate_disasm_line(head, 0)))
    w("")


def dump_xrefs_to(ea, label=""):
    w("-" * 80)
    w("XREFS_TO %X (%s) %s" % (ea, fn_name(ea), label))
    for xref in idautils.CodeRefsTo(ea, 0):
        w("  code %X in %s" % (xref, fn_name(xref)))
    for xref in idautils.DataRefsTo(ea):
        w("  data %X in %s" % (xref, fn_name(xref)))
    w("")


dump_range(0x36797E0, 0x3679920, "callers of vtable+0x2A8 thunk")
dump_range(0x367A470, 0x367A540, "callers of vtable+0x2B0 thunk")
dump_range(0x3687DD0, 0x3687E30, "sink common tails")

for ea in (0x3687DE8, 0x3687DF4, 0x3687E00, 0x3687E0C,
           0x3687A10, 0x36877B0, 0x36879D0, 0x3687BE0, 0x3687BC8):
    dump_xrefs_to(ea)

# Data refs into the keyboard source getters / vehicle sink setters.
for ea in (0x385A7AC, 0x385A7D0, 0x385A7EC, 0x367B1F0, 0x367B21C,
           0x367B3E4, 0x367B548, 0x386B1E0):
    dump_xrefs_to(ea)

f.close()
print("WROTE " + OUT)
idc.qexit(0)
