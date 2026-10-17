-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The GPU's display engine, from Lua: `/Devices/gpu`, the Intel GPU's
-- driver (`docs/m700-2d.md`). The one door to it.
--
-- **The layout below is `gpuproto.h` written a second time**, as
-- `backlight.lua` writes its own: eight words each way, asserted when this
-- loads.
--
-- `cursor(surface, hot_x, hot_y, x, y)` hands the cursor plane a 64 by 64
-- shared surface of 0xAARRGGBB, by its region - control by message, data by
-- shared memory - and the driver copies it, so the surface may be freed
-- after. `cursor_move(x, y)` is then only where; `cursor_hide()` takes it
-- away. Each returns true, or nil and a sentence; a machine with no display
-- engine the driver knows - QEMU - answers "no Intel GPU".

local gpu = {}

local OP_LOOK, OP_CURSOR_TEST = 1, 2
local OP_CURSOR_SET, OP_CURSOR_MOVE, OP_CURSOR_HIDE = 3, 4, 5

local REQUEST = "<I4I4i4i4I4I4I4I4"
local REPLY   = "<I4I4I4I4I4I4I4I4"

assert(#string.pack(REQUEST, 0, 0, 0, 0, 0, 0, 0, 0) == 32,
       "gpu: the request layout does not match gpuproto.h")

gpu.ERRORS = {
  [1] = "no Intel GPU this driver knows",
  [2] = "a request the driver does not know",
  [3] = "no pipe running to show it on",
  [4] = "no contiguous pages for the picture",
  [5] = "the GTT slots chosen could not be written",
  [6] = "written, and read back different",
  [7] = "the cursor plane is in use",
}

-- The reply's eight words, after the error: the test's read-back.
local function request(op, f, pass)
  local reply, why = fs.raw("/Devices/gpu",
                            string.pack(REQUEST, op, f.seconds or 0, f.x or 0, f.y or 0,
                                        f.hot_x or 0, f.hot_y or 0, f.offset or 0,
                                        f.pitch or 0),
                            pass, "gpu")

  if not reply then return nil, tostring(why) end

  if #reply < 32 then
    return nil, "the GPU's driver sent a reply of the wrong size"
  end

  local err, cntr, base, pos, buf, wm0, before, after = string.unpack(REPLY, reply)

  if err ~= 0 then
    return nil, gpu.ERRORS[err] or ("gpu error " .. tostring(err)), err
  end

  return { curcntr = cntr, curbase = base, curpos = pos, buf_cfg = buf, wm0 = wm0,
           underrun_before = before ~= 0, underrun_after = after ~= 0 }
end

function gpu.look()
  local r, why = request(OP_LOOK, {})
  return r and true, why
end

-- The test of `testing.md` 18.659: the driver's own picture for `seconds`.
-- Returns the registers as read back, or nil, a sentence and the code.
function gpu.cursor_test(seconds)
  return request(OP_CURSOR_TEST, { seconds = seconds })
end

function gpu.cursor(surface, hot_x, hot_y, x, y)
  local cap, offset, pitch = surface:region()

  if not cap then return nil, "the cursor's picture is not a shared surface" end

  local r, why = request(OP_CURSOR_SET, { x = x, y = y, hot_x = hot_x, hot_y = hot_y,
                                          offset = offset, pitch = pitch }, cap)
  return r and true, why
end

function gpu.cursor_move(x, y)
  local r, why = request(OP_CURSOR_MOVE, { x = x, y = y })
  return r and true, why
end

function gpu.cursor_hide()
  local r, why = request(OP_CURSOR_HIDE, {})
  return r and true, why
end

return gpu
