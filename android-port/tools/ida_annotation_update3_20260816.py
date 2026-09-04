# IDAPython annotation update #3 for final-field writer audit 20260816.
import idaapi
import ida_name
import idc

N = ida_name.SN_NOCHECK | ida_name.SN_NOWARN


def setn(ea, name, cmt):
    old = ida_name.get_name(ea)
    if old and old != name and not old.startswith("sub_"):
        print("SKIP %X existing %s" % (ea, old))
        return
    ida_name.set_name(ea, name, N)
    idc.set_cmt(ea, cmt, 0)
    print("SET %X -> %s" % (ea, name))


setn(0x36934AC, "VehicleDynamics_SetValueC98_Direct",
     "Static audit 20260816: direct (non-adjusted) setter for final +0xC98.")
setn(0x36934D4, "VehicleDynamics_SetValueC9C_Direct",
     "Static audit 20260816: direct (non-adjusted) setter for final +0xC9C.")
setn(0x36A6758, "VehicleDynamics_ResetC9C_FromX22_1C_UnverifiedBoundary",
     "Static audit 20260816: writes +0xC9C from [X22+0x1C], zeroes "
     "+0xCA0/+0xCA4; called only from two reset-like paths near "
     "0x3691CC8/0x36927B0. Tick frequency unproven.")
setn(0x3691CC8, "VehicleDynamics_ResetPathA_ZeroC98_UnverifiedBoundary",
     "Static audit 20260816: reset-like path A zeroes +0xC98 then calls "
     "VehicleDynamics_ResetC9C_FromX22_1C_UnverifiedBoundary.")
setn(0x36927B0, "VehicleDynamics_ResetPathB_ZeroC98_UnverifiedBoundary",
     "Static audit 20260816: reset-like path B zeroes +0xC98 then calls "
     "VehicleDynamics_ResetC9C_FromX22_1C_UnverifiedBoundary.")

idaapi.save_database()
print("SAVED")
idc.qexit(0)
