-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- MIDI keyboards and controllers, as a program sees them (`roadmap.md` 6zg,
-- `usb.md` §12).
--
--   local midi = use("/Kosmos/Libraries/midi.lua")
--   local all = midi.all()                  -- every device: id, name, ports
--   local s = midi.open()                   -- every device's events;
--                                           -- midi.open(id) for one
--   s:events(function(e) ... end)           -- the events since last time
--   midi.send(id, cable, 0x90, 60, 100)     -- a message to a device's port
--   s:close()
--
-- `/Devices/midi` speaks a declared struct (`midiproto.h`), and this is the
-- one place in Lua that knows its shape, as `camera.lua` is for the camera.
-- Only a program that declares `kosmos: needs midi` has `/Devices/midi` at
-- all; for any other `midi.all()` is empty and says why.
--
-- **Events arrive in a page of this program's**, which the driver writes as
-- they come: `s:events` reads what arrived since it last looked - so a
-- program that draws takes its notes once a frame, and never waits for one.
-- An event is a table:
--
--   kind      "on", "off", "cc", "bend", "program", "pressure", "touch",
--             "sysex", or "system" for the rest (clock, start, stop)
--   channel   1 to 16, for the channel messages
--   d1, d2    the data bytes: a note and its velocity, a controller and its
--             value; `bend` is also `value`, -8192 to 8191
--   cable     the device's port it came on - on the Launchkey, 0 its keys
--             and 1 its DAW controls
--   device    its id, as `midi.all` names it
--   counter   the counter when the driver took it: *when* it was played
--   bytes     the MIDI bytes themselves
--
-- A note on with velocity zero is a note off, as MIDI says, and comes as one.

local midi = {}

local regions = use("/Kosmos/Libraries/regions.lua")

-- struct midi_request: op, index, device, handle, cable, length, reserved, bytes
local REQUEST  = "<I4I4I4I4BBc6c40"
local SEND_MAX = 40

-- struct midi_reply: error, devices, id, handle, ins, outs, source, listening,
-- name, four in names, four out names
local REPLY      = "<I4I4I4I4BBBBc40"
local REPLY_SIZE = 380
local NAME       = 40
local NAMES_AT   = 21 + NAME                  -- the first in name, 1-based

-- The ring: write, read, slots, reserved, then events of sixteen bytes.
local RING_WRITE, RING_READ, RING_EVENTS = 0, 4, 16
local EVENT, EVENT_BYTES, SLOTS = "<I8I2BBBc3", 16, 255

local OP = { list = 1, open = 2, close = 3, send = 4 }
local EVERY = 0xFFFFFFFF

local ERRORS = {
  [1] = "there is no MIDI device with that number",
  [2] = "the MIDI driver did not understand that",
  [3] = "the page for its events was refused",
  [4] = "as many programs are listening as can",
  [5] = "those bytes are not whole MIDI messages",
  [6] = "the device did not take them",
  [7] = "the device has no port on that cable",
}

local SOURCES = { [1] = "usb", [2] = "virtual" }

local function pad(s, n)
  s = s or ""
  return #s >= n and s:sub(1, n) or (s .. string.rep("\0", n - #s))
end

