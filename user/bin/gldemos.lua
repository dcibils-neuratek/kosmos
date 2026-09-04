-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- GL Demos: TinyGL's own, rendered in software.
-- kosmos: application
-- kosmos: section demos
--
--   wm gldemos
--
-- **The demos are C and they were not rewritten.** They are TinyGL's own,
-- vendored unmodified in `runtime/upstream/tinygl/examples/`, renamed apart
-- on the compile line so that eight programs which each define a function
-- called `draw` can share one binary. Doom is arranged this way and for the
-- same reason: transcribing them into Lua would be eight chances to get a
-- normal backwards, and would mean the next upstream release could no
-- longer simply be dropped in.
--
-- What is Lua here is what Lua is for everywhere else - which one to show,
-- where to put it, and when to stop. The triangles are somebody else's and
-- the pixels are C's.
--
-- **A direct window**, like `cube3d` and `doom`: this draws its own pixels
-- into a surface shared with the compositor rather than recording a display
-- list, because there is nothing to record - a rasteriser hands over a
-- finished picture. `gfx.md` 19.4.

local ui = use("/lib/ui.lua")
local gl = use("/kits/gl")

local W, H  = 520, 400
local LIST  = 132                   -- the column of names, on the left
local VW    = W - LIST
local BG    = 0xff101828
local INK   = 0xffc8d4e8
local DIM   = 0xff7c8ba0
local PICK  = 0xff1a6fdc

local win, err = ui.window{ title = "GL Demos", w = W, h = H, x = 150, y = 100,
                            direct = true }

if not win then
  print("gldemos: " .. tostring(err))
  return
end

if not win:surface() then
  print("gldemos: this window did not get a shared surface")
  return
end

local demos = gl.demos()
local chosen = 1
local ctx, failed

--
-- One context, remade for each demo.
--
-- Rather than reused: a demo sets lighting, depth testing and matrices in
-- its `init` and none of them undoes what the last one did. A fresh context
-- is a fresh GL state, which is the only honest way to switch between eight
-- programs each written as though it owned the machine.
--
local function show(n)
  chosen = n

  if ctx then ctx:close() ctx = nil end

  local c, why = gl.context(VW, H)

  if not c then failed = tostring(why) return end

  local ok, oops = gl.start(demos[n].name, VW, H)

  if not ok then failed = tostring(oops) return end

  ctx, failed = c, nil
end

show(1)

local frames, from, fps = 0, sys.ticks(), 0
local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

while win.running do
  local s = win:surface()

  s:fill(0, 0, LIST, H, BG)

  for i, d in ipairs(demos) do
    local y = 10 + (i - 1) * 18

    if i == chosen then
      s:fill(2, y - 3, LIST - 4, 17, PICK)
    end

    s:text(8, y, ("%d  %s"):format(i, d.name),
           (i == chosen) and 0xffffffff or INK)
  end

  s:text(8, H - 42, ("%d fps"):format(fps), DIM)
  s:text(8, H - 24, "1-8 to choose", DIM)

  if ctx then
    --
    -- One frame from the demo, then the copy.
    --
    -- `frame` is `idle` and `draw` back to back, the order `ui_loop` uses.
    -- The blit is separate because TinyGL renders into a buffer of its own
    -- and a surface has a pitch of its own - `gfx.md` 19.3, the discipline
    -- everything here obeys.
    --
    gl.frame()
    gl.blit(ctx, s, LIST, 0)
  else
    s:fill(LIST, 0, VW, H, BG)
    s:text(LIST + 10, 20, failed or "no demo", DIM)
  end

  if not win:commit{ x = 0, y = 0, w = W, h = H } then
    break
  end

  frames = frames + 1

  local span = sys.ticks() - from

  if span >= hz then
    fps = (frames * hz) // span
    frames, from = 0, sys.ticks()
  end

  -- A tick of waiting rather than none: a loop that never blocks is a thread
  -- that is always runnable, and an idle desktop should be idle.
  local reply = fs.send("/dev/wm", { type = "poll", window = win.handle,
                                     wait = 1 })

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "key" then
      if ev.code == 3 then
        win:close()
      elseif ev.code >= 49 and ev.code <= 56 then    -- '1'..'8'
        local n = ev.code - 48

        if demos[n] then show(n) end
      elseif ctx then
        gl.key(ev.code)
      end
    end
  end
end
