-- loop_fault.lua, then TEST held for 1 s at STOP_T: the soak run must stop
-- and hand the screen back (snapshots continue after it).
dofile("loop_fault.lua")
local mie5 = manager.machine.ioport.ports[":MIE.5"]
local test_f = mie5.fields["Service Mode"]
local st = tonumber(os.getenv("STOP_T") or "1e9")
lst = emu.add_machine_frame_notifier(function()
    local t = manager.machine.time.seconds
    if t >= st and t < st + 1 then test_f:set_value(1) else test_f:set_value(0) end
end)
