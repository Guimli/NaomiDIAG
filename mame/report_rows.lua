local mem = manager.machine.devices[":maincpu"].spaces["program"]
local FB, W = 0x05200000, 640
local function rowhas(y)
    for x = 8, 400, 2 do
        if mem:read_u16(FB + (y*W + x)*2) ~= 0 then return true end
    end
    return false
end
local shown = false
notif = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds > 115 and not shown then
        shown = true
        local n, last = 0, -1
        for k = 0, 18 do                    -- renderer allows y = 48+20k < 410
            local y = 48 + k*20
            if rowhas(y + 4) or rowhas(y + 10) then n = n + 1; last = k end
        end
        print(string.format("[rows] occupees=%d  derniere=%d  capacite=19", n, last))
    end
end)
print("[report_rows] armed")
