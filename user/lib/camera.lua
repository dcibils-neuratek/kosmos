-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A camera, as a program sees one (`roadmap.md` 6d, `usb.md` §11 8d).
--
--   local camera = use("/lib/camera.lua")
--   local all = camera.all()                  -- every camera, and its sizes
--   local s = camera.open(0, all[1].sizes[1]) -- one camera, at one size
--   s:draw(surface, true)                     -- the newest frame, mirrored
--   s:close()
--
-- `/dev/camera` speaks a declared struct (`cameraproto.h`), and this is the
-- one place in Lua that knows its shape - the same bargain `/lib/audio.lua`
-- makes for `/dev/audio`. Only a program that declares `kosmos: needs
-- camera` has `/dev/camera` at all; for any other `camera.all()` is empty
-- and says why.
--
-- **No picture passes through Lua.** The program makes a region, hands it
-- to the driver with OPEN, and `surface:camera` - in C, in the graphics kit
-- - takes the newest whole frame from it into a surface. What this file
-- holds is the region's capability and its address, never its bytes.

local camera = {}

-- struct camera_request: op, camera, size, handle
local REQUEST  = "<I4I4I4I4"
local REQ_SIZE = 16

-- struct camera_reply, as far as its sizes: error, cameras, name[40], sizes
local HEAD      = "<I4I4c40I4"
local HEAD_SIZE = 52

-- struct camera_size: width, height, pixels, fps, reserved, slot_bytes
local SIZE       = "<I2I2BBI2I4"
local SIZE_BYTES = 12

-- struct camera_reply's handle, after the header and 48 sizes: 1-based.
local HANDLE_AT = HEAD_SIZE + 48 * SIZE_BYTES + 1

-- And where the frames come from, after the handle (`CAMERA_SOURCE_*`).
local SOURCE_AT = HANDLE_AT + 4
local SOURCES   = { [1] = "usb", [2] = "pattern" }

-- The region: a page of header, then three frames (`cameraproto.h`).
local RING_DATA = 4096
local SLOTS     = 3
local PAGE      = 4096

local OP = { list = 1, open = 2, close = 3 }

assert(#string.pack(REQUEST, 0, 0, 0, 0) == REQ_SIZE,
       "camera: the request layout does not match cameraproto.h")
assert(#string.pack(SIZE, 0, 0, 0, 0, 0, 0) == SIZE_BYTES,
       "camera: the size layout does not match cameraproto.h")

-- An error is a number on the wire and a sentence here.
local ERRORS = {
  [1] = "there is no camera with that number",
  [2] = "the camera does not offer that size",
  [3] = "the region for its frames was refused",
  [4] = "another window is using the camera",
  [5] = "the camera would not start",
  [6] = "the camera's driver did not understand that",
}

camera.IN_USE = ERRORS[4]

local PIXELS = { [1] = "yuy2", [2] = "mjpeg" }

