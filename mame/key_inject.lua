local mem = manager.machine.devices[":maincpu"].spaces["program"]
local SCFTDR = 0xffe8000c
scif_tap = mem:install_write_tap(0xffe80000, 0xffe8000f, "scif_tx",
    function(offset, data, mask)
        for lane = 0, 7 do
            if (mask & (0xff << (8*lane))) ~= 0 and (offset + lane) == SCFTDR then
                io.write(string.char((data >> (8*lane)) & 0xff)) io.flush()
            end
        end
    end)

-- one simulated keystroke. SCFSR2 (0x10, 16-bit) and SCFRDR2 (0x14, 8-bit)
-- share one 64-bit word, so a single tap covers both.
local armed, sent = false, false
local KEY = string.byte(os.getenv("DIAGKEY") or "h")
notif = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds > 100 and not sent then armed = true end
end)
rt = mem:install_read_tap(0xffe80010, 0xffe80017, "scif_rx",
    function(offset, data, mask)
        if not armed or sent then return data end
        if (mask & 0xffff) ~= 0 then                 -- SCFSR2 read: raise DR
            return data | 0x0001
        end
        if (mask & (0xff << 32)) ~= 0 then           -- SCFRDR2 read: the byte
            sent = true
            print(string.format("\n[key] touche '%s' delivree", string.char(KEY)))
            return (data & ~(0xff << 32)) | (KEY << 32)
        end
        return data
    end)
print("[key_inject] armed")
