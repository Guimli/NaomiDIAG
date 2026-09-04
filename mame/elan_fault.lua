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
fault_tap = mem:install_write_tap(0x0a040000, 0x0a04ffff, "bad_elan",
    function(offset, data, mask)
        return data & ~0x0000008000000080
    end)
print("[elan_fault] armed")
