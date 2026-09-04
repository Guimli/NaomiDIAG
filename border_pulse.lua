local mem = manager.machine.devices[":maincpu"].spaces["program"]
local last, t0, n = -1, nil, 0
notif = emu.add_machine_frame_notifier(function()
    local v = mem:read_u32(0x005f8040) & 0xffffff
    if v ~= last then
        local t = manager.machine.time.seconds + manager.machine.time.attoseconds/1e18
        if t0 then print(string.format("[border] 0x%06x  a t=%.2fs  (+%.2fs)", v, t, t-t0)) end
        t0 = t; last = v; n = n + 1
    end
end)
print("[border_pulse] armed")
