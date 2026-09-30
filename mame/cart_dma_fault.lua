-- Corrupts one word of the second cartridge DMA buffer every frame once the
-- cartridge check is under way: the digest of a DMA-read chip goes wrong and
-- the port re-read must clear it.
dofile("key_inject.lua")
local mem = manager.machine.devices[":maincpu"].spaces["program"]
cdf = emu.add_machine_frame_notifier(function()
    local t = manager.machine.time.seconds
    if t > tonumber(os.getenv("FAULT_T") or "101.9") then mem:write_u32(0x0c402100, 0xdeadbeef) end
end)
print("[cart_dma_fault] buffer 1 word 0x100 corrupted from t=" .. (os.getenv("FAULT_T") or "101.9") .. " s")
