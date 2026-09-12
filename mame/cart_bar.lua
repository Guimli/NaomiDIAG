local mem = manager.machine.devices[":maincpu"].spaces["program"]
local FB, W = 0x05200000, 640
local function barfill()            -- green pixels along the bar interior
    local n = 0
    for x = 18, 620, 4 do
        local v = mem:read_u16(FB + (450*W + x)*2)
        if v ~= 0 then n = n + 1 end
    end
    return n
end
local function labelpix()
    local n = 0
    for x = 16, 400, 2 do
        if mem:read_u16(FB + (425*W + x)*2) ~= 0 then n = n + 1 end
    end
    return n
end
local last = -1
notif = emu.add_machine_frame_notifier(function()
    local t = math.floor(manager.machine.time.seconds)
    if t ~= last and t > 100 then
        last = t
        print(string.format("[bar] t=%3ds  remplissage=%3d  libelle=%3d px", t, barfill(), labelpix()))
    end
end)
print("[cart_bar] armed")
