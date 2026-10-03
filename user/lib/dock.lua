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

dock.H        = 64      -- the dock's height
dock.STRIP_H  = 32      -- the strip across the top, which holds the time
dock.ICON     = 36      -- an application's picture
dock.CELL     = 50      -- the square a picture is pressed in
dock.GAP      = 4       -- between cells
dock.PAD      = 10      -- at each end of the dock
dock.SEP      = 13      -- a separator's room
dock.KOSMOS_H = 42      -- the Kosmos button, a pill
dock.KOSMOS_IN = 16     -- inside it, either side
dock.MARK     = 22      -- the Kosmos button's picture
dock.RADIUS   = 22      -- the dock's own corners, floating

--
-- **What is pinned, until somebody pins something else**: the drawing's
-- eight (`docs/dock.html`), by the name `launch` takes. A person's own list
-- is `/Home/Preferences/dock`, `{ pins = { ... } }`, read in its place.
--
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
             starting = starting, front = front,
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

return dock
