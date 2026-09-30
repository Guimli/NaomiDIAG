-- Key injection (DIAGKEY) plus a snapshot every SNAP_EVERY emulated seconds
-- from SNAP_T on: follows a looping test's screen over its passes.
dofile("key_inject.lua")
local scr = manager.machine.screens[":screen"]
local t0 = tonumber(os.getenv("SNAP_T") or "101")
local every = tonumber(os.getenv("SNAP_EVERY") or "3")
local nexts = t0
lsn = emu.add_machine_frame_notifier(function()
    local t = manager.machine.time.seconds
    if t >= nexts then
        nexts = nexts + every
        scr:snapshot()
        print(string.format("[loop_snap] t=%.1f", t))
    end
end)
