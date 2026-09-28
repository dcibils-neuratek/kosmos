-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs midi
-- MIDI keyboards and controllers, at the prompt (`roadmap.md` 6zg).
--
--   midi                      every device, its id, its ports each way, and
--                             how many programs listen to it
--   midi listen [seconds]     what is played, as it is played, for ten
--                             seconds or as many as said
--   midi send ID CABLE HEX..  whole messages to a device's port:
--                             `midi send 3 1 9F 0C 7F` puts a Launchkey in
--                             its DAW mode
--   midi try ID               a note on and off, a controller, a bend and
--                             System Exclusive sent to a device's first
--                             port, and what comes back - from the virtual
--                             keyboard, all of it
--   midi play ID NOTE [CH]    once a program listens to the device, NOTE
--                             on channel CH, 1 unless said, for a tenth of
--                             a second: the virtual keyboard played into
--                             whatever is listening - `wm groove,midi:play
--                             1 36 10` is Groove's kick from the
--                             Launchkey's first pad
--
-- The smallest thing that uses `/Devices/midi` end to end (`usb.md` §12),
-- through `midi.lua`, the way `sticks` uses the block protocol. With
-- `opt/kosmos/midi=virtual` there is a virtual keyboard whose port gives
-- back what is sent to it, so `midi send` and `midi listen` can be tried
-- together on a machine with no MIDI at all.

local midi = use("/Kosmos/Libraries/midi.lua")

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local function say_event(e, hz, since)
  local when = (e.counter - since) / hz * 1000
  local what

  if e.kind == "on" or e.kind == "off" or e.kind == "touch" then
    what = ("%s channel %d note %d velocity %d"):format(e.kind, e.channel, e.d1, e.d2)
  elseif e.kind == "cc" then
    what = ("cc channel %d controller %d value %d"):format(e.channel, e.d1, e.d2)
  elseif e.kind == "bend" then
    what = ("bend channel %d value %d"):format(e.channel, e.value)
  elseif e.kind == "program" or e.kind == "pressure" then
    what = ("%s channel %d %d"):format(e.kind, e.channel, e.d1)
  else
    local hex = {}
    for i = 1, #e.bytes do hex[i] = ("%02X"):format(e.bytes:byte(i)) end
    what = e.kind .. " " .. table.concat(hex, " ")
  end

  print(("midi: %8.1f ms  device %d cable %d  %s"):format(when, e.device, e.cable, what))
end

if words[1] == nil then
  local all, why = midi.all()

  if #all == 0 then
    print("midi: no MIDI devices" .. (why and (" (" .. why .. ")") or ""))
    return
  end

  for _, d in ipairs(all) do
    print(("midi: device %d, %s, over %s: %d in (%s), %d out (%s), %d listening"):format(
          d.id, d.name, d.source, d.ins, table.concat(d.inputs, ", "),
          d.outs, table.concat(d.outputs, ", "), d.listening))
  end

  return
end

if words[1] == "listen" then
  local seconds = tonumber(words[2]) or 10
  local s, why = midi.open()

  if not s then
    print("midi: " .. tostring(why))
    return
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
  local since = sys.ticks()
  local stop = since + seconds * hz
  local heard = 0

  print(("midi: listening for %d seconds"):format(seconds))

  while sys.ticks() < stop do
    heard = heard + s:events(function(e) say_event(e, hz, since) end)
    sys.sleep(10)
  end

  s:close()
  print(("midi: %d events heard%s"):format(heard,
        s.lost > 0 and (", " .. s.lost .. " lost") or ""))
  return
end

if words[1] == "send" then
  local id, cable = tonumber(words[2]), tonumber(words[3])
  local bytes = {}

  for i = 4, #words do
    local b = tonumber(words[i], 16)

    if not b or b > 255 then
      print("midi: not a byte in hexadecimal: " .. tostring(words[i]))
      return
    end

    bytes[#bytes + 1] = string.char(b)
  end

  if not id or not cable or #bytes == 0 then
    print("midi: midi send ID CABLE HEX..")
    return
  end

  local ok, why = midi.send(id, cable, table.concat(bytes))
  print(ok and ("midi: sent %d bytes to device %d cable %d"):format(#bytes, id, cable)
           or ("midi: " .. tostring(why)))
  return
end

if words[1] == "try" then
  local id = tonumber(words[2])
  local s, why = id and midi.open(id)

  if not s then
    print("midi: " .. tostring(why or "midi try ID"))
    return
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
  local since = sys.ticks()
  local sent = { string.char(0x90, 60, 100), string.char(0x80, 60, 0),
                 string.char(0xB0, 21, 64), string.char(0xE0, 0x00, 0x60),
                 string.char(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7) }

  for _, m in ipairs(sent) do
    local ok, err = midi.send(id, 0, m)

    if not ok then print("midi: send refused: " .. tostring(err)) end
  end

  sys.sleep(20)

  local heard = s:events(function(e) say_event(e, hz, since) end)

  s:close()
  print(("midi: sent %d messages, heard %d events"):format(#sent, heard))
  return
end

if words[1] == "play" then
  local id, note, channel = tonumber(words[2]), tonumber(words[3]), tonumber(words[4] or "1")

  if not id or not note or note > 127 or not channel or channel < 1 or channel > 16 then
    print("midi: midi play ID NOTE [CHANNEL]")
    return
  end

  -- A minute for the program it plays into to start listening: the thing,
  -- rather than a guess at how long a program takes to open.
  local listening, why

  for _ = 1, 600 do
    local all
    all, why = midi.all()

    for _, d in ipairs(all) do
      if d.id == id and d.listening > 0 then listening = d.listening end
    end

    if listening then break end
    sys.sleep(25)
  end

  if not listening then
    print("midi: nothing listened to device " .. id .. (why and (" (" .. why .. ")") or ""))
    return
  end

  local on, err = midi.send(id, 0, 0x8F + channel, note, 100)
  sys.sleep(25)
  midi.send(id, 0, 0x7F + channel, note, 0)
  print(on and ("midi: played note %d on channel %d of device %d, %d listening")
                 :format(note, channel, id, listening)
           or ("midi: " .. tostring(err)))
  return
end

print("midi: midi | midi listen [seconds] | midi send ID CABLE HEX.. | midi try ID "
      .. "| midi play ID NOTE [CHANNEL]")
