local mem = manager.machine.devices[":maincpu"].spaces["program"]
print(string.format("[id] HOLLY ID   @5F8000 = %08x", mem:read_u32(0x005f8000)))
print(string.format("[id] HOLLY REV  @5F8004 = %08x", mem:read_u32(0x005f8004)))
print(string.format("[id] SH4 PVR    @FF000030 = %08x", mem:read_u32(0xff000030)))
print(string.format("[id] SH4 PRR    @FF000044 = %08x", mem:read_u32(0xff000044)))
manager.machine:exit()
