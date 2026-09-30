-- Flips one byte of the cartridge ROM region at CART_FAULT_OFF (default
-- 0x800100, inside Cannon Spike's ic1), then runs cart_snap.lua: the chip
-- must come out BAD on the report grid.
local off = tonumber(os.getenv("CART_FAULT_OFF") or "0x800100")
for tag, r in pairs(manager.machine.memory.regions) do
    if r.size >= 0x1000000 and tag:find("rom_board") then
        r:write_u8(off, r:read_u8(off) ~ 0xFF)
        print(string.format("[cart_ic_fault] %s +%X flipped", tag, off))
    end
end
dofile("cart_snap.lua")
