local mem = manager.machine.devices[":maincpu"].spaces["program"]
local addrs = {0x08800000, 0x08800004, 0x09800000, 0x005f6800, 0x025f8000,
               0x025f8004, 0x0a000000, 0x08000000, 0x088000f0, 0x01000000}
for _, a in ipairs(addrs) do
    print(string.format("[n2] %08x = %08x", a, mem:read_u32(a)))
end
manager.machine:exit()
