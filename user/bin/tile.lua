-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Puts every open window somewhere you can see it.
--
--   tile
--
-- The desktop cascades a window that opens on top of another one, which is
-- enough to keep every title bar reachable and is not enough to let you read
-- six windows at once. This spreads them onto a grid instead.
--
-- It asks the window manager what is open and moves each one by handle. No
-- pointer, no dragging, no guessing which window is on top - `move` is part
-- of the protocol and a handle names exactly one window, which is the whole
-- reason to do it this way. Dragging windows around by their title bars from
-- outside was tried first, three times, and each attempt failed differently
-- because it depended on the stacking order, and the stacking order is a
-- race between applications reaching `ui.window`.
--
-- The Deskbar is left alone. It puts itself in the corner a window is least
-- likely to want, and moving it into the grid would be moving the one thing
-- that was already where it belonged.

--
-- Wait for the desktop to stop changing before arranging it.
--
-- `wm a,b,c` starts everything at once and this is one of them, so the
-- first version tiled an empty desktop and exited before any of the windows
-- it was meant to arrange had opened. Applications reach `ui.window` when
-- they reach it - the same race that made dragging them from outside
-- impossible.
--
-- So: count the windows, and act when the count has held still for a
-- moment. No list of what to wait for, which would go stale the first time
-- somebody tiled a different set.
--
local function settled()
  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
  local last, steady = -1, 0
  local giveup = sys.ticks() + hz * 30

  while sys.ticks() < giveup do
    local r = fs.send("/dev/wm", { type = "windows" })

    if not r or not r.windows then return nil end

    local n = #r.windows

    if n == last and n > 1 then
      steady = steady + 1

      -- Three passes of no change, and something is actually open.
      if steady >= 3 then return r end
    else
      last, steady = n, 0
    end

    -- A second between looks, which is long enough for an application to
    -- get from `run` to its first window and short enough not to be felt.
    local until_ = sys.ticks() + hz
    while sys.ticks() < until_ do sys.yield() end
  end

  return nil
end

local reply = settled()

if not reply or not reply.windows then
  print("tile: the desktop did not answer, or nothing opened")
  return
end

local screen = fs.read("/dev/screen") or {}
local W = screen.width or 1024
local H = screen.height or 768

-- Sorted by handle, which is the order the windows opened in and does not
-- change. Sorting by anything the desktop reorders - stacking, focus - would
-- mean the same set of windows landing in a different arrangement each run,
-- and these pictures are meant to be comparable with each other.
local wins = {}

for _, w in ipairs(reply.windows) do
  if w.title ~= "Deskbar" then wins[#wins + 1] = w end
end

table.sort(wins, function(a, b) return a.handle < b.handle end)

if #wins == 0 then return end

--
-- As square a grid as the count allows, biased wide because a screen is.
--
local across = math.ceil(math.sqrt(#wins))
local down = math.ceil(#wins / across)

-- The Deskbar's own corner is out of bounds: it is 210 wide with a margin,
-- and a window placed under it is a window you cannot read.
local usable = W - 240
local cell_w = usable // across
local cell_h = (H - 60) // down

for i, w in ipairs(wins) do
  local col = (i - 1) % across
  local row = (i - 1) // across

  local ok, why = fs.send("/dev/wm", {
    type = "move",
    window = w.handle,
    x = 30 + col * cell_w,
    y = 60 + row * cell_h,
  })

  if not ok then
    print(("tile: %s would not move: %s"):format(w.title, tostring(why)))
  end
end

print(("tile: %d windows, %dx%d"):format(#wins, across, down))
