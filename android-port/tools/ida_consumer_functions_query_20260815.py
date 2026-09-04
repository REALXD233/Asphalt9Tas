# IDAPython read-only query #3: identify functions containing the two
# sink-field consumer paths and their upstream callers.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_consumer_functions_20260815.txt"
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


def dump_funcs_in(start, end):
    w("=" * 100)
    w("FUNCTIONS WITH START IN %X..%X" % (start, end))
    ea = start
    while ea < end:
        fn = ida_funcs.get_next_func(ea)
        if fn is None or fn.start_ea >= end:
            break
        w("%X..%X size=%X flags=%X name=%s" %
          (fn.start_ea, fn.end_ea, fn.end_ea - fn.start_ea,
           fn.flags, ida_name.get_name(fn.start_ea) or "<none>"))
        ea = fn.start_ea + 1
    w("")


def dump_range(start, end, label=""):
    w("=" * 100)
    w("RANGE %X..%X %s" % (start, end, label))
    for head in idautils.Heads(start, end):
        w("%08X  %s" % (head, idc.generate_disasm_line(head, 0)))
    w("")


def dump_xrefs_to(ea, label=""):
    w("-" * 80)
    w("XREFS_TO %X (%s) %s" % (ea, fn_name(ea), label))
    seen = set()
    for xref in idautils.CodeRefsTo(ea, 0):
        if xref in seen:
            continue
        seen.add(xref)
        fn = ida_funcs.get_func(xref)
        if fn:
            w("  code %X in %X..%X %s" %
              (xref, fn.start_ea, fn.end_ea, ida_name.get_name(fn.start_ea)))
        else:
            w("  code %X (no func)" % xref)
    for xref in idautils.DataRefsTo(ea):
        if xref in seen:
            continue
        seen.add(xref)
        w("  data %X" % xref)
    w("")


dump_funcs_in(0x3670000, 0x367C000)

dump_range(0x3679600, 0x3679890, "path A prologue and consumer calls")
dump_range(0x367A300, 0x367A510, "path B prologue and consumer calls")

# Probe plausible function starts around both paths.
for ea in range(0x3679000, 0x3679900, 0x40):
    n = ida_name.get_name(ea)
    if n:
        w("name @ %X = %s" % (ea, n))
for ea in range(0x367A300, 0x367A520, 0x40):
    n = ida_name.get_name(ea)
    if n:
        w("name @ %X = %s" % (ea, n))

# The two consumer thunk targets and likely nearby helpers.
for ea in (0x3679848, 0x367985C, 0x367A4D8, 0x367A4EC,
           0x3687B80, 0x3687BD4, 0x3687C34, 0x3687B24, 0x3687B38):
    dump_xrefs_to(ea)

f.close()
print("WROTE " + OUT)
idc.qexit(0)
