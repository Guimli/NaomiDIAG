local mem = manager.machine.devices[":maincpu"].spaces["program"]
local AICA = 0x00700000
local shown = false
local names = {[0x28]="Q/TL",[0x2C]="FLV0",[0x30]="FLV1",[0x34]="FLV2",
               [0x38]="FLV3",[0x3C]="FLV4",[0x40]="FAR/FD1R",[0x44]="FD2R/FRR"}
notif = emu.add_machine_frame_notifier(function()
    if manager.machine.time.seconds > 18 and not shown then
        shown = true
        for ch = 0, 1 do
            print(string.format("=== slot %d ===", ch))
            for off = 0x28, 0x44, 4 do
                print(string.format("  +0x%02X %-9s = 0x%04X", off, names[off] or "",
                      mem:read_u32(AICA + ch*0x80 + off) & 0xffff))
            end
        end
    end
end)
print("[aica_slot] armed")
