-- SCIF tap that stamps every completed line with emulated time and with the
-- SH-4's own cycle count, so two builds can be compared on the same run.
--
-- Emulated seconds are what the ROM's TMU would see; total cycles are what
-- the CPU actually issued. MAME does not model the bus wait states of a real
-- Naomi, so both numbers understate what a change to an UNCACHED loop -- the
-- boot EPROM runs at P2 -- is worth on real hardware. They are still exact
-- for comparing two builds against each other.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local SCFTDR = 0xffe8000c

local line, t0, c0 = "", 0.0, 0

local function now()
    return manager.machine.time:as_double(), cpu.state["TOTALCYCLES"] and
           cpu.state["TOTALCYCLES"].value or 0
end

scif_tap = mem:install_write_tap(0xffe80000, 0xffe8002f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            local lanemask = 0xff << (8 * lane)
            if (mask & lanemask) ~= 0 and (offset + lane) == SCFTDR then
                local ch = string.char((data >> (8 * lane)) & 0xff)
                if ch == "\n" then
                    local t, c = now()
                    io.write(string.format("[%8.3fs +%7.3f] %s\n",
                                           t, t - t0, line))
                    io.flush()
                    line, t0, c0 = "", t, c
                elseif ch ~= "\r" then
                    line = line .. ch
                end
            end
        end
    end)
print("[scif_time] installed")
