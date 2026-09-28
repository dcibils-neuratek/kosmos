-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove's MIDI (`roadmap.md` 6zg): the interface PulseMusic's
-- `midi_portmidi.lua` gave its app and its Launchkey code, over
-- `/Devices/midi` rather than PortMidi through LuaJIT's FFI.
--
--   M.open()                   every device, listened to; false and M.err
--   M.names()                  the ports that play into Groove, by name
--   M.outputNames()            the ports it can send to
--   M.send(port, s, d1, d2)    one message out, to a port by its name
--   M.poll(fn)                 every event since last time:
--                              fn(kind, channel, d1, d2, port)
--   M.close()
--   M.inputs, M.last, M.err    as PulseMusic had them
--
-- **A port's name is its device's and its jack's**, "Launchkey Mini MK3
-- DAW Port", which is what PulseMusic matched on under macOS. A jack with
-- no name of its own is its cable counted from one - "Launchkey Mini MK3
-- 2" - which the Launchkey's matcher takes as Windows's second port.

local midi = use("/Kosmos/Libraries/midi.lua")

local M = { inputs = {}, err = nil, last = nil }

local outputs = {}                      -- name -> { id, cable }
local port_of = {}                      -- id * 16 + cable -> name
local stream = nil

local function port_name(device, jack, cable)
  if jack and jack ~= "" then
    -- A jack that repeats its device's name is not said twice.
    if jack:lower():find(device:lower(), 1, true) then return jack end
    return device .. " " .. jack
  end

  return device .. " " .. (cable + 1)
end

function M.close()
  if stream then stream:close() end
  stream = nil
  M.inputs, outputs, port_of = {}, {}, {}
end

function M.open()
  M.close()
  M.err = nil

  local all, why = midi.all()

  if #all == 0 then
    M.err = why or "no MIDI device"
    return why == nil
  end

  for _, d in ipairs(all) do
    for cable = 0, d.ins - 1 do
      local name = port_name(d.name, d.inputs[cable + 1], cable)

      port_of[d.id * 16 + cable] = name
      M.inputs[#M.inputs + 1] = { name = name, id = d.id, cable = cable }
    end

    for cable = 0, d.outs - 1 do
      outputs[port_name(d.name, d.outputs[cable + 1], cable)] = { id = d.id, cable = cable }
    end
  end

  local s, err = midi.open()

  if not s then
    M.err = err
    M.inputs = {}
    return false
  end

  stream = s
  return true
end

function M.names()
  local t = {}
  for _, p in ipairs(M.inputs) do t[#t + 1] = p.name end
  return t
end

function M.outputNames()
  local t = {}
  for name in pairs(outputs) do t[#t + 1] = name end
  table.sort(t)
  return t
end

function M.send(port, status, d1, d2)
  local out = outputs[port]

  if not out then return false end

  return midi.send(out.id, out.cable, status, d1 or 0, d2 or 0) == true
end

-- PulseMusic's words for what arrived: "program" is its "pc", and
-- channel and key pressure are both "touch".
local KIND = { on = "on", off = "off", cc = "cc", bend = "bend",
               program = "pc", pressure = "touch", touch = "touch" }

function M.poll(handler)
  if not stream then return 0 end

  return stream:events(function(e)
    local kind = KIND[e.kind]

    if not kind then return end

    local port = port_of[e.device * 16 + e.cable] or ("MIDI " .. e.device)

    if kind ~= "touch" then
      M.last = string.format("%s ch%d %d %d", kind, e.channel, e.d1, e.d2)
    end

    handler(kind, e.channel, e.d1, e.d2, port)
  end)
end

return M
