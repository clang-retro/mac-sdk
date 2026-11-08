-- Drive a booted Mac: a list of {time, action, args} steps, in emulated seconds.
local script = dofile(os.getenv("MACSTEPS"))
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local function button()
  for _, p in pairs(manager.machine.ioport.ports) do
    for name, f in pairs(p.fields) do
      if name:find("Mouse Button") then return f end
    end
  end
end
local function field(n)
  for _, p in pairs(manager.machine.ioport.ports) do
    for name, f in pairs(p.fields) do if name == n then return f end end
  end
end
local btn = button()
local function moveto(x, y)
  for _, a in ipairs({0x828, 0x82C, 0x830}) do mem:write_u16(a, y); mem:write_u16(a + 2, x) end
  mem:write_u8(0x8CE, 1); mem:write_u8(0x8CF, 1)
end
local i, hold = 1, {}
emu.register_frame_done(function()
  local t = manager.machine.time.seconds
  for k, v in pairs(hold) do if t >= v.at then v.f:set_value(v.val); hold[k] = nil end end
  while i <= #script and t >= script[i][1] do
    local s = script[i]
    if s[2] == "shot" then manager.machine.video:snapshot()
    elseif s[2] == "move" then moveto(s[3], s[4])
    elseif s[2] == "down" then btn:set_value(1)
    elseif s[2] == "up" then btn:set_value(0)
    elseif s[2] == "type" then manager.machine.natkeyboard:post(s[3])
    elseif s[2] == "key" then local f = field(s[3]); f:set_value(1); table.insert(hold, {at = t + 0.2, f = f, val = 0})
    elseif s[2] == "cmd" then local f = field("Command"); f:set_value(1); table.insert(hold, {at = t + 0.5, f = f, val = 0}); manager.machine.natkeyboard:post(s[3])
    end
    i = i + 1
  end
end)
