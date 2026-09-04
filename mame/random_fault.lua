-- Inject ONE random hardware fault so the diagnostic ROM can be exercised
-- the way a genuinely broken board would behave. The chosen fault is
-- written to fault_answer.txt, so a run can be graded afterwards without
-- knowing the answer in advance.
--
-- Implementation note: a memory *tap* cannot be used here. MAME hands the
-- full 64-bit bus value to Lua, and the diagnostic writes 0xAAAAAAAA over
-- both lanes (bit 63 set), which overflows Lua's signed integer and aborts
-- the emulation. Instead a frame notifier keeps clearing the faulty bit
-- over a window of the target memory, using plain 32-bit accesses.

local mem = manager.machine.devices[":maincpu"].spaces["program"]

-- SCIF console capture (same tap as scif_tap.lua)
local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8002f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            if (mask & (0xff << (8 * lane))) ~= 0 and (offset + lane) == SCFTDR then
                io.write(string.char((data >> (8 * lane)) & 0xff))
                io.flush()
            end
        end
    end)

math.randomseed(os.time())

-- candidate targets: {label, window start, window end, widest bit}
local targets = {
    { "MAIN CPU RAM (SDRAM)",   0x0c040000, 0x0c043ffc, 31 },
    { "SOUND RAM (AICA)",       0x00840000, 0x00843ffc, 31 },
    { "VRAM TEX1 (IC35)",       0x05820000, 0x05823ffc, 31 },
    { "BACKUP SRAM",            0x00202000, 0x00202ffc,  7 },
}

local t = targets[math.random(1, #targets)]
local label, lo, hi, maxbit = t[1], t[2], t[3], t[4]
local bit = math.random(0, maxbit)
local clear = ~(1 << bit)
if maxbit == 7 then                     -- byte-wide device: every byte lane
    clear = ~(0x01010101 * (1 << bit))
end

fault_notifier = emu.add_machine_frame_notifier(function()
    for a = lo, hi, 4 do
        local v = mem:read_u32(a)
        if (v & ~clear) ~= 0 then
            mem:write_u32(a, v & clear)
        end
    end
end)

local answer = string.format("%s -- data bit D%d forced to 0\nwindow 0x%08x-0x%08x\n",
                             label, bit, lo, hi)
local f = io.open("fault_answer.txt", "w")
f:write(answer)
f:close()

print("[random_fault] one fault injected (answer sealed in fault_answer.txt)")
