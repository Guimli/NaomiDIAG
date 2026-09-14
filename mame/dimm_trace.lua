local mem = manager.machine.devices[":maincpu"].spaces["program"]
local nw, nr = 0, 0
wt = mem:install_write_tap(0x005f7000, 0x005f707f, "dimmw",
    function(offset, data, mask)
        nw = nw + 1
        if nw < 40 then print(string.format("[W] %08X mask=%016X t=%.2f", offset, mask, manager.machine.time.seconds)) end
    end)
rt = mem:install_read_tap(0x005f7038, 0x005f704f, "dimmr",
    function(offset, data, mask)
        nr = nr + 1
        if nr < 20 then print(string.format("[R] %08X t=%.2f", offset, manager.machine.time.seconds)) end
        return data
    end)
notif = emu.add_machine_frame_notifier(function()
    local t = math.floor(manager.machine.time.seconds)
    if t == 55 then print(string.format("[bilan] ecritures G1=%d  lectures mailbox=%d", nw, nr)) end
end)
print("[dimm_trace] armed")
