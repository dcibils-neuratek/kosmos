-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Deskbar as a dock: what is in it, where, and what a press hit.
--
-- `roadmap.md`, a dock at the bottom - Diego, 3 October 2026: "an
-- appearance setting to place the taskbar on the bottom center and
-- replicate as mich as possible the design language of googlebook", drawn
-- in `docs/dock.html` and agreed: "everything else is just perfect!". The
-- same Deskbar - the Kosmos button, what is running, what is pinned - laid
-- out as Googlebook's dock rather than BeOS's bar.
--
-- **Arithmetic here, pixels in the Deskbar.** Which cells there are and
-- where each one is are questions with answers that can be checked on the
-- Mac (`tools/test_dock.lua`) without booting anything; what each looks like
-- is drawn by the Deskbar with the kit's commands, as the bar is.
--
-- **A cell an application, not a window**, as Googlebook's dock and every
-- dock since NeXT's: an application's picture once, however many windows
-- it has open, with a mark under it while it runs. What is pinned is there
-- whether or not it runs, in the order it was pinned; what runs and is not
-- pinned comes after a separator, in the order it started.

local dock = {}

dock.H        = 60      -- the dock's height, at its Medium size (below); 64 until 7 October
dock.STRIP_H  = 32      -- the strip across the top, which holds the time
dock.ICON     = 36      -- an application's picture
dock.CELL     = 50      -- the square a picture is pressed in
dock.GAP      = 4       -- between cells
dock.PAD      = 10      -- at each end of the dock
dock.SEP      = 13      -- a separator's room
dock.KOSMOS_H = 42      -- the Kosmos button, a pill
dock.KOSMOS_IN = 16     -- inside it, either side
dock.MARK     = 22      -- the Kosmos button's picture
dock.RADIUS   = 18      -- the dock's own corners, floating: a window's (`theme.CORNER`)

--
-- **What is pinned, until somebody pins something else**: the drawing's
-- eight (`docs/dock.html`), by the name `launch` takes. A person's own list
-- is `/Home/Preferences/dock`, `{ pins = { ... } }`, read in its place.
--
--
-- **The dock's size, a setting** (Diego, 7 October: "can you make the dock 4
-- points shorter in height", then "can we add a preferences setting for
-- it?", "so we can resize as needed"): Appearance's `dock_size`, each size
-- its height and what is in it, scaled together. Medium is the 60 he asked
-- for; 64 was the drawing's.
--
--
-- **Its corners are a window's at every size** (Diego, 8 October:
-- "rounded corners should always stay the same regardless of the color
-- theme, use the dark one as the default"): 18, `theme.CORNER`, so the dock
-- and the windows over it are rounded alike.
--
dock.SIZES = {
  small  = { H = 52, ICON = 30, CELL = 42, KOSMOS_H = 36, MARK = 20, RADIUS = 18 },
  medium = { H = 60, ICON = 36, CELL = 50, KOSMOS_H = 42, MARK = 22, RADIUS = 18 },
  large  = { H = 72, ICON = 44, CELL = 60, KOSMOS_H = 50, MARK = 26, RADIUS = 18 },
  larger = { H = 84, ICON = 52, CELL = 70, KOSMOS_H = 58, MARK = 30, RADIUS = 18 },
}

-- The sizes taken for this process's dock, by name; Medium for any other.
function dock.sized(name)
  local s = dock.SIZES[tostring(name or "")] or dock.SIZES.medium

  for k, v in pairs(s) do dock[k] = v end

  return dock
end

dock.PINS = { "tracker", "browser", "terminal", "music", "groove",
              "cafesa3d", "ide", "preferences" }

-- An application's name from the program the window manager reports:
-- `/Kosmos/Apps/groove.lua` is `groove`, and so is `groove`.
function dock.name(program)
  if type(program) ~= "string" or program == "" then return nil end

  return (program:match("([^/]+)%.lua$") or program:match("([^/]+)$")):lower()
end

