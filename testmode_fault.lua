local machine = manager.machine
local scr = machine.screens[":screen"]
local mem = machine.devices[":maincpu"].spaces["program"]
local mie5 = machine.ioport.ports[":MIE.5"]
local test_f, serv_f
for fname, f in pairs(mie5.fields) do
    if fname == "Service Mode" then test_f = f end
    if fname == "Service 1"   then serv_f = f end
end
local frame = 0
local checked = false
notif = emu.add_machine_frame_notifier(function()
    frame = frame + 1
    -- sparse stuck-at: clear D5 of the FIRST EVEN word of every 4KB block
    -- in the top 16MB of WORK RAM (0x0D000000-0x0DFFFFFF)
    for a = 0x0d000000, 0x0dfff000, 0x1000 do
        local v = mem:read_u32(a)
        if (v & 0x20) ~= 0 then mem:write_u32(a, v & ~0x20) end
    end
    if not checked and frame == 100 then
        checked = true
        mem:write_u32(0x0d000000, 0xFFFFFFFF)
        local rb = mem:read_u32(0x0d000000)
        print(string.format("[testmode] lua-write check: %08x", rb))
    end
    if frame == 2400 then test_f:set_value(1) end
    if frame == 2460 then test_f:set_value(0) end
    if frame == 3000 then serv_f:set_value(1) end
    if frame == 3060 then serv_f:set_value(0) end
    if frame == 3360 then test_f:set_value(1) end
    if frame == 3420 then test_f:set_value(0) end
    if frame >= 3600 and frame % 1800 == 0 then scr:snapshot() end
end)
print("[testmode] armed, sparse even-word D5 fault on top 16MB")
