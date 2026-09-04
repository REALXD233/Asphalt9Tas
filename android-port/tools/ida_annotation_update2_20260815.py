# IDAPython annotation update #2 for DeepSeek V4 Pro downstream resolution.
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


setn(0x36A8390, "VehicleDynamicsDispatch_C98_Thunk2A8",
     "Runtime 20260815: vtable+0x2A8 thunk; adjusts via metadata -0x2D8, "
     "then inner object vtable+0x2A8 -> final owner+0xC98.")
setn(0x36A83BC, "VehicleDynamicsDispatch_C9C_Thunk2B0",
     "Runtime 20260815: vtable+0x2B0 thunk; adjusts via metadata -0x2E0, "
     "then inner object vtable+0x2B0 -> final owner+0xC9C.")
setn(0x36934B8, "VehicleDynamics_SetValueC98_Vtable2A8",
     "Final consumer of sink owner+0xE78 (S-axis). Stores incoming float "
     "at adjusted owner+0xC98.")
setn(0x36934E0, "VehicleDynamics_SetValueC9C_Vtable2B0",
     "Final consumer of sink owner+0xE7C (steering). Stores incoming float "
     "at adjusted owner+0xC9C.")

idaapi.save_database()
print("SAVED")
idc.qexit(0)
