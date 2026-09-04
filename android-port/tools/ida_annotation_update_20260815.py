# IDAPython minimal annotation update for DeepSeek V4 Pro 20260815.
# Adds only evidence-supported neutral names/comments. Backup exists under
# ida/backup/a9cn_600k_targeted_annotated_20260815_before_deepseek.
import idaapi
import ida_name
import idc

N = ida_name.SN_NOCHECK | ida_name.SN_NOWARN


def setn(ea, name, cmt):
    old = ida_name.get_name(ea)
    if old and old != name and not old.startswith("sub_"):
        print("SKIP %X existing name %s" % (ea, old))
        return
    ida_name.set_name(ea, name, N)
    idc.set_cmt(ea, cmt, 0)
    print("SET %X -> %s" % (ea, name))


setn(0x3687BE0, "VehicleControlDispatch_E78_Vtable2A8",
     "Evidence 20260815: called after reading sink owner+0xE78; forwards "
     "(owner+0x30, &local_e78) to vtable+0x2A8. Neutral downstream.")
setn(0x3687BC8, "VehicleControlDispatch_E7C_Vtable2B0",
     "Evidence 20260815: called after reading sink owner+0xE7C; forwards "
     "(owner+0x30, &local_e7c) to vtable+0x2B0. Neutral downstream.")
setn(0x3679394, "VehicleControlUpdate_PathA_UnverifiedBoundary",
     "Evidence 20260815: orphan region around 0x36797E0; reads +0xE78/+0xE7C "
     "and dispatches through 0x3687BE0/0x3687BC8. Function boundary not fixed.")
setn(0x3679F88, "VehicleControlUpdate_PathB_UnverifiedBoundary",
     "Evidence 20260815: reads +0xE78/+0xE7C and dispatches through "
     "0x3687BE0/0x3687BC8. Called from 0x374179C. Function boundary not fixed.")

idc.set_cmt(0x3679838, "reads sink owner+0xE78 -> VehicleControlDispatch_E78_Vtable2A8", 0)
idc.set_cmt(0x367984C, "reads sink owner+0xE7C -> VehicleControlDispatch_E7C_Vtable2B0", 0)
idc.set_cmt(0x367A4C8, "reads sink owner+0xE78 -> VehicleControlDispatch_E78_Vtable2A8", 0)
idc.set_cmt(0x367A4DC, "reads sink owner+0xE7C -> VehicleControlDispatch_E7C_Vtable2B0", 0)

# Dynamic mapping evidence (20260815): A/D -> value_A (steering), S -> value_B.
idc.set_cmt(0x385A7D0,
            "DYNAMIC 20260815: A key -> smoothed -1, D key -> smoothed +1. "
            "ValueA is steering. Neutral name retained.", 0)
idc.set_cmt(0x385A7EC,
            "DYNAMIC 20260815: S key -> immediate -1. ValueB is the only "
            "keyboard negative-longitudinal axis; Space has no A/B change.", 0)
idc.set_cmt(0x367B1F0,
            "DYNAMIC 20260815: receives value_B from UpdatePerTick; S key -> -1 "
            "observed at owner+0xE78.", 0)
idc.set_cmt(0x367B21C,
            "DYNAMIC 20260815: receives value_A from UpdatePerTick; A -> -1, "
            "D -> +1 observed at owner+0xE7C (steering).", 0)
idc.set_cmt(0x386B1E0,
            "DYNAMIC 20260815: A/D drive value_A (steering), S drives value_B "
            "negative; keyboard value_C remains process-wide 0. Conditional "
            "value_B clamp controlled by +0x104/+0x105.", 0)

idaapi.save_database()
print("SAVED")
idc.qexit(0)
