local mem = manager.machine.devices[":maincpu"].spaces["program"]
local FB, W = 0x05200000, 640
-- the IC-name line of a failure is drawn in red at x=32; look for red pixels
local shown = false
notif = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds > 115 and not shown then
        shown = true
        local red = 0
        for y = 48, 409 do
            for x = 30, 200, 2 do
                local v = mem:read_u16(FB + (y*W + x)*2)
                if v ~= 0 and (v >> 11) > 20 and ((v >> 5) & 0x3f) < 24 then
                    red = red + 1
                end
            end
        end
        print(string.format("[dbus] pixels rouges dans la zone de rapport : %d", red))
    end
end)
print("[dbus_visible] armed")
