-- The resume after a DIMM-ordered reset, under MAME. MAME's full DIMM
-- emulation does not talk to any BIOS (debug only), so the DIMM's reset is
-- imitated: a soft reset of the machine as soon as the ROM has sent its
-- first message to the DIMM -- the moment the real board resets the Naomi.
-- Then the resume is expected: no suite, report back, DIMM test resumed.
--
-- DIAGKEYS: "trigger=key;..." as in dimm_host.lua; key '!' = soft reset.
-- The script runs again after each soft reset: its state lives in a global.
if DR == nil then
    DR = { step = 1, plan = {} }
    for trig, key in string.gmatch(os.getenv("DIAGKEYS") or
            "Press TEST=d;first message sent=!;= run=y", "([^=;]+)=([^;]+)") do
        DR.plan[#DR.plan + 1] = { trig = trig, key = key }
    end
end
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local pending, seen, reset_now = nil, "", false

local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8000f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            if (mask & (0xff << (8 * lane))) ~= 0 and (offset + lane) == SCFTDR then
                local c = string.char((data >> (8 * lane)) & 0xff)
                io.write(c) io.flush()
                seen = (seen .. c):sub(-200)
                local p = DR.plan[DR.step]
                if p and not pending and seen:find(p.trig, 1, true) then
                    DR.step = DR.step + 1
                    seen = ""
                    if p.key == "!" then reset_now = true else pending = string.byte(p.key) end
                end
            end
        end
    end)
rt = mem:install_read_tap(0xffe80010, 0xffe80017, "scif_rx",
    function(offset, data, mask)
        if not pending then return data end
        if (mask & 0xffff) ~= 0 then return data | 0x0001 end
        if (mask & (0xff << 32)) ~= 0 then
            local k = pending
            pending = nil
            print(string.format("\n[key] '%s'", string.char(k)))
            return (data & ~(0xff << 32)) | (k << 32)
        end
        return data
    end)
notif = emu.add_machine_frame_notifier(function()
    if reset_now then
        reset_now = false
        print("\n[dimm_resume] soft reset (stands for the DIMM's)")
        manager.machine:soft_reset()
    end
end)
print("[dimm_resume] armed, step " .. DR.step)
