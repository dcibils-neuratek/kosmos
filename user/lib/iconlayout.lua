-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where icons go on a desktop, and how a name fits under one.
--
-- Here rather than inside Tracker so that it can be checked without a
-- screen: `tools/test_iconlayout.lua` runs these on the build machine. It is
-- arithmetic over rectangles and strings, which is the part of a desktop
-- that is easy to get subtly wrong and tedious to find by looking at one.

local layout = {}

--
-- **A name as at most two lines, each no wider than `width`**, as `measure`
-- says a string is wide - `gfx.measure` in Tracker, anything on the host.
--
-- It counted characters, `width` over the face's widest glyph, and the face
-- is proportional: about five of them fitted in a cell where "Deskbar" is
-- sixty pixels of seventy-six, so it wrapped as "Deskb" and "ar". Diego, 24
-- September: "the name of files is being broken into 2 lines where it could
-- it in 1 line", "make the space for the file name wider like macos does"
-- (`roadmap.md` 6f). So as the Finder does it:
--
--   - a name that fits is one line;
--   - the first line ends where a word does - after a space, a `-` or a
--     `_` - or, with none, before the extension: "cheatsheet", ".html";
--   - a second line too long for the cell is shortened in the middle, with
--     its end kept whole - where the extension is, and the extension is
--     what says what a file is: "2026-0...AM.mov".
--
-- Cut between characters and never inside one: UTF-8 where the name is.
--
local function starts(name)
  local out = {}

  if utf8.len(name) then
    for p in utf8.codes(name) do out[#out + 1] = p end
  else
    for i = 1, #name do out[i] = i end
  end

  out[#out + 1] = #name + 1
  return out
end

-- How many bytes from the start of `s` fit in `width`, whole characters.
local function head_fits(s, width, measure)
  local at, n = starts(s), 0

  for k = 2, #at do
    if measure(s:sub(1, at[k] - 1)) > width then break end
    n = at[k] - 1
  end

  return n
end

-- Where the most of the end of `s` that fits in `width` begins.
local function tail_fits(s, width, measure)
  local at, from = starts(s), #s + 1

  for k = #at - 1, 1, -1 do
    if measure(s:sub(at[k])) > width then break end
    from = at[k]
  end

  return from
end

local BREAK_AFTER = { [" "] = true, ["-"] = true, ["_"] = true }

function layout.label(name, width, measure)
  name = tostring(name or "")
  measure = measure or function(s) return #s end

  if measure(name) <= width then return name, nil end

  -- The first line: as much as fits, back to where a word last ended - but
  -- not so far back that it holds a third of a line or less.
  local cut = math.max(1, head_fits(name, width, measure))
  local first, rest = name:sub(1, cut), name:sub(cut + 1)
  local broke = false

  for i = cut, 2, -1 do
    local c = name:sub(i, i)

    if BREAK_AFTER[c] and measure(name:sub(1, i)) >= width // 3 then
      first = (c == " ") and name:sub(1, i - 1) or name:sub(1, i)
      rest = name:sub(i + 1)
      broke = true
      break
    end
  end

  local ext = name:match(".*()%.")

  if not broke and ext and ext > 2 and ext - 1 <= cut
     and measure(name:sub(1, ext - 1)) >= width // 3 then
    first, rest = name:sub(1, ext - 1), name:sub(ext)
  end

  rest = rest:gsub("^ +", "")

  if measure(rest) <= width then return first, rest end

  -- The second line, shortened in the middle: two fifths of it for the end,
  -- stretched to the whole extension and a character before its dot when
  -- that fits in two thirds, and the rest for as much of the start as fits.
  local dots = "..."
  local room = width - measure(dots)
  local from = tail_fits(rest, room * 2 // 5, measure)
  local dot = rest:match(".*()%.")

  if dot and dot < from and measure(rest:sub(dot)) <= room * 2 // 3 then
    from = dot
  end

  if dot and from == dot and dot > 1 then
    local before = (utf8.len(rest) and utf8.offset(rest, -1, dot)) or dot - 1

    if measure(rest:sub(before)) <= room * 2 // 3 then from = before end
  end

  local tail = rest:sub(from)
  local lead = head_fits(rest, room - measure(tail), measure)

  return first, rest:sub(1, lead) .. dots .. tail
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
