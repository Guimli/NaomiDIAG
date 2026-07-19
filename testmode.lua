local machine = manager.machine
local scr = machine.screens[":screen"]
local mie5 = machine.ioport.ports[":MIE.5"]
local test_f, serv_f
for fname, f in pairs(mie5.fields) do
    if fname == "Service Mode" then test_f = f end
    if fname == "Service 1"   then serv_f = f end
end
local frame = 0
notif = emu.add_machine_frame_notifier(function()
    frame = frame + 1
    if frame == 2400 then test_f:set_value(1) end   -- 40s: menu
    if frame == 2460 then test_f:set_value(0) end
    if frame == 2820 then scr:snapshot() end        -- 47s
    if frame == 3000 then serv_f:set_value(1) end   -- 50s: cursor wrap -> RAM TEST
    if frame == 3060 then serv_f:set_value(0) end
    if frame == 3180 then scr:snapshot() end        -- 53s
    if frame == 3360 then test_f:set_value(1) end   -- 56s: launch RAM TEST
    if frame == 3420 then test_f:set_value(0) end
    if frame >= 3600 and frame % 900 == 0 then scr:snapshot() end
end)
print("[testmode] armed")