local function ask(op, which, size, pass, handle)
  local ok, reply, why = pcall(fs.raw, "/dev/camera",
                               string.pack(REQUEST, op, which or 0,
                                           size or 0, handle or 0), pass)

  if not ok or not reply then
    return nil, tostring(ok and why or reply)
  end

  if #reply < HEAD_SIZE then
    return nil, "the camera's driver sent a reply of the wrong size"
  end

  local err, cameras, name, count = string.unpack(HEAD, reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("camera error " .. tostring(err))
  end

  return reply, cameras, (name:match("^[^%z]*")), count
end

--
-- One camera: its name and every size it can send, in the order it listed
-- them. `cameras` is how many there are, which is how a caller finds the
-- rest.
--
function camera.list(which)
  local reply, cameras, name, count = ask(OP.list, which)

  if not reply then return nil, cameras end

  local sizes = {}

  for i = 1, count do
    local at = HEAD_SIZE + (i - 1) * SIZE_BYTES + 1

    if at + SIZE_BYTES - 1 > #reply then break end

    local w, h, pixels, fps, _, slot = string.unpack(SIZE, reply, at)

    sizes[#sizes + 1] = { index = i - 1, width = w, height = h,
                          pixels = PIXELS[pixels] or "other", fps = fps,
                          slot_bytes = slot }
  end

  local source = nil

  if #reply >= SOURCE_AT + 3 then
    source = SOURCES[string.unpack("<I4", reply, SOURCE_AT)]
  end

  return { index = which, cameras = cameras, name = name, sizes = sizes,
           source = source }
end

-- Every camera there is; an empty list and why when there are none.
function camera.all()
  local first, why = camera.list(0)

  if not first then return {}, why end

  local out = { first }

  for i = 1, first.cameras - 1 do
    local c = camera.list(i)

    if c then out[#out + 1] = c end
  end

  return out
end

--
-- The size of `cam` nearest `width` by `height` that this can show: the
-- exact one, or else the largest inside it, or else the smallest there is.
-- YUY2 only for now: MJPEG needs the JPEG decoder between the camera and
-- the screen, which is not wired to this yet.
--
function camera.pick(cam, width, height)
  local best, smallest

  for _, s in ipairs(cam.sizes) do
    if s.pixels == "yuy2" then
      if s.width == width and s.height == height then return s end

      if s.width <= width and s.height <= height
         and (not best or s.width * s.height > best.width * best.height) then
        best = s
      end

      if not smallest or s.width * s.height < smallest.width * smallest.height then
        smallest = s
      end
    end
  end

  return best or smallest
end

local stream = {}
stream.__index = stream

--
-- Open camera `which` at `size` (an entry of its `sizes`): a region big
-- enough for three of its frames, mapped here and handed to the driver.
--
function camera.open(which, size)
  local pages = (RING_DATA + SLOTS * size.slot_bytes + PAGE - 1) // PAGE
  local cap, why = sys.memory(pages)

  if not cap then
    return nil, "no memory for the camera's frames: " .. tostring(why)
  end

  local at = sys.memory_map(cap)

  if not at then
    sys.release(cap)
    return nil, "the camera's frames could not be mapped"
  end

  local reply, err = ask(OP.open, which, size.index, cap)

  if not reply then
    sys.release(cap)
    return nil, err
  end

  local handle = #reply >= HANDLE_AT + 3
                 and string.unpack("<I4", reply, HANDLE_AT) or 0

  return setmetatable({ cap = cap, at = at, width = size.width,
                        height = size.height, size = size, which = which,
                        handle = handle, last = 0, frames = 0 }, stream)
end

--
-- The newest frame into `surface` - `width` by `height` of it from its
-- corner - mirrored or not. True when one was drawn; false and why when
-- not: "same", "waiting", "stopped" (the driver closed it: pulled out, or
-- nobody took a frame for three seconds) or "mjpeg".
--
function stream:draw(surface, mirror)
  local sequence, why = surface:camera(self.at, mirror, self.last)

  if sequence then
    self.last = sequence
    self.frames = self.frames + 1
    return true
  end

  return false, why
end

function stream:close()
  if self.closed then return end

  if self.recording then self:record_stop() end

  self.closed = true
  ask(OP.close, 0, 0, nil, self.handle)
  sys.release(self.cap)
end

--------------------------------------------------------------------------
-- **Recording** (`roadmap.md` 6d 8f): this stream's frames into an MP4 of
-- H.264, by the Record Kit.
--
--   stream:record_start()        true, or nil and why
--   stream:record_take()         the newest frame recorded; true when one was
--   stream:record_stop(path)     the file written: its bytes, or nil and why
--
-- Two regions of this process's own: the encoder's memory, and the file,
-- put together whole and written with one `write_from` when it stops - which
-- kfs takes at any size since 24 September (`design.md` 8.3b). The file's
-- region is a quarter of the free memory, between 16 and 256 MB; at 1.8
-- Mbit/s 64 MB is about four and a half minutes, and a recording that fills
-- it stops and is kept.
--------------------------------------------------------------------------

local function region(bytes)
  local cap, why = sys.memory((bytes + PAGE - 1) // PAGE)

  if not cap then return nil, why end

  local at = sys.memory_map(cap)

  if not at then
    sys.release(cap)
    return nil, "a region that could not be mapped"
  end

  return cap, at
end

-- As much of `want` as the machine will give, halving down to `least`.
local function largest(want, least)
  while want >= least do
    local cap, at = region(want)

    if cap then return cap, at, want end

    want = want // 2
  end

  return nil
end

function stream:record_start()
  local kit = use("/kits/record")

  if self.recording then return true end

  if self.size.pixels ~= "yuy2" then
    return nil, "a recording is made from YUY2 sizes"
  end

  local work_bytes, why = kit.work_bytes(self.width, self.height)

  if not work_bytes then return nil, why end

  local wcap, wat = region(work_bytes)

  if not wcap then
    return nil, "no memory for the encoder: " .. tostring(wat)
  end

  local mem = fs.read("/dev/memory")
  local free_mb = type(mem) == "table" and tonumber(mem.free_mb) or 64
  local want = math.max(16, math.min(256, free_mb // 4)) * 1024 * 1024
  local ocap, oat, out_bytes = largest(want, 8 * 1024 * 1024)

  if not ocap then
    sys.release(wcap)
    return nil, "no memory for the recording"
  end

  local r, rwhy = kit.open{ work = wat, work_bytes = work_bytes,
                            out = oat, out_bytes = out_bytes,
                            width = self.width, height = self.height,
                            fps = self.size.fps or 30 }

  if not r then
    sys.release(wcap)
    sys.release(ocap)
    return nil, rwhy
  end

  self.recording = { r = r, wcap = wcap, ocap = ocap, out_bytes = out_bytes,
                     last = 0, started = sys.ticks() }
  return true
end

-- The newest frame, recorded once. False and why when there was none new, or
-- when the recording could not take it - "the recording is full" among them,
-- which is the caller's cue to stop and keep what there is.
function stream:record_take()
  local rec = self.recording

  if not rec then return false, "not recording" end

  local sequence, why = rec.r:camera(self.at, rec.last)

  if sequence then
    rec.last = sequence
    return true
  end

  return false, why
end

-- How far it has got: bytes, frames, and the room it has.
function stream:record_progress()
  local rec = self.recording

  if not rec then return nil end

  return rec.r:bytes(), rec.r:frames(), rec.out_bytes
end

function stream:record_stop(path)
  local rec = self.recording

  if not rec then return nil, "not recording" end

  self.recording = nil

  local bytes, why = rec.r:close()
  local wrote, werr = nil, nil

  if bytes and path then
    wrote, werr = fs.write_from(path, rec.ocap, bytes)
  end

  sys.release(rec.wcap)
  sys.release(rec.ocap)

  if not bytes then return nil, why end
  if not path then return bytes end
  if wrote ~= bytes then return nil, "the file could not be written: " .. tostring(werr) end

  return bytes
end

return camera
