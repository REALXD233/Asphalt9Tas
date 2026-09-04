# IDAPython read-only query #4: locate orphan function entries for the two
# sink-field consumer paths.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_orphan_entries_20260815.txt"
f = open(OUT, "w", encoding="utf-8")


def w(s=""):
    f.write(s + "\n")


def dump_range(start, end, label=""):
    w("=" * 100)
    w("RANGE %X..%X %s" % (start, end, label))
    for head in idautils.Heads(start, end):
        w("%08X  %s" % (head, idc.generate_disasm_line(head, 0)))
    w("")


def dump_xrefs_to(ea):
    w("-" * 80)
    w("XREFS_TO %X (%s)" % (ea, ida_name.get_name(ea) or "<none>"))
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


dump_range(0x3678DF0, 0x3678E80, "candidate entry path A")
dump_range(0x3679F70, 0x367A010, "candidate entry path B")

for ea in (0x3678E20, 0x3678E24, 0x3679F88, 0x3679F8C,
           0x3679040, 0x36790C0, 0x36791C0):
    dump_xrefs_to(ea)

# What is immediately before the first dumped instruction of each path?
dump_range(0x3678E00, 0x3679050, "entry A wider")
dump_range(0x3679F80, 0x367A060, "entry B wider")

f.close()
print("WROTE " + OUT)
idc.qexit(0)
