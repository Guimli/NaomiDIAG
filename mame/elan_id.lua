-- MAME 0.288 reads 0 at the Elan ID register (0x08800000) where a real
-- Naomi 2 answers 0xE1AD0000. Fake the ID and a read/write IFCTL so the
-- Naomi 2 path can be exercised; the SCIF output goes to stdout.
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8002f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            local lm = 0xff << (8 * lane)
            if (mask & lm) ~= 0 and (offset + lane) == SCFTDR then
                io.write(string.char((data >> (8 * lane)) & 0xff)); io.flush()
            end
        end
    end)
-- The bus is 64 bits wide: a tap covers a register pair and its mask says
-- which half the access wants. Only reads are tapped -- a write tap on the
-- IFCTL pair aborts MAME ("integer value will be misrepresented") when the
-- refresh register beside it is written -- so IFCTL simply reads back as
-- CLXB enabled, channel 2 on Elan, no broadcast (6): what the ROM sets.
elan_r = mem:install_read_tap(0x08800000, 0x08800017, "elan_r",
    function(offset, data, mask)
        local lo = (mask & 0xffffffff) ~= 0
        if offset == 0x08800000 then return lo and 0xE1AD0000 or 0 end
        if offset == 0x08800010 then return lo and 6 or 0 end
        return 0
    end)
