local n = 0
for tag, dev in pairs(manager.machine.devices) do
    n = n + 1
    if tag:find("pvr") or tag:find("elan") or tag:find("powervr") then
        print("[dev] " .. tag)
    end
end
print("[dev] total devices: " .. n)
local mem = manager.machine.devices[":maincpu"].spaces["program"]
mem:write_u32(0x04000000, 0x11112222)
mem:write_u32(0x06000000, 0x33334444)
print(string.format("[dev] tex1@04000000=%08x tex2@06000000=%08x",
      mem:read_u32(0x04000000), mem:read_u32(0x06000000)))
manager.machine:exit()
