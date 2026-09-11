local mem = manager.machine.devices[":maincpu"].spaces["program"]
local AICA = 0x00700000
local function r32(a) return mem:read_u32(a) end
local shown = false
notif = emu.add_machine_frame_notifier(function()
    local t = manager.machine.time.seconds
    if t > 18 and not shown then
        shown = true
        print("=== AICA registres communs ===")
        for _, off in ipairs({0x2800,0x2804,0x2808,0x280C,0x2810,0x2814,0x2880,0x2884,0x2C00}) do
            print(string.format("  0x%04X = 0x%08X", off, r32(AICA + off)))
        end
        print("=== slot 0 ===")
        for off = 0, 0x2C, 4 do
            print(string.format("  +0x%02X = 0x%08X", off, r32(AICA + off)))
        end
        print("=== premier slot avec KYONB actif ===")
        for ch = 0, 63 do
            local v = r32(AICA + ch*0x80)
            if (v & 0x4000) ~= 0 then
                print(string.format("  slot %d: 0x00=0x%08X 0x24=0x%08X 0x28=0x%08X",
                      ch, v, r32(AICA+ch*0x80+0x24), r32(AICA+ch*0x80+0x28)))
            end
        end
    end
end)
print("[aica_dump] armed")
