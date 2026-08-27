-- Fault injection for validating the diag ROM under MAME:
-- simulate sound RAM data bit D5 stuck at 0 in 0x00810000-0x0081FFFF
-- (inside the first MB tested by the QUICK build), on top of the SCIF tap.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]

local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8002f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            local lanemask = 0xff << (8 * lane)
            if (mask & lanemask) ~= 0 and (offset + lane) == SCFTDR then
                io.write(string.char((data >> (8 * lane)) & 0xff))
                io.flush()
            end
        end
    end)

-- stuck-at-0 on D5 of every 32-bit word in the faulty window
fault_tap = mem:install_write_tap(0x00810000, 0x0081ffff, "bad_aram",
    function(offset, data, mask)
        return data & ~0x0000002000000020
    end)
print("[aram_fault] D5 stuck-at-0 in ARAM 0x00810000-0x0081FFFF")
