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
    for a = 0x0d000000, 0x0dfff000, 0x1000 do
        local v = mem:read_u32(a)
        if (v & 0x20) ~= 0 then mem:write_u32(a, v & ~0x20) end
    end
end)
print("[diag_fault] armed")
