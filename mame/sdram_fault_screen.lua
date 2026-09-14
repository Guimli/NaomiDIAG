local machine = manager.machine
local mem = machine.devices[":maincpu"].spaces["program"]
local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8002f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            if (mask & (0xff << (8*lane))) ~= 0 and (offset + lane) == SCFTDR then
                io.write(string.char((data >> (8*lane)) & 0xff)) io.flush()
            end
        end
    end)
notif = emu.add_machine_frame_notifier(function()
    for a = 0x0c040000, 0x0c04f000, 0x1000 do
        local v = mem:read_u32(a)
        if (v & 0x20) ~= 0 then mem:write_u32(a, v & ~0x20) end
    end
end)
print("[sdram_cell_fault] D5 force a 0 en 0x0C040000-0x0C04F000")

-- red pixels in the report area = a FAIL line drawn on screen
local mem2 = manager.machine.devices[":maincpu"].spaces["program"]
local shown = false
probe = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds > 125 and not shown then
        shown = true
        local red = 0
        for y = 48, 460 do
            for x = 30, 620, 2 do
                local v = mem2:read_u16(0x05200000 + (y*640 + x)*2)
                if v ~= 0 and (v >> 11) > 20 and ((v >> 5) & 0x3f) < 24 then red = red + 1 end
            end
        end
        print(string.format("[screen] pixels rouges = %d", red))
    end
end)
