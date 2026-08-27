local mem = manager.machine.devices[":maincpu"].spaces["program"]
local names = {[0]="BLACK",[0xff]="BLUE",[0xffff]="CYAN",[0xff00]="GREEN",
               [0xffff00]="YELLOW",[0xff00ff]="MAGENTA",[0xffffff]="WHITE",
               [0x808080]="GREY",[0xff0000]="RED"}
local last = -1
notif = emu.add_machine_frame_notifier(function()
    local v = mem:read_u32(0x005f8040) & 0xffffff
    if v ~= last then
        last = v
        print(string.format("[border] 0x%06x  %s", v, names[v] or "?"))
    end
end)
print("[border_tap] armed")
