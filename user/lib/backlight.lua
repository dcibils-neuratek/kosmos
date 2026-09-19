-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The screen's brightness, from Lua: `/dev/backlight`, the backlight driver.
--
-- **The layout below is `backlightproto.h` written a second time**, as
-- `audio.lua` and `blocks.lua` write theirs: two words each way, asserted
-- when this loads. Levels are 0 to 256, the audio server's scale, and the
-- driver keeps a floor - a `set` of 0 comes back as the dimmest the driver
-- allows, never black.
--
-- `get` and `set` each return the level as the controller now holds it, or
-- nil and a sentence. A machine with no backlight the driver knows - QEMU,
-- or a laptop whose graphics are not Intel's - answers "no backlight".

local backlight = {}

local OP_GET, OP_SET = 1, 2

local REQUEST = "<I4I4"
local REPLY   = "<I4I4"

assert(#string.pack(REQUEST, 0, 0) == 8,
       "backlight: the request layout does not match backlightproto.h")

backlight.FULL = 256

local ERRORS = {
  [1] = "no backlight on this machine",
  [2] = "the backlight driver did not understand that",
  [3] = "the backlight did not take the level",
}

local function request(op, level)
  local reply, why = fs.raw("/dev/backlight", string.pack(REQUEST, op, level))

  if not reply then return nil, tostring(why) end

  if #reply < 8 then
    return nil, "the backlight driver sent a reply of the wrong size"
  end

  local err, now = string.unpack(REPLY, reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("backlight error " .. tostring(err))
  end

  return now
end

function backlight.get()
  return request(OP_GET, 0)
end

function backlight.set(level)
  level = math.max(0, math.min(backlight.FULL, math.floor(level)))
  return request(OP_SET, level)
end

return backlight
