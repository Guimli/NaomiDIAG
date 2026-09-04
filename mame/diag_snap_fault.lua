local machine = manager.machine
local mem = machine.devices[":maincpu"].spaces["program"]
local scr = machine.screens[":screen"]
local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8002f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            if (mask & (0xff << (8*lane))) ~= 0 and (offset + lane) == SCFTDR then
                io.write(string.char((data >> (8*lane)) & 0xff)) io.flush()
            end
        end
    end)
-- stuck-at-0 on D3 in a window of VRAM TEX1 (32-bit path)
fault_tap = mem:install_write_tap(0x05820000, 0x0582ffff, "bad_vram",
    function(offset, data, mask)
        return data & ~0x0000000800000008
    end)
local frame = 0
notif = emu.add_machine_frame_notifier(function()
    frame = frame + 1
    if frame % 1200 == 0 then scr:snapshot() end
end)
print("[diag_snap_fault] armed")
