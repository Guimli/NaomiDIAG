-- Every ROM-board (0x5F7000-0x5F70FF) read and write and every G1 control
-- write (0x5F74xx), in order, through MAME's debugger -- no 64-bit tap crash.
-- Run with: DISPLAY= mame <game> -debug -debugger none -autoboot_script g1_bus_trace.lua
-- This is how the original BIOS's G1 unlock (0x5F74E4) was found.
local d = manager.machine.debugger
d:command('wpset 5f7000,100,r,1,{printf "R %08x %04x pc=%08x", wpaddr, wpdata, pc; g}')
d:command('wpset 5f7000,100,w,1,{printf "W %08x %08x pc=%08x", wpaddr, wpdata, pc; g}')
d:command('wpset 5f7400,100,w,1,{printf "G %08x %08x pc=%08x", wpaddr, wpdata, pc; g}')
d:command('g')
n = 0
fn = emu.add_machine_frame_notifier(function()
    local log = d.consolelog
    while n < #log do n = n + 1; if log[n]:match("^[RWG] ") then print(log[n]) end end
end)
