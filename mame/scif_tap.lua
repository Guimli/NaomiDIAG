-- Capture SH4 SCIF transmit-FIFO writes (SCFTDR2 @ 0xFFE8000C) under MAME.
-- The SH4 program space is 64-bit wide: taps receive qword-aligned accesses
-- with a byte-lane mask, so extract the byte aimed at 0xFFE8000C.
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

-- PC sampler: proves the CPU is (or is not) running our code.
local samples = 0
pc_notifier = emu.add_machine_frame_notifier(
    function()
        samples = samples + 1
        if samples % 300 == 0 then
            print(string.format("\n[pc] %08x", cpu.state["PC"].value))
        end
    end)
print("[scif_tap] installed (64-bit lane-aware)")
