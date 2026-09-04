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
-- stuck-at-0 D2 on EVEN byte lanes of a backup-SRAM window
fault_tap = mem:install_write_tap(0x00202000, 0x00202fff, "bad_sram",
    function(offset, data, mask)
        return data & ~0x0004000400040004
    end)
print("[sram_fault] armed")