local function ask(op, fields, pass)
  local ok, reply, why = pcall(fs.raw, "/Devices/midi",
                               string.pack(REQUEST, op, fields.index or 0,
                                           fields.device or 0, fields.handle or 0,
                                           fields.cable or 0, #(fields.bytes or ""),
                                           pad("", 6), pad(fields.bytes, SEND_MAX)),
                               pass, "midi")

  if not ok or not reply then
    return nil, tostring(ok and why or reply)
  end

  if #reply < REPLY_SIZE then
    return nil, "the MIDI driver sent a reply of the wrong size"
  end

  local err = string.unpack("<I4", reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("MIDI error " .. tostring(err))
  end

  return reply
end

local function cstring(s)
  return (s:match("^[^%z]*"))
end

--
-- The device at `index` (from 0): its id, name, where its events come from,
-- its ports each way by name, and how many programs listen to it - and how
-- many devices there are.
--
function midi.list(index)
  local reply, why = ask(OP.list, { index = index })

  if not reply then return nil, why end

  local _, devices, id, _, ins, outs, source, listening, name = string.unpack(REPLY, reply)
  local inputs, outputs = {}, {}

  for i = 1, math.min(ins, 4) do
    inputs[i] = cstring(reply:sub(NAMES_AT + (i - 1) * NAME, NAMES_AT + i * NAME - 1))
  end

  for i = 1, math.min(outs, 4) do
    local at = NAMES_AT + 4 * NAME + (i - 1) * NAME
    outputs[i] = cstring(reply:sub(at, at + NAME - 1))
  end

  return { index = index, devices = devices, id = id, name = cstring(name),
           source = SOURCES[source] or "other", ins = ins, outs = outs,
           inputs = inputs, outputs = outputs, listening = listening }
end

-- Every device there is; an empty list and why when there are none.
function midi.all()
  local first, why = midi.list(0)

  if not first then return {}, why end

  local out = { first }

  for i = 1, first.devices - 1 do
    local d = midi.list(i)

    if d then out[#out + 1] = d end
  end

  return out
end

--
-- Whole MIDI messages to device `id`'s port on `cable`: a status byte and
-- its data as numbers, or the bytes as a string - several messages at once
-- if they are all whole. True, or nil and why.
--
function midi.send(id, cable, first, ...)
  local bytes = type(first) == "string" and first or string.char(first, ...)

  if #bytes == 0 or #bytes > SEND_MAX then
    return nil, "a message of 1 to " .. SEND_MAX .. " bytes"
  end

  local reply, why = ask(OP.send, { device = id, cable = cable, bytes = bytes })

  if not reply then return nil, why end

  return true
end

--------------------------------------------------------------------------
-- Listening.
--------------------------------------------------------------------------

local stream = {}
stream.__index = stream

local KINDS = { [0x8] = "off", [0x9] = "on", [0xA] = "touch", [0xB] = "cc",
                [0xC] = "program", [0xD] = "pressure", [0xE] = "bend" }

-- What a table of bytes says, in the words above.
local function event_of(counter, device, cable, length, flags, raw)
  local b0, b1, b2 = raw:byte(1, 3)
  local e = { counter = counter, device = device, cable = cable,
              bytes = raw:sub(1, length) }

  if flags & 1 ~= 0 then
    e.kind = "sysex"
  elseif b0 >= 0x80 and b0 < 0xF0 then
    e.kind = KINDS[b0 >> 4]
    e.channel = (b0 & 0x0F) + 1
    e.d1 = length > 1 and b1 or 0
    e.d2 = length > 2 and b2 or 0

    if e.kind == "on" and e.d2 == 0 then e.kind = "off" end
    if e.kind == "bend" then e.value = e.d1 + e.d2 * 128 - 8192 end
  else
    e.kind = "system"
    e.status = b0
  end

  return e
end

--
-- Listen to device `id`, or to every device when it is nil: a page of this
-- program's, handed to the driver, which writes each event into it.
--
function midi.open(id)
  local page, why = regions.make(regions.PAGE)

  if not page then
    return nil, "no memory for the MIDI events: " .. tostring(why)
  end

  local reply, err = ask(OP.open, { device = id or EVERY }, page.cap)

  if not reply then
    regions.free(page)
    return nil, err
  end

  local handle = string.unpack("<I4", reply, 13)

  -- `at` is where the page is in this process, for C in the same process
  -- to read it itself - the Synth Kit takes a keyboard's notes that way
  -- (`synth.listen`). What reads it writes its `read`, and so a stream
  -- handed to C is not read here as well.
  return setmetatable({ page = page, cap = page.cap, handle = handle,
                        device = id, read = 0, lost = 0, at = page.at }, stream)
end

--
-- Every event since the last call, oldest first, each handed to `fn`; the
-- answer is how many. Events more than a ring behind are gone, and counted
-- in `s.lost`.
--
function stream:events(fn)
  if self.closed then return 0 end

  local write = sys.region_load32(self.cap, RING_WRITE)

  if not write then return 0 end

  local waiting = (write - self.read) & 0xFFFFFFFF

  if waiting > SLOTS then
    self.lost = self.lost + waiting - SLOTS
    self.read = (write - SLOTS) & 0xFFFFFFFF
    waiting = SLOTS
  end

  if waiting == 0 then return 0 end

  -- Read in one piece, or two where the ring wraps.
  local first = self.read % SLOTS
  local run = math.min(waiting, SLOTS - first)
  local raw = sys.region_read(self.cap, RING_EVENTS + first * EVENT_BYTES,
                              run * EVENT_BYTES)

  if run < waiting then
    raw = raw .. sys.region_read(self.cap, RING_EVENTS,
                                 (waiting - run) * EVENT_BYTES)
  end

  self.read = write
  sys.region_write(self.cap, RING_READ, string.pack("<I4", write))

  for i = 0, waiting - 1 do
    local counter, device, cable, length, flags, bytes =
      string.unpack(EVENT, raw, i * EVENT_BYTES + 1)

    fn(event_of(counter, device, cable, length, flags, bytes))
  end

  return waiting
end

function stream:close()
  if self.closed then return end

  self.closed = true
  ask(OP.close, { handle = self.handle })
  regions.free(self.page)
end

return midi
