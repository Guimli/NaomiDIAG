local mem = manager.machine.devices[":maincpu"].spaces["program"]
local FB, W = 0x05200000, 640
local function rowpix(y)
    local n = 0
    for x = 8, 520, 2 do
        if mem:read_u16(FB + (y*W + x)*2) ~= 0 then n = n + 1 end
    end
    return n
end
local last = -1
notif = emu.add_machine_frame_notifier(function()
    local t = math.floor(manager.machine.time.seconds)
    if t ~= last and t > 2 then
        last = t
        local lab, bar = rowpix(425), rowpix(450)
        if lab > 0 or bar > 2 then
            print(string.format("[fb] t=%3ds  libelle: %3d px   barre remplie: %3d px", t, lab, bar))
        end
    end
end)
print("[label_probe] armed")
