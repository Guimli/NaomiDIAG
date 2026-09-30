-- For a looping VRAM test: from FAULT_T on, D5 forced to 0 in a TEX1
-- block, so a later pass fails and the soak screen has to show it.
dofile("loop_snap.lua")
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local ft = tonumber(os.getenv("FAULT_T") or "1e9")
lft = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds < ft then return end
    for a = 0x05840000, 0x0584f000, 0x1000 do
        local v = mem:read_u32(a)
        if (v & 0x20) ~= 0 then mem:write_u32(a, v & ~0x20) end
    end
end)
print("[loop_fault] TEX1 D5 stuck from t=" .. ft)
