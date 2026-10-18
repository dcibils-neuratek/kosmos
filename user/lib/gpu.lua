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
  [8] = "no such buffer, or not the screen's size",
  [9] = "the GPU did not wake (forcewake was not acknowledged)",
  [10] = "the blitter's ring would not start",
  [11] = "the blitter was given the command and its number never came",
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

--
-- **The blitter's first work** (`m700-2d.md` step 5): the driver wakes the
-- GPU, gives its blit engine a ring, puts one command in it and looks for
-- the number it writes. The registers as left, in `gpu.last_engine`,
-- whether it worked or not.
--
function gpu.engine_test()
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 10, 0, 0, 0, 0, 0, 0, 0), nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, value, head, tail, ctl, mode, esr, us = string.unpack(REPLY, reply)

  gpu.last_engine = { value = value, head = head, tail = tail, ctl = ctl, mode = mode, esr = esr,
                      elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_engine
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

--
-- **What the GPU is doing** (`docs/gpumonitor.md`, G1): one snapshot of its
-- counters and its buffers, as `gpuproto.h`'s `struct gpu_stats` says -
-- sixteen words, then a row of six for each buffer. Counters wrap at 32
-- bits; a rate is the difference of two snapshots, `gpu.since(a, b)`.
--
local STATS_HEAD = "<I4I4I4I4I4I4I4I4I4I4I4I4I4I4I4I4"
local STATS_ROW = "<I4I4I4I4I4I4"
local OP_STATS = 9

assert(#string.pack(STATS_HEAD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 64,
       "gpu: the stats layout does not match gpuproto.h")

-- The names of the GPUs this driver has met, by device id; the rest by id.
gpu.MODELS = {
  [0x1902] = "HD Graphics 510", [0x1912] = "HD Graphics 530", [0x191B] = "HD Graphics 530",
  [0x5912] = "HD Graphics 630", [0x591B] = "HD Graphics 630", [0x3E92] = "UHD Graphics 630",
  [0x9BC5] = "UHD Graphics 630", [0x9B41] = "UHD Graphics", [0x9A49] = "Iris Xe Graphics",
}

gpu.KINDS = { [0] = "scanned out", [1] = "cursor plane", [2] = "back buffer, shared" }

function gpu.stats()
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, OP_STATS, 0, 0, 0, 0, 0, 0, 0),
                            nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 64 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, device, width, height, mhz, max_mhz, min_mhz, rpstat, rc6, flips, frames,
        underrun, desktop, shown, n, blitter_us = string.unpack(STATS_HEAD, reply)

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  local s = { device = device, model = gpu.MODELS[device] or ("Intel GPU 8086:%04x"):format(device),
              width = width, height = height, mhz = mhz, max_mhz = max_mhz, min_mhz = min_mhz,
              rpstat = rpstat, rc6 = rc6, flips = flips, frames = frames,
              underrun = underrun ~= 0, cursor = desktop & 1 ~= 0, page_flips = desktop & 2 ~= 0,
              shown = shown ~= 0xffffffff and shown or nil, buffers = {}, ticks = sys.ticks(),
              blitter_us = blitter_us }

  for i = 1, math.min(n, (#reply - 64) // 24) do
    local id, holder, w, h, pages, kind = string.unpack(STATS_ROW, reply, 64 + (i - 1) * 24 + 1)

    s.buffers[i] = { id = id ~= 0xffffffff and id or nil, holder = holder, width = w, height = h,
                     bytes = pages * 4096, kind = gpu.KINDS[kind] or ("kind " .. kind) }
  end

  return s
end

-- Two snapshots' difference, as rates a second: flips, frames (the
-- refresh, measured) and the share of the time asleep in RC6.
function gpu.since(a, b, counter_hz)
  local seconds = (b.ticks - a.ticks) / counter_hz
  local function d(x, y) return (y - x) & 0xffffffff end

  if seconds <= 0 then return nil end

  return { seconds = seconds,
           flips = d(a.flips, b.flips) / seconds,
           frames = d(a.frames, b.frames) / seconds,
           asleep = math.min(1, d(a.rc6, b.rc6) * 1280e-9 / seconds),
           -- The blitter's share of the time, by its own clock (G3).
           blitter = math.min(1, d(a.blitter_us or 0, b.blitter_us or 0) / 1e6 / seconds) }
end

-- What the desktop is drawn with, in the words of its label: nil when
-- there is no GPU this driver knows.
function gpu.desktop_words(s)
  local parts = {}

  if s.cursor then parts[#parts + 1] = "cursor plane" end
  if s.page_flips then parts[#parts + 1] = "page flips" end

  return s.model:gsub("^U?HD Graphics ", "HD "):gsub("^Iris Xe Graphics", "Iris Xe")
         .. " - " .. (#parts > 0 and table.concat(parts, ", ") or "software")
end

return gpu
