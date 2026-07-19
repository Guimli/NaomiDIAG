local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local seen = {}
local n = 0
fault_tap = mem:install_write_tap(0x00000000, 0xdfffffff, "spy",
    function(offset, data, mask)
        local top = offset >> 24
        if not seen[top] then
            seen[top] = true
            n = n + 1
            print(string.format("[tap] first write in region %02x000000 @%08x mask=%016x", top, offset, mask))
        end
    end)
print("[fault_dbg] whole-space spy tap")
