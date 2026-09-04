# IDAPython read-only query #5: locate large prologue for path A and dump
# callers of both path entries.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_pathA_entry_20260815.txt"
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


dump_range(0x3679040, 0x3679300, "looking for path A prologue")

w("PROLOGUE CANDIDATES (SUB SP, SP, #imm with imm>=0x100)")
for head in idautils.Heads(0x3679000, 0x3679680):
    if idc.print_insn_mnem(head) == "SUB":
        ops = []
        ok = True
        for i in range(2):
            if idc.get_operand_type(head, i) == idc.o_void:
                ok = False
                break
            ops.append(idc.print_operand(head, i))
        line = idc.generate_disasm_line(head, 0)
        if "SP" in line and "#0x1" in line:
            w("%08X  %s" % (head, line))

for ea in (0x3679F88, 0x3679040, 0x374179C, 0x36E7B28):
    dump_xrefs_to(ea)

dump_range(0x3741700, 0x3741840, "caller of path B entry")
dump_range(0x36E7A80, 0x36E7C00, "caller around 3679040")

f.close()
print("WROTE " + OUT)
idc.qexit(0)
