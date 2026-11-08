-- Load from an -autoboot_script (with -debug -debugger none): when the
-- bus-error or address-error handler runs, print the 68000 exception frame
-- (status, access address, instruction, SR, PC) and the registers, and
-- append them to crash.txt in the current directory.
local cpu = manager.machine.devices[":maincpu"]
local mem = cpu.spaces["program"]
local dbg = manager.machine.debugger
local set, n = false, 0
emu.register_periodic(function()
  if not set and manager.machine.time.seconds > 8 then
    set = true
    for _, v in ipairs({0x08, 0x0C}) do cpu.debug:bpset(mem:read_u32(v), "", "") end
  end
  if set and dbg.execution_state == "stop" and n < 3 then
    n = n + 1
    local sp = cpu.state["SP"].value
    local s = string.format("CRASH pc=%08x sp=%08x frame:", cpu.state["CURPC"].value, sp)
    for k = 0, 13 do s = s .. string.format(" %04x", mem:read_u16(sp + 2 * k)) end
    for r = 0, 7 do s = s .. string.format(" D%d=%08x", r, cpu.state["D" .. r].value) end
    for r = 0, 6 do s = s .. string.format(" A%d=%08x", r, cpu.state["A" .. r].value) end
    print(s)
    local f = io.open("crash.txt", "a"); f:write(s .. "\n"); f:close()
    dbg.execution_state = "run"
  end
end)
