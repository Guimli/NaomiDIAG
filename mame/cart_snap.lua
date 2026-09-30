-- Key injection (DIAGKEY) plus one snapshot at SNAP_T emulated seconds:
-- the cartridge report screen, for the README and for checking the layout.
dofile("key_inject.lua")
local scr = manager.machine.screens[":screen"]
local snap_t = tonumber(os.getenv("SNAP_T") or "150")
local done = false
csn = emu.add_machine_frame_notifier(function()
    if not done and manager.machine.time.seconds > snap_t then
        done = true
        scr:snapshot()
        print("[cart_snap] snapshot at " .. snap_t .. " s")
    end
end)
