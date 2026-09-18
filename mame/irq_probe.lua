local mem = manager.machine.devices[":maincpu"].spaces["program"]
local function r32(a) return mem:read_u32(a) end
local function r16(a) return mem:read_u16(a) end
local shown = 0
notif = emu.add_machine_frame_notifier(function()
    local t = math.floor(manager.machine.time.seconds)
    if (t == 25 or t == 60) and shown ~= t then
        shown = t
        print(string.format("=== t=%ds ===", t))
        print(string.format("  SH4 INTC  IPRA=%04X IPRB=%04X IPRC=%04X ICR=%04X",
              r16(0xFFD00004), r16(0xFFD00008), r16(0xFFD0000C), r16(0xFFD00000)))
        print(string.format("  HOLLY pendant  ISTNRM=%08X ISTEXT=%08X ISTERR=%08X",
              r32(0x005F6900), r32(0x005F6904), r32(0x005F6908)))
        print(string.format("  HOLLY masques  IML2=%08X/%08X/%08X",
              r32(0x005F6910), r32(0x005F6914), r32(0x005F6918)))
        print(string.format("                 IML4=%08X/%08X/%08X",
              r32(0x005F6920), r32(0x005F6924), r32(0x005F6928)))
        print(string.format("                 IML6=%08X/%08X/%08X",
              r32(0x005F6930), r32(0x005F6934), r32(0x005F6938)))
    end
end)
print("[irq_probe] armed")
