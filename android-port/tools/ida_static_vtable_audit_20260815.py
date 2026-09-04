# IDAPython read-only static audit: which vtables expose the final setters
# 0x36934B8 (slot 0x2A8) / 0x36934E0 (slot 0x2B0), who constructs those
# objects, and how that set relates to known input-path vtables.
import idaapi
import ida_funcs
import ida_name
import idautils
import idc

OUT = r"C:\Users\Administrator\Documents\a9tasv2\android-port\evidence\ida_static_vtable_audit_20260815.txt"
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


def dump_vtable(base, label):
    w("=" * 100)
    w("VTABLE_BASE %X %s" % (base, label))
    w("  name=%s" % (ida_name.get_name(base) or "<none>"))
    for slot in range(0, 0x80, 8):
        ea = base + slot
        val = idc.get_qword(ea)
        w("  +%02X = %X (%s)" % (slot, val, fn_name(val)))
    w("  xrefs-to-base:")
    seen = set()
    for x in idautils.DataRefsTo(base):
        if x in seen:
            continue
        seen.add(x)
        fn = ida_funcs.get_func(x)
        if fn:
            w("    data %X in %X..%X %s" %
              (x, fn.start_ea, fn.end_ea,
               ida_name.get_name(fn.start_ea) or "<none>"))
        else:
            w("    data %X (no func)" % x)
    for x in idautils.CodeRefsTo(base, 0):
        if x in seen:
            continue
        seen.add(x)
        fn = ida_funcs.get_func(x)
        if fn:
            w("    code %X in %X..%X %s" %
              (x, fn.start_ea, fn.end_ea,
               ida_name.get_name(fn.start_ea) or "<none>"))
        else:
            w("    code %X (no func)" % x)
    w("")


def bases_for_slot_function(fn_ea, slot):
    bases = []
    for x in idautils.DataRefsTo(fn_ea):
        base = x - slot
        if idc.get_qword(x) == fn_ea:
            bases.append(base)
        else:
            w("UNEXPECTED_REF %X -> %X for fn %X slot %X" %
              (x, idc.get_qword(x), fn_ea, slot))
    return sorted(set(bases))


bases_c98 = bases_for_slot_function(0x36934B8, 0x2A8)
bases_c9c = bases_for_slot_function(0x36934E0, 0x2B0)

w("FINAL SETTER 36934B8 (C98, slot 2A8) vtable bases:")
for b in bases_c98:
    w("  %X" % b)
w("FINAL SETTER 36934E0 (C9C, slot 2B0) vtable bases:")
for b in bases_c9c:
    w("  %X" % b)
w("")

for b in sorted(set(bases_c98 + bases_c9c)):
    dump_vtable(b, "final-setter vtable")

# Known input-path vtable bases for comparison.
known = [
    (0x80BF1C0, "KeyboardAxisSource vtable"),
    (0x7EEA080, "VehicleControlSink vtable (runtime sink)"),
    (0x80C4DA8, "GameplayInputController vtable (UpdatePerTick at +0x70)"),
    (0x80C53A0, "gameplay action subscriber vtable"),
    (0x80C53F8, "scalar action subscriber vtable"),
    (0x80C54A8, "bool action subscriber vtable"),
    (0x7EF09B0, "downstream dispatch vtable (runtime)"),
    (0x7EED9E0, "inner downstream vtable (runtime)"),
]
w("=" * 100)
w("KNOWN INPUT-PATH VTABLE BASES")
for b, label in known:
    w("%X %s (present in final set: %s)" %
      (b, label, b in set(bases_c98 + bases_c9c)))

# Sink setters are referenced by many vtables; enumerate their bases too.
w("")
w("SINK SETTER VTABLE BASES (for 367B1F0 slot 0x20)")
for b in bases_for_slot_function(0x367B1F0, 0x20):
    w("  %X" % b)
w("SINK SETTER VTABLE BASES (for 367B21C slot 0x28)")
for b in bases_for_slot_function(0x367B21C, 0x28):
    w("  %X" % b)

# Overlap between final-setter vtable construction sites and any known
# input-related function names.
w("")
w("CONSTRUCTION SITE FUNCTIONS FOR FINAL-SETTER VTABLES")
sites = set()
for b in set(bases_c98 + bases_c9c):
    for x in idautils.DataRefsTo(b):
        fn = ida_funcs.get_func(x)
        sites.add((fn.start_ea if fn else 0, fn_name(x) if fn else "(no func)"))
for start, name in sorted(sites):
    w("  %X %s" % (start, name))

f.close()
print("WROTE " + OUT)
idc.qexit(0)
