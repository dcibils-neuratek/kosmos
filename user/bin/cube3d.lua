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

local hz    = fs.read("/dev/cpu").counter_hz
local angle = 0

-- The rate, measured over the last second rather than since the window
-- opened. An average from boot converges and then stops moving, so it
-- cannot show a frame getting more expensive - which is the only thing the
-- number is for.
local shown_fps   = 0
local shown_faces = 0
local total_faces = #mesh.faces // 3
local window_from = sys.ticks()
local window_n    = 0

while win.running do
  local s = win:surface()

  s:fill(0, 0, W, H, BG)

  -- Two axes at different rates, so the cube never returns to the same
  -- pose and every face gets its turn at being culled.
  local model = g3d.multiply(g3d.rotation_x(angle * 0.7),
                             g3d.rotation_y(angle))

  local faces = g3d.render(s, mesh, g3d.multiply(model, view_proj),
                           W, H, scratch)

  window_n = window_n + 1

  local now = sys.ticks()
  local span = now - window_from

  if span >= hz then
    shown_fps   = window_n * hz // span
    shown_faces = faces
    window_from = now
    window_n    = 0
  end

  -- Drawn after the cube so it is on top of it, and inside the same frame
  -- so the number and the picture it describes are committed together.
  --
  -- The face count is here because it is the one thing on screen that says
  -- back-face culling is working: half of a cube's twelve triangles face
  -- away at any moment, and a `12/12` would mean the far ones are being
  -- drawn too.
  s:text(8, 8, ("%d fps   %d/%d faces")
               :format(shown_fps, shown_faces, total_faces), 0xffc8d4e8)

  if not win:commit{ x = 0, y = 0, w = W, h = H } then
    break
  end

  angle = angle + 0.03

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
end
