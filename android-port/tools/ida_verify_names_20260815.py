import idaapi
import ida_name
import idc

for ea in (0x3687BE0, 0x3687BC8, 0x3679394, 0x3679F88,
           0x385A7D0, 0x385A7EC, 0x367B1F0, 0x367B21C, 0x386B1E0):
    print("%X name=%s cmt=%s" % (ea, ida_name.get_name(ea) or "<none>",
                                  idc.get_cmt(ea, 0) or "<none>"))
idc.qexit(0)
