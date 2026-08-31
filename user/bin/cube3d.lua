-- kosmos: application
-- A rotating solid cube, rendered in software.
--
--   wm cube3d
--
-- The demonstration is the split, not the cube. Every vertex transform,
-- every back-face test and the whole depth ordering happen in Lua, in
-- `/lib/g3d.lua`. The only thing in C is `surface:triangle`, because that
-- is the one loop that runs once per *pixel*.
--
-- The numbers that make that the obvious division rather than a matter of
-- taste: this cube is 12 triangles, so a frame's scene maths is roughly a
-- thousand operations. The same frame fills tens of thousands of pixels.
-- Moving the matrix code to C would speed up the small half.
--
-- It draws into a surface shared with the compositor - `gfx.md` 19.4 - so a
-- finished frame is one `commit` and no pixel is ever copied between
-- processes.

local ui  = use("/lib/ui.lua")
local g3d = use("/lib/g3d.lua")

local W, H = 400, 320
local BG = 0xff101828

local win, err = ui.window{ title = "Cube", w = W, h = H, x = 180, y = 100,
                            direct = true }

if not win then
  print("cube3d: " .. tostring(err))
  return
end

if not win:surface() then
  print("cube3d: this window did not get a shared surface")
  return
end

local mesh    = g3d.cube(1.6)
local scratch = g3d.scratch()

-- Fixed for the life of the window: neither the lens nor where the camera
-- stands changes, so neither belongs in the frame loop.
local projection = g3d.perspective(math.pi / 4, W / H, 0.1, 100)
local view       = g3d.look_at({ 0, 0, -4.5 }, { 0, 0, 0 }, { 0, 1, 0 })
local view_proj  = g3d.multiply(view, projection)

local hz      = fs.read("/dev/cpu").counter_hz
local frames  = 0
local started = sys.ticks()
local angle   = 0

while win.running do
  local s = win:surface()

  s:fill(0, 0, W, H, BG)

  -- Two axes at different rates, so the cube never returns to the same
  -- pose and every face gets its turn at being culled.
  local model = g3d.multiply(g3d.rotation_x(angle * 0.7),
                             g3d.rotation_y(angle))

  g3d.render(s, mesh, g3d.multiply(model, view_proj), W, H, scratch)

  if not win:commit{ x = 0, y = 0, w = W, h = H } then
    break
  end

  angle  = angle + 0.03
  frames = frames + 1

  -- A tick of waiting rather than none, for the reason plasma gives: a loop
  -- that never blocks is a thread that is always runnable, and an idle
  -- desktop should be idle.
  local reply = fs.send("/dev/wm", { type = "poll", window = win.handle,
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
      print(("cube3d: %d frames in %ds, %d a second")
            :format(frames, seconds, frames // seconds))
    end
  end
end
