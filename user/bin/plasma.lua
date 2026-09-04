-- kosmos: application
-- kosmos: section demos
-- Pixels, drawn by the application itself, thirty times a second.
--
--   wm plasma
--
-- Every other window in this system sends the compositor a list of things
-- to draw. This one sends nothing: it and the compositor share the memory,
-- it fills that memory, and it says when a frame is ready.
--
--------------------------------------------------------------------------
-- What this is demonstrating, and what it is not.
--
-- It is `gfx.md` 19.4 working: two buffers in one shared region, the
-- application drawing into the one that is not being shown, and a `commit`
-- carrying a damage rectangle that swaps them. No locks anywhere - neither
-- side ever touches the buffer the other is using - and no copy: the
-- compositor blits straight out of this program's memory.
--
-- It is *not* a demonstration that Lua can push pixels. It cannot, and this
-- program does not try: every pixel here is written by `fill` and `span`,
-- which are C. What Lua does is decide where the bands go, which is a few
-- dozen decisions a frame rather than a few hundred thousand.
--
-- That is the same division as everywhere else, and this is the case that
-- tests it hardest.
--------------------------------------------------------------------------

local ui = use("/lib/ui.lua")

local W, H = 420, 300
local BANDS = 60                -- horizontal strips, each one `span` call

local win, err = ui.window{ title = "Plasma", w = W, h = H, x = 140, y = 120,
                            direct = true }

if not win then
  print("plasma: " .. tostring(err))
  return
end

if not win:surface() then
  print("plasma: this window did not get a shared surface")
  return
end

local hz = fs.read("/dev/cpu").counter_hz
local frames = 0
local started = sys.ticks()

--
-- A colour that moves. Integer arithmetic, though no longer because it has
-- to be: the context switch saves the whole FP register file now, so a
-- process may use doubles freely and `cube3d` does. Integer here because a
-- colour ramp is integer arithmetic and reaching for a double to compute
-- one would be the affectation, not the discipline.
--
local function band_colour(i, t)
  local a = (i * 7 + t * 3) % 512
  local b = (i * 3 + t * 5) % 512

  if a > 255 then a = 511 - a end
  if b > 255 then b = 511 - b end

  local r = a
  local g = b
  local bl = 255 - ((a + b) // 2)

  return 0xff000000 | (r << 16) | (g << 8) | bl
end

local height = (H + BANDS - 1) // BANDS

while win.running do
  local s = win:surface()
  local t = frames

  for i = 0, BANDS - 1 do
    s:fill(0, i * height, W, height, band_colour(i, t))
  end

  -- Finished. The compositor shows this one and hands back the other.
  if not win:commit{ x = 0, y = 0, w = W, h = H } then
    break
  end

  frames = frames + 1

  --
  -- Every event, and the window's own bookkeeping. A direct window still
  -- has to be told when it is closed, and this is the loop that does it -
  -- `win:run()` is for windows made of views and would paint over nothing
  -- here.
  --
  --
  -- One scheduler tick of waiting, not zero.
  --
  -- With zero this loop never blocks, which makes it a thread that is
  -- always runnable - the thing that had the processor meter reading ninety
  -- per cent on an empty desktop before input was interrupt-driven. A tick
  -- is ten milliseconds, so this still draws as fast as anything can be
  -- seen, and the machine is idle in between.
  --
  local reply = fs.send("/app/wm", { type = "poll", window = win.handle,
                                     wait = 1 })

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "key" and ev.code == 3 then
      win:close()
    end
  end

  if frames % 60 == 0 then
    local seconds = (sys.ticks() - started) // hz

    if seconds > 0 then
      print(("plasma: %d frames in %ds, %d a second")
            :format(frames, seconds, frames // seconds))
    end
  end
end
