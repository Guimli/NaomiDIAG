-- Types a few serial-monitor lines after boot (MONKEYS, ';' separated).
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local SCFTDR = 0xffe8000c
st = mem:install_write_tap(0xffe80000, 0xffe8000f, "scif_tx", function(offset, data, mask)
    for lane = 0, 7 do
        if (mask & (0xff << (8*lane))) ~= 0 and (offset + lane) == SCFTDR then
            io.write(string.char((data >> (8*lane)) & 0xff)) io.flush()
        end
    end end)
local text = "!" .. (os.getenv("MONKEYS") or "r l a05f7034 2;d;x;u f;q"):gsub(";", "\r") .. "\r"
local pos, armed, wait = 1, false, 0
fn = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds > 100 then armed = true end end)
rt = mem:install_read_tap(0xffe80010, 0xffe80017, "scif_rx", function(offset, data, mask)
    if not armed or pos > #text then return data end
    if (mask & 0xffff) ~= 0 then
        wait = wait + 1
        if wait < 40 then return data end
        return data | 0x0001
    end
    if (mask & (0xff << 32)) ~= 0 then
        local c = text:byte(pos); pos = pos + 1; wait = 0
        return (data & ~(0xff << 32)) | (c << 32)
    end
    return data end)
