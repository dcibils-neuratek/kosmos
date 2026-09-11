-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where icons go on a desktop, and how a name fits under one.
--
-- Here rather than inside Tracker so that it can be checked without a
-- screen: `tools/test_iconlayout.lua` runs these on the build machine. It is
-- arithmetic over rectangles and strings, which is the part of a desktop
-- that is easy to get subtly wrong and tedious to find by looking at one.

local layout = {}

--
-- A name as at most two lines of `room` characters.
--
-- The first line is the start of the name and the second carries on from
-- it. A name too long even for two keeps its *end* on the second line,
-- after a `~`, because the end is where the extension is and the extension
-- is what says what the file is.
--
function layout.label(name, room)
  name = tostring(name or "")
  room = math.max(2, room)

  if #name <= room then return name, nil end

  local rest = name:sub(room + 1)

  if #rest > room then rest = "~" .. rest:sub(-(room - 1)) end

  return name:sub(1, room), rest
end

--
-- Where each of `items` goes, as a cell `cell_w` by `cell_h` in a view `w`
-- by `h`, returned as `{ x, y }` per item in the same order.
--
-- An item with `x` and `y` keeps them, pulled back inside the view if the
-- screen has shrunk since: an icon past the edge is an icon nobody can drag
-- back. The rest take free cells down the first column and then the next,
-- which is where a desktop's icons have started since the Macintosh. A cell
-- is free when no icon already placed overlaps it, so an icon dragged into
-- the grid moves the next new one along instead of hiding under it.
--
-- Once every cell is taken there is nowhere left, and the rest share the
-- last one rather than landing off the screen.
--
function layout.place(items, cell_w, cell_h, w, h, margin)
  margin = margin or 0

  local rects, placed = {}, {}

  local function overlaps(x, y)
    for _, r in ipairs(placed) do
      if x < r.x + cell_w and x + cell_w > r.x
         and y < r.y + cell_h and y + cell_h > r.y then
        return true
      end
    end

    return false
  end

  for i, it in ipairs(items) do
    local x, y = tonumber(it.x), tonumber(it.y)

    if x and y then
      local r = {
        x = math.max(0, math.min(math.floor(x), w - cell_w)),
        y = math.max(0, math.min(math.floor(y), h - cell_h)),
      }

      rects[i] = r
      placed[#placed + 1] = r
    end
  end

  local per_column = math.max(1, (h - margin) // cell_h)
  local columns = math.max(1, (w - margin) // cell_w)
  local last = per_column * columns
  local cell = 0

  local function cell_xy(c)
    return margin + (c // per_column) * cell_w,
           margin + (c % per_column) * cell_h
  end

  for i in ipairs(items) do
    if not rects[i] then
      local x, y = cell_xy(math.min(cell, last - 1))

      while cell < last and overlaps(x, y) do
        cell = cell + 1

        if cell < last then x, y = cell_xy(cell) end
      end

      cell = cell + 1

      local r = { x = x, y = y }

      rects[i] = r
      placed[#placed + 1] = r
    end
  end

  return rects
end

return layout
