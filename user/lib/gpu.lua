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
  [1] = "no GPU this driver knows",
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
  [15] = "no such context",
  [16] = "that binding is over another, or out of range",
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
  local which = math.type(engine) == "integer" and engine or engine == "render" and 1 or 0
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
-- **Contexts on the video engine** (`docs/gpu3d.md` G6b and G6c):
-- execlists on VCS0, `contexts` of them - one or two, submitted together -
-- each with a page table of its own mapping the same GPU address to a page
-- of its own. What the engine said, in `gpu.last_execlist`, whether it
-- worked or not: what each page holds, in `values`.
--
function gpu.execlist_test(contexts)
  local n = contexts or 1                -- 3: two sharing one page table, a control
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, 26, 0, n, 0, 0, 0, 0, 0), nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, first, second, status, ctl, mode, esr, us = string.unpack(REPLY, reply)

  gpu.last_execlist = { values = n > 1 and { first, second } or { first }, status = status,
                        shared = n == 3,
                        events = ctl >> 16, seen = ctl & 0xffff, mode = mode, esr = esr, elapsed_us = us }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_execlist
end

--
-- **A hang, caught and reset** (`docs/gpu3d.md` G6d): a batch that spins
-- for ever given to the blitter, or the render engine with "render"; the
-- driver's watch resets the engine, and a store after shows it works.
-- Answers { value, resets, reset_ms, mode, esr }, or nil and why.
function gpu.hang_test(which)
  local reply, why = fs.raw("/Devices/gpu",
    string.pack(REQUEST, 27, 0, which == "render" and 1 or 0, 0, 0, 0, 0, 0), nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, value, resets, reset_ms, _, mode, esr = string.unpack(REPLY, reply)

  gpu.last_hang = { value = value, resets = resets, reset_ms = reset_ms, mode = mode, esr = esr }

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  return gpu.last_hang
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
-- seventeen words, then a row of six for each buffer. Counters wrap at 32
-- bits; a rate is the difference of two snapshots, `gpu.since(a, b)`.
--
local STATS_HEAD = "<I4I4I4I4I4I4I4I4I4I4I4I4I4I4I4I4I4"
local STATS_ROW = "<I4I4I4I4I4I4"
local OP_STATS = 9

assert(#string.pack(STATS_HEAD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 68,
       "gpu: the stats layout does not match gpuproto.h")

-- The names of the GPUs this driver has met, by device id; the rest by id.
gpu.MODELS = {
  [0x1902] = "HD Graphics 510", [0x1912] = "HD Graphics 530", [0x191B] = "HD Graphics 530",
  [0x5912] = "HD Graphics 630", [0x591B] = "HD Graphics 630", [0x3E92] = "UHD Graphics 630",
  [0x9BC5] = "UHD Graphics 630", [0x9B41] = "UHD Graphics", [0x9A49] = "Iris Xe Graphics",
  -- The Mac Pro 2013's AMD cards (docs/amdgpu.md); no id here collides.
  [0x6798] = "FirePro D700 (Tahiti XT)", [0x679E] = "FirePro D500 (Tahiti LE)",
  [0x6810] = "FirePro D300 (Pitcairn)",
}

-- Whose card a device id is: the driver's reply has no vendor word, and
-- Southern Islands' ids are 6600h to 68FFh, which Intel has never used.
function gpu.vendor(device)
  return (device >= 0x6600 and device <= 0x68FF) and 0x1002 or 0x8086
end

gpu.KINDS = { [0] = "scanned out", [1] = "cursor plane", [2] = "shared" }

function gpu.stats()
  local reply, why = fs.raw("/Devices/gpu", string.pack(REQUEST, OP_STATS, 0, 0, 0, 0, 0, 0, 0),
                            nil, "gpu")

  if not reply then return nil, tostring(why) end
  if #reply < 68 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local err, device, width, height, mhz, max_mhz, min_mhz, rpstat, rc6, flips, frames,
        underrun, desktop, shown, n, blitter_us, render_us = string.unpack(STATS_HEAD, reply)

  if err ~= 0 then return nil, gpu.ERRORS[err] or ("gpu error " .. err), err end

  local s = { device = device, model = gpu.MODELS[device] or ("GPU %04x:%04x"):format(gpu.vendor(device), device),
              width = width, height = height, mhz = mhz, max_mhz = max_mhz, min_mhz = min_mhz,
              rpstat = rpstat, rc6 = rc6, flips = flips, frames = frames,
              underrun = underrun ~= 0, cursor = desktop & 1 ~= 0, page_flips = desktop & 2 ~= 0,
              shown = shown ~= 0xffffffff and shown or nil, buffers = {}, ticks = sys.ticks(),
              blitter_us = blitter_us, render_us = render_us }

  for i = 1, math.min(n, (#reply - 68) // 24) do
    local id, holder, w, h, pages, kind = string.unpack(STATS_ROW, reply, 68 + (i - 1) * 24 + 1)

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
           -- The blitter's share of the time, by its own clock (G3); and
           -- the render engine's, the programs' runs - a compute kernel's.
           blitter = math.min(1, d(a.blitter_us or 0, b.blitter_us or 0) / 1e6 / seconds),
           render = math.min(1, d(a.render_us or 0, b.render_us or 0) / 1e6 / seconds) }
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

--
-- **Programs' own contexts** (`docs/gpu3d.md` §5.3, G6e) - the 3D half,
-- `gpuproto.h`'s `struct gpu_ctx_request` written a second time: a context
-- with a page table of its own, regions bound in it at addresses the
-- caller chooses, batches run in it in order, and a timeline whose value
-- reached is in a fence page the caller reads without asking - or is
-- pulsed when it comes. Where there is no Intel GPU the driver's stand-in
-- runs a batch's MI_ commands on the processor.
--
--   local c = gpu.context("render")          -- or "copy", the blitter
--   c:bind(0x100000000, region)             -- all its pages, there
--   c:exec(0x200000, bytes, 1)              -- a batch, signalling 1
--   c:wait(1, endpoint, code)               -- a pulse when 1 is reached
--   c:reached(), c:state(), c:unbind(at), c:forget()
--
local CTX = "<I4I4I8I4I4I4I4I8" .. ("I4I4I8"):rep(4)

assert(#string.pack(CTX, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 104,
       "gpu: the context request does not match gpuproto.h")

gpu.ENGINES = { render = 0, copy = 1 }
gpu.CACHES = { wb = 0, uc = 1, wb_ellc = 2 }

local function ctx_call(op, f, pass)
  local args = { op, f.context or 0, f.gpu or 0, f.first_page or 0, f.pages or 0, f.flags or 0,
                 f.engine or 0, f.signal or 0 }

  for i = 1, 4 do
    local w = (f.waits or {})[i] or {}

    args[#args + 1] = w.context or 0
    args[#args + 1] = 0
    args[#args + 1] = w.value or 0
  end

  local reply, cap = fs.raw("/Devices/gpu", string.pack(CTX, table.unpack(args)), pass, "gpu")

  if not reply then return nil, tostring(cap) end
  if #reply < 32 then return nil, "the GPU's driver sent a reply of the wrong size" end

  local r = { string.unpack(REPLY, reply) }

  if r[1] ~= 0 then return nil, gpu.ERRORS[r[1]] or ("gpu error " .. tostring(r[1])), r[1] end

  return r, cap
end

local Context = {}
Context.__index = Context

function gpu.context(engine)
  local r, cap = ctx_call(28, { engine = gpu.ENGINES[engine or "render"] or 0 })

  if not r then return nil, cap end

  return setmetatable({ id = r[2], fence = cap, engine = engine or "render" }, Context)
end

-- The region's pages `first_page` on, `pages` of them (nil: the rest), at
-- `at`; `cache` one of gpu.CACHES' names, "wb" by default.
function Context:bind(at, region, first_page, pages, cache)
  local r, why, code = ctx_call(30, { context = self.id, gpu = at, first_page = first_page or 0,
                                      pages = pages or 0, flags = gpu.CACHES[cache or "wb"] or 0 },
                                region)

  if not r then return nil, why, code end
  return true
end

function Context:unbind(at)
  local r, why, code = ctx_call(31, { context = self.id, gpu = at })

  if not r then return nil, why, code end
  return true
end

-- A batch of `bytes` at `at`, after `waits` - a list of { context = c,
-- value = v } - signalling `signal`. Answered at once.
function Context:exec(at, bytes, signal, waits)
  local w = {}

  for i, x in ipairs(waits or {}) do
    w[i] = { context = type(x.context) == "table" and x.context.id or x.context, value = x.value }
  end

  local r, why, code = ctx_call(32, { context = self.id, gpu = at, pages = bytes, signal = signal,
                                      flags = #w, waits = w })

  if not r then return nil, why, code end
  return r[2] | r[3] << 32
end

-- A pulse of `code` to `endpoint` once the timeline reaches `value`.
function Context:wait(value, endpoint, code)
  local r, why, err = ctx_call(33, { context = self.id, signal = value, flags = code or 0 }, endpoint)

  if not r then return nil, why, err end
  return true
end

-- The value reached, read from the fence page: nobody asked.
function Context:reached()
  return sys.region_load32(self.fence, 0) | sys.region_load32(self.fence, 4) << 32
end

function Context:state()
  local r, why, code = ctx_call(34, { context = self.id })

  if not r then return nil, why, code end
  return { reached = r[2] | r[3] << 32, given = r[4] | r[5] << 32, hangs = r[6], faults = r[7],
           fault_word = r[8] }
end

function Context:forget()
  local r, why, code = ctx_call(29, { context = self.id })

  if not r then return nil, why, code end
  return true
end

return gpu
