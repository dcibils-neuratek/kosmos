-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Puts every open window somewhere you can see it.
--
--   tile
--   tile 15      once fifteen application windows are open
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
-- Chrome is left alone - the desktop, the Deskbar's strip or dock, a popup -
-- which the window manager marks as such (`chrome`): none of it is a window
-- anybody opened, and moving it into the grid would be moving the one thing
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
-- **And a number, when the caller knows it.** Three still seconds was
-- taken for "everything is open" on 28 September, when the dated picture
-- opened sixteen applications at once under QEMU: Groove, the IDE and Text
-- Editor reached their windows after the others had been arranged, and
-- Groove, maximised, lay over all of them. `tile 15` waits until fifteen
-- application windows are open - the chrome, the desktop and the Deskbar,
-- not counted - and then for the same still moment, and gives up later,
-- since it knows there is something to wait for.
--
local wanted = tonumber(tostring(args or ""):match("%d+"))

local function counted(windows)
  local n = 0

  for _, w in ipairs(windows) do
    if not w.chrome then n = n + 1 end
  end

  return n
end

local function settled()
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local tick_hz = (fs.read("/Devices/kernel") or {}).tick_hz or 250
  local last, steady = -1, 0
  local giveup = sys.ticks() + hz * (wanted and 120 or 30)

  while sys.ticks() < giveup do
    local r = use("/Kosmos/Libraries/wmproto.lua").windows()

    if not r or not r.windows then return nil end

    local n = #r.windows

    if wanted and counted(r.windows) < wanted then
      last, steady = -1, 0
    elseif n == last and n > 1 then
      steady = steady + 1

      -- Three passes of no change, and something is actually open.
      if steady >= 3 then return r end
    else
      last, steady = n, 0
    end

    -- A second between looks, which is long enough for an application to
    -- get from `run` to its first window and short enough not to be felt.
    sys.sleep(tick_hz)
  end

  return nil
end

local reply = settled()

if not reply or not reply.windows then
  print("tile: the desktop did not answer, or nothing opened")
  return
end

local screen = fs.read("/Devices/screen") or {}
local W = screen.width or 1024
local H = screen.height or 768

-- Sorted by handle, which is the order the windows opened in and does not
-- change. Sorting by anything the desktop reorders - stacking, focus - would
-- mean the same set of windows landing in a different arrangement each run,
-- and these pictures are meant to be comparable with each other.
local wins = {}

for _, w in ipairs(reply.windows) do
  if not w.chrome then wins[#wins + 1] = w end
end

table.sort(wins, function(a, b) return a.handle < b.handle end)

if #wins == 0 then return end

--
-- As square a grid as the count allows, biased wide because a screen is.
--
--
-- **A big window gets two cells by two.** Cafesa3D is 1400 by 820, and a
-- corner of it one cell wide showed its header and nothing it is for. So a
-- window that would cover four cells of the grid the small ones make is
-- given four, placed first, and the rest fill in around it. The grid has as
-- many rows as a square would and as many columns as that needs - which for
-- eleven small windows is the four by three this always made, and for a
-- wide screen leans the same way it is wide.
--
local function grid_for(cells)
  local down = math.max(1, math.floor(math.sqrt(cells)))

  return math.ceil(cells / down), down
end

-- A margin of thirty either side, and sixty at the top. The Deskbar was a
-- panel in the top right corner and took 240 of the width; it is a strip or
-- a dock now, which is chrome and not in the grid.
local usable_w, usable_h = W - 60, H - 60
local across, down = grid_for(#wins)
local big, cells = {}, 0

for _, w in ipairs(wins) do
  big[w] = (w.w or 0) >= 2 * (usable_w // across) and (w.h or 0) >= 2 * (usable_h // down)
  cells = cells + (big[w] and 4 or 1)
end

across, down = grid_for(cells)

local cell_w, cell_h = usable_w // across, usable_h // down
local taken = {}

local function free(col, row, n)
  if col + n > across or row + n > down then return false end

  for r = row, row + n - 1 do
    for c = col, col + n - 1 do
      if taken[r * across + c] then return false end
    end
  end

  return true
end

-- The first cells, reading across, with room for `n` by `n`.
local function place(w, n)
  for row = 0, down - 1 do
    for col = 0, across - 1 do
      if free(col, row, n) then
        for r = row, row + n - 1 do
          for c = col, col + n - 1 do taken[r * across + c] = true end
        end

        local ok, why = fs.send("/Running/wm", {
          type = "move",
          window = w.handle,
          x = 30 + col * cell_w,
          y = 60 + row * cell_h,
        })

        if not ok then
          print(("tile: %s would not move: %s"):format(w.title, tostring(why)))
        end

        -- Raised in the order placed, the big first, so each small window
        -- lies over the big one's overflow rather than under it: the one
        -- that opened last - Cafesa3D, reading its scene - was on top of
        -- everything, and covered most of the screen.
        fs.send("/Running/wm", { type = "raise", window = w.handle })
        return
      end
    end
  end

  print(("tile: no room for %s"):format(w.title))
end

for _, w in ipairs(wins) do
  if big[w] then place(w, 2) end
end

for _, w in ipairs(wins) do
  if not big[w] then place(w, 1) end
end

print(("tile: %d windows, %dx%d"):format(#wins, across, down))
