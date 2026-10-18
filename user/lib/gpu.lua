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
  [12] = "the engine is busy with what it was given before",
  [13] = "the kernel was refused",
  [14] = "that kernel or buffer is another program's",
}

-- Why a kernel was refused, GPU_KERNEL_* in gpuproto.h.
gpu.KERNEL_WHY = {
  [1] = "it is empty, too large, or not whole instructions",
  [2] = "it is compacted, which the driver does not read",
  [3] = "it sets a breakpoint",
  [4] = "the instruction is not one a kernel may use",
  [5] = "it addresses a register through a0",
  [6] = "it names a register a kernel may not use",
  [7] = "it sends to a unit a kernel may not use",
  [8] = "it sends a message or an address the unit may not be sent - only buffers 0 to 7",
  [9] = "its send's descriptor is in a register",
  [10] = "it branches outside the kernel, or through a register",
  [11] = "the last instruction does not end the thread",
  [12] = "a thread's end is predicated, or not from r112-r127 to the spawner",
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
-- whether it worked or not. `engine` "render" asks the render engine
-- instead (step 7a), its ring at 0x2000 and its fence a PIPE_CONTROL - the
-- request's `x`, 1 for it.
--
function gpu.engine_test(engine)
  local which = engine == "render" and 1 or 0
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 10, 0, which, 0, 0, 0, 0, 0), nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, value, head, tail, ctl, mode, esr, us = string.unpack(REPLY, reply)

  gpu.last_engine = { value = value, head = head, tail = tail, ctl = ctl, mode = mode, esr = esr,
                      elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_engine
end

--
-- **The first program on the execution units** (`m700-2d.md` 7b): the
-- render engine runs a kernel that fills a 64 by 4 surface with `colour`,
-- a byte. Answers { right, past, elapsed_us, ... } - how many of the 256
-- bytes are the colour and how many after them were written - or nil, the
-- driver's sentence and that, in `gpu.last_compute`, as far as it got.
--
-- **The render engine's starting state** (`m700-2d.md` 7c): i915's batch
-- for a new context, run once before any 3D work. True, or nil and why;
-- the registers after in `gpu.last_state`.
function gpu.render_state()
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 16, 0, 0, 0, 0, 0, 0, 0), nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, fence, head, tail, _, mode, esr, us = string.unpack(REPLY, reply)

  gpu.last_state = { fence = fence, head = head, tail = tail, mode = mode, esr = esr, elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_state
end

-- **A texture drawn by the 3D pipeline** (`m700-2d.md` 7c): a 64 by 64
-- picture sampled by the render engine into another and held to it pixel
-- for pixel. { right, past, elapsed_us, ... }, or nil and why; the registers
-- after in `gpu.last_render`. `blend` lays it over an opaque ground by its
-- alpha instead, as the compositor lays a window: `right` the pixels within
-- one of the compositor's arithmetic in every channel, `exact` those equal.
function gpu.render_test(blend)
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 17, 0, blend and 1 or 0, 0, 0, 0, 0, 0),
                            nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, right, head, tail, past, exact, esr, us = string.unpack(REPLY, reply)

  gpu.last_render = { right = right, exact = exact, past = past, head = head, tail = tail, esr = esr,
                      elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_render
end

-- **A window at a window's proportions** (`m700-2d.md` 7c): a part of a
-- 301 by 203 picture blended onto an 800 by 600 target at (123, 77), every
-- pixel of the target held to the compositor. { right, exact, outside,
-- elapsed_us, ... } or nil and why, in `gpu.last_draw` either way.
function gpu.draw_test(big)
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 18, 0, big and 1 or 0, 0, 0, 0, 0, 0), nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, right, head, tail, outside, exact, esr, us = string.unpack(REPLY, reply)

  gpu.last_draw = { right = right, exact = exact, outside = outside, head = head, tail = tail, esr = esr,
                    elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_draw
end

function gpu.compute_test(colour)
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 15, 0, (colour or 0x5a) & 0xff, 0, 0, 0, 0, 0),
                            nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, right, head, tail, past, mode, esr, us = string.unpack(REPLY, reply)

  gpu.last_compute = { right = right, past = past, head = head, tail = tail, mode = mode, esr = esr,
                       elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_compute
end

--
-- **A program's own kernels** (`docs/compute.md` C2): the door the Compute
-- Kit stands on. Each answers what it says, or nil, a sentence and the
-- driver's code.
--
-- `import(cap, width, height, pitch, offset)` enters a region in the GPU's
-- table - `width` by `height` four-byte pixels at `pitch` - and answers its
-- buffer id; the driver keeps a copy of the capability until `forget(id)`.
--
function gpu.import(cap, width, height, pitch, offset)
  local r, why, code = request(11, { x = width, y = height, pitch = pitch, offset = offset or 0 }, cap)

  return r and r.curcntr, why, code     -- buffer.id is the reply's first word
end

function gpu.forget(id)
  local r, why, code = request(14, { x = id })
  return r and true, why, code
end

--
-- `kernel(code, lines)`: a kernel's bytes - `eu.assemble`'s, and its lines,
-- so a refusal says the line - copied into a region and handed over. The
-- driver checks every instruction (decision D2) and answers the kernel's id;
-- refused, nil and "line N: why", or the instruction's number with no lines.
--
local regions

function gpu.kernel(code, lines)
  regions = regions or use("/Kosmos/Libraries/regions.lua")

  local r, why = regions.unmapped(#code)

  if not r then return nil, why end

  sys.region_write(r.cap, 0, code)

  local reply, err = fs.raw("/Devices/gpu", string.pack(REQUEST, 21, 0, #code, 0, 0, 0, 0, 0), r.cap, "gpu")

  regions.free(r)

  if not reply then return nil, tostring(err) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local e, id, at, kwhy = string.unpack("<I4I4I4I4", reply)

  if e == 13 then
    local where = lines and lines[at + 1] and ("line " .. lines[at + 1])
                  or ("instruction " .. at)
    return nil, ("%s: %s"):format(where, gpu.KERNEL_WHY[kwhy] or ("refused, " .. kwhy)), e, at, kwhy
  end

  if e ~= 0 then return nil, gpu.ERRORS[e] or ("gpu error " .. e), e end

  return id
end

function gpu.forget_kernel(id)
  local r, why, code = request(22, { x = id })
  return r and true, why, code
end

--
-- `run{ kernel, simd, groups_x, groups_y, buffers = { {id, view}, ... },
-- constants = "bytes" }`: the kernel over the grid, answered at once with
-- the render engine's fence; `fence()` says how far it has got. Views:
-- gpu.BYTES, BGRA, R8, R32F, as `GPU_VIEW_*`. Constants are whole registers
-- of 32 bytes, padded here.
--
gpu.BYTES, gpu.BGRA, gpu.R8, gpu.R32F = 0, 1, 2, 3

local RUN_HEAD = "<I4I4I4I4I4I4"

function gpu.run(q)
  local b = q.buffers or {}
  local c = q.constants or ""

  if #b > 8 then return nil, "a run binds eight buffers at most" end
  if #c > 256 then return nil, "a run's constants are 256 bytes at most" end

  c = c .. string.rep("\0", (32 - #c % 32) % 32)

  local parts = { string.pack(RUN_HEAD, 23, q.kernel, q.simd or 16, q.groups_x, q.groups_y, #b) }

  for i = 1, 8 do
    parts[#parts + 1] = string.pack("<I4I4", b[i] and b[i][1] or 0, b[i] and (b[i][2] or 0) or 0)
  end

  parts[#parts + 1] = string.pack("<I4", #c) .. c .. string.rep("\0", 256 - #c)

  local msg = table.concat(parts)

  assert(#msg == 344, "gpu: the run layout does not match gpuproto.h")

  local reply, err = fs.raw("/Devices/gpu", msg, nil, "gpu")

  if not reply then return nil, tostring(err) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local e, fence = string.unpack("<I4I4", reply)

  if e ~= 0 then return nil, gpu.ERRORS[e] or ("gpu error " .. e), e end

  return fence
end

-- How far the render engine has got: its fence, and the last asked for.
function gpu.fence()
  local r, why = request(13, { x = 1 })

  if not r then return nil, why end

  return r.curcntr, r.curpos    -- engine.value, and engine.tail: the last issued
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
