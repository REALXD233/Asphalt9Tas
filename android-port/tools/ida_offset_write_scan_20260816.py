# IDAPython read-only scan for every instruction touching final-control
# offsets 0xC98 / 0xC9C / 0xE84 (and reference offsets 0xE78 / 0xE7C).
import re
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_offset_writes_C98_C9C_E84_20260816.txt"
f = open(OUT, "w", encoding="utf-8")

PAT = re.compile(r"#-?0x(E78|E7C|E84|C98|C9C)\]", re.I)
WRITE_MNEM = {"STR", "STUR", "STRB", "STURB", "STP", "STURP", "STLR",
              "STLRB", "STXR", "STXP"}


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


def w(s=""):
    f.write(s + "\n")


matches = 0
heads_total = 0
for seg_ea in idautils.Segments():
    seg = idaapi.getseg(seg_ea)
    if seg is None or not (seg.perm & idaapi.SEGPERM_EXEC):
        continue
    w("SEGMENT %s %X..%X class=%s" %
      (idaapi.get_segm_name(seg), seg.start_ea, seg.end_ea,
       idaapi.get_segm_class(seg)))
    ea = seg.start_ea
    while ea < seg.end_ea and ea != idaapi.BADADDR:
        heads_total += 1
        mnem = idc.print_insn_mnem(ea)
        if mnem:
            line = idc.generate_disasm_line(ea, 0)
            if PAT.search(line):
                matches += 1
                kind = "WRITE" if mnem.upper() in WRITE_MNEM else "READ "
                w("%08X [%s] %s  func=%s" %
                  (ea, kind, line, fn_name(ea)))
        ea = idc.next_head(ea, seg.end_ea)
        if ea == idaapi.BADADDR:
            break
    w("")

w("SUMMARY heads_scanned=%d matches=%d" % (heads_total, matches))
f.close()
print("WROTE " + OUT + " heads=%d matches=%d" % (heads_total, matches))
idc.qexit(0)
