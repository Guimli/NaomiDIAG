-- For the looping sound RAM test: from FAULT_T on, D5 forced to 0 in
-- 0x10000-0x1F000 of sound RAM, so a later pass fails.
dofile("loop_snap.lua")
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local ft = tonumber(os.getenv("FAULT_T") or "1e9")
laf = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds < ft then return end
    for a = 0x00810000, 0x0081f000, 0x1000 do
        local v = mem:read_u32(a)
        if (v & 0x20) ~= 0 then mem:write_u32(a, v & ~0x20) end
    end
end)