--
-- The cells, left to right: the Kosmos button, the pinned applications, a
-- separator, and what runs that is not pinned. `running` is the Deskbar's
-- list - each `{ handle, title, icon, program, focused, hidden, starting }`
-- - and `icon_of(name)` a pinned application's picture when none of its
-- windows is open to say.
--
function dock.items(pins, running, icon_of)
  local by, order = {}, {}
  local pinned_name = {}

  for _, name in ipairs(pins or {}) do pinned_name[name] = true end

  for _, row in ipairs(running or {}) do
    local name = dock.name(row.program)

    if name then
      if not by[name] then
        by[name] = {}
        order[#order + 1] = name
      end

      local list = by[name]

      list[#list + 1] = row
    end
  end

  local function cell(name)
    local rows = by[name] or {}
    local front, icon, title, starting = false, nil, nil, false

    for _, row in ipairs(rows) do
      if row.focused and not row.hidden then front = true end
      if row.starting then starting = true end
      icon = icon or row.icon
      title = title or row.title
    end

    return { kind = "app", name = name, windows = rows,
             running = #rows > 0 and not (starting and #rows == 1 and rows[1].starting),
             starting = starting, front = front, pinned = pinned_name[name] or nil,
             icon = icon or (icon_of and icon_of(name)) or "App_Generic",
             title = title or name }
  end

  local items, pinned = { { kind = "kosmos" } }, {}

  for _, name in ipairs(pins or {}) do
    if not pinned[name] then
      pinned[name] = true
      items[#items + 1] = cell(name)
    end
  end

  local rest = {}

  for _, name in ipairs(order) do
    if not pinned[name] then rest[#rest + 1] = cell(name) end
  end

  if #rest > 0 then
    items[#items + 1] = { kind = "separator" }

    for _, c in ipairs(rest) do items[#items + 1] = c end
  end

  return items
end

--
-- Where each cell goes, given how wide "Kosmos" is in the face in force:
-- each item's `x` and `w` set, and the dock's whole width returned.
--
function dock.layout(items, kosmos_text_w)
  local x = dock.PAD

  for i, item in ipairs(items) do
    if i > 1 then x = x + dock.GAP end

    if item.kind == "kosmos" then
      item.w = dock.KOSMOS_IN * 2 + dock.MARK + 8 + (kosmos_text_w or 0)
    elseif item.kind == "separator" then
      item.w = dock.SEP
    else
      item.w = dock.CELL
    end

    item.x = x
    x = x + item.w
  end

  return x + dock.PAD
end

-- The cell a press at `x` landed in, or nil: between cells, the ends and a
-- separator are nothing to press.
function dock.hit(items, x)
  for _, item in ipairs(items) do
    if item.kind ~= "separator" and item.x and x >= item.x and x < item.x + item.w then
      return item
    end
  end

  return nil
end

--
-- **What a press on an application does**, as a word for the window
-- manager and the window it is about - the Deskbar's own rule, a cell
-- rather than a button:
--
--   not running        `launch` it;
--   in front           `minimise` it - a second press puts it away, as a
--                      bar's button does;
--   running behind     `raise` the window of it nearest the front, or bring
--                      back one that was put away.
--
-- An application with several windows raises the first that is showing,
-- else the first.
--
function dock.action(item)
  if not item or item.kind ~= "app" then return nil end

  if item.starting and not item.running then return "wait" end

  if not item.running then return "launch", item.name end

  if item.front then
    for _, row in ipairs(item.windows) do
      if row.focused and not row.hidden then return "minimise", row.handle end
    end
  end

  for _, row in ipairs(item.windows) do
    if not row.hidden and not row.starting then return "raise", row.handle end
  end

  for _, row in ipairs(item.windows) do
    if not row.starting then return "raise", row.handle end
  end

  return "wait"
end

--------------------------------------------------------------------------
-- **Arranged by hand** (Diego, 3 October 2026: "we need a way to move apps
-- around the dock to reorder them as the user wants. also how do i add or
-- remove apps from the dock?"): an icon dragged along the dock lands where
-- it is let go, dragged up off it is taken out, and a running one kept is
-- pinned. These are the lists; the Deskbar writes them to
-- `/Home/Preferences/dock`.
--------------------------------------------------------------------------

-- How far above the dock an icon has to be let go to be taken out of it.
dock.REMOVE_ABOVE = 40

-- `pins` with `name` at `at` (1 the first; nil, the end) - moved there if
-- it was elsewhere in it. A new table; `pins` is not changed.
function dock.pin(pins, name, at)
  local out = {}

  for _, n in ipairs(pins or {}) do
    if n ~= name then out[#out + 1] = n end
  end

  at = math.max(1, math.min(#out + 1, at or #out + 1))
  table.insert(out, at, name)

  return out
end

-- `pins` without `name`. A new table.
function dock.unpin(pins, name)
  local out = {}

  for _, n in ipairs(pins or {}) do
    if n ~= name then out[#out + 1] = n end
  end

  return out
end

--
-- **Where a dragged icon would land**, let go at `x` in the dock: its place
-- among the pins - one more than the number of pinned cells, other than
-- itself, whose middle is left of `x` - or nil when `x` is past the last of
-- them, in the part after the separator, which is what runs unpinned.
-- `items` laid out (`dock.layout`); `name` the icon being dragged.
--
function dock.drop_at(items, x, name)
  local before, last_right = 0, nil

  for _, it in ipairs(items) do
    if it.kind == "app" and it.pinned then
      last_right = it.x + it.w

      if it.name ~= name and it.x + it.w // 2 < x then before = before + 1 end
    end
  end

  if last_right and x > last_right + dock.SEP then return nil end

  return before + 1
end

--
-- **What letting go of a dragged icon does**, at `x, y` in the dock (`y`
-- negative above it): the new pins and a word for it -
--
--   "removed"   let go well above the dock, and it was pinned;
--   "moved"     a pinned one to another place among the pins;
--   "kept"      a running one dropped among the pins - pinned there;
--   "unpinned"  a pinned one dropped after the separator;
--
-- or the pins as they were and nil, for a drop that changes nothing.
--
function dock.drop(pins, items, name, x, y)
  local was = false

  for _, n in ipairs(pins or {}) do
    if n == name then was = true end
  end

  if y < -dock.REMOVE_ABOVE then
    if was then return dock.unpin(pins, name), "removed" end

    return pins, nil
  end

  local at = dock.drop_at(items, x, name)

  if at then
    local out = dock.pin(pins, name, at)

    for i, n in ipairs(out) do
      if n ~= (pins or {})[i] then return out, was and "moved" or "kept" end
    end

    if #out ~= #(pins or {}) then return out, "kept" end

    return pins, nil
  end

  if was then return dock.unpin(pins, name), "unpinned" end

  return pins, nil
end

return dock
