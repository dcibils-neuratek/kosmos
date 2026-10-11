-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Open and Save window, the same in every application (USB step 6d).
--
-- BeOS called this a BFilePanel and Tracker provided it, so that every
-- application's open-and-save looked the same because it *was* the same.
-- Here it is a library rather than a service for one reason: a panel served
-- by another process would have to draw into this application's window or
-- open one of its own, and the second is a window the application cannot
-- place, size, or close when it goes away. Running here, it also sees
-- exactly what this application can see, and nothing more.
--
-- **It is drawn in `drives.html`**: the same sidebar as Tracker - Places,
-- System and Drives, from `/Kosmos/Libraries/sidebar.lua` - the path as a trail, and the
-- folder as Name, Size and Kind. What the application is handed is the file
-- at its real place, so no application has to know that MyPhotos exists.
--
-- **One click selects; a second click, or Enter, opens** - a folder is
-- entered and a file chosen - as in Tracker and its sidebar (`ui.md`
-- 16.8c). The button at the bottom right does the same to the selection.
--
-- **The keyboard is where the panel's work is, from the moment it opens**:
-- on the list when opening, so the arrows and Return choose without a
-- click first; on the name when saving, so typing names the file and
-- Return saves it. The first thing added - the trail - had it until 4
-- October 2026, and Return did nothing until something was clicked.
--
--   panel.open{ start = "/Home/roms/snes", title = "Open ROM",
--               filter = function(name) return name:match("%.sfc$") end,
--               on_choose = function(path) ... end }
--   panel.save{ start = "/Home", name = "untitled.lua",
--               on_choose = function(path) ... end }
--   panel.folder{ start = "/Home", title = "Share a folder", button = "Share",
--                 on_choose = function(path) ... end }
--
-- **A folder is chosen as a file is opened** (`docs/sharing.md` N12c-2, the
-- Servers window's Share a folder of /Home): only folders are listed, a
-- second click or Return goes into one, and the button chooses the one
-- selected - or, with none to select, the folder the list is showing.
--
-- The callback gets a whole path or is never called: Cancel closes the
-- window and calls `on_cancel`, for a caller that wants to know. `filter`
-- hides files it answers false for; folders always show, because a folder
-- is how you reach the files.

local ui      = use("/Kosmos/Libraries/ui.lua")
local files   = use("/Kosmos/Libraries/files.lua")
local types   = use("/Kosmos/Libraries/filetypes.lua")
local sidebar = use("/Kosmos/Libraries/sidebar.lua")
local placelib = use("/Kosmos/Libraries/places.lua")

local theme = ui.theme

local panel = {}

local W, H   = 640, 420
local SIDE_W = 190
local TOP    = 34                     -- the trail above, the panes below
local FOOT   = 72                     -- the name, the buttons, the status

-- A name cut to fit its column - `ui.fitted`, which began here.
local fitted = ui.fitted

local function open(spec, mode)
  local win, err = ui.window{
    title = spec.title or (mode == "save" and "Save" or mode == "folder" and "Choose a folder"
                           or "Open"),
    w = W, h = H,
    x = spec.x or 200, y = spec.y or 120,
  }

  if not win then return nil, err end

  local where   = spec.start or "/Home"
  local entries = {}
  local pane_h  = H - TOP - FOOT
  local name

  local status = ui.label{ x = 12, y = H - 24, w = W - 24, text = "" }
  local trail  = ui.trail{ x = 12, y = 10, w = W - 24, text = where }
  local side   = sidebar.new()
  local tree   = ui.tree{ x = 12, y = TOP, w = SIDE_W, h = pane_h,
                          roots = side.roots() }

  -- Where Size ends and Kind begins, from the list's own width, so the
  -- header and the rows are measured against the same numbers.
  local list_x = 12 + SIDE_W + 6
  local list_w = W - list_x - 12
  local SIZE_R = list_w - 150
  local KIND_X = list_w - 136

  local header = ui.view{ x = list_x, y = TOP, w = list_w, h = gfx.font.h + 4 }

  function header:draw(g)
    g:text(6, 2, "Name", theme.text_dim)
    g:text(SIZE_R - gfx.measure("Size"), 2, "Size", theme.text_dim)
    g:text(KIND_X, 2, "Kind", theme.text_dim)
  end

  local listing = ui.list{ x = list_x, y = TOP + header.h, w = list_w,
                           h = pane_h - header.h, items = {} }

  function listing:draw_item(g, e, x, y, _, on)
    -- White on the accent where a look fills a chosen row with it; a flat
    -- look marks the row with a pale fill and keeps the words' own colours.
    local lit = on and not theme.flat
    local fg  = lit and theme.text_on or theme.text
    local dim = lit and theme.text_on or theme.text_dim
    local kind = (e.kind == "directory") and "folder"
                 or (types.kind_of(e.name, e.attrs) or "file")

    if e.kind ~= "directory" then
      local size = files.size(e.size)

      g:text(SIZE_R - gfx.measure(size), y, size, dim)
      g:text(x, y, fitted(e.name, SIZE_R - gfx.measure(size) - x - 12), fg)
    else
      g:text(x, y, fitted(e.name, SIZE_R - x - 12), fg)
    end

    g:text(KIND_X, y, fitted(kind, list_w - KIND_X - 16), dim)
  end

  -- In folder mode, for whoever drives the window from outside: where it
  -- is, what is selected, and where its button is in the window.
  local function told()
    if mode ~= "folder" or not panel.button_at then return end

    local e = entries[listing.selected]

    print(("panel: %s at %s, %s selected; button at %d,%d"):format(
          spec.title or "Choose a folder", where, e and e.name or "nothing",
          panel.button_at.x, (win.head_h or 0) + panel.button_at.y))
  end

  local function show(path)
    local found, why = files.entries(path)

    if not found then
      status.text = tostring(why)
      return
    end

    local shown = {}

    for _, e in ipairs(placelib.files_only(path, found)) do
      if e.kind == "directory"
         or (mode ~= "folder" and (not spec.filter or spec.filter(e.name))) then
        shown[#shown + 1] = e
      end
    end

    where, entries = path, shown
    trail.text       = path
    listing.items    = shown
    listing.selected = 1
    listing.top      = 1
    status.text      = (#shown == 0) and (mode == "folder" and "no folders in it" or "empty")
                       or ""
    told()
  end

  local function finish(path)
    win:close()

    if spec.on_choose then spec.on_choose(path) end
  end

  -- A folder is entered; a file is chosen when opening, and its name offered
  -- when saving - which is the whole difference between the two modes and
  -- the reason they are one function.
  local function act(e)
    if not e then return end

    if e.kind == "directory" then
      show(files.join(where, e.name))
    elseif mode == "save" then
      name.text = e.name
    else
      finish(files.join(where, e.name))
    end
  end

  listing.on_select = function(_, e)
    if mode == "save" and e and e.kind ~= "directory" then name.text = e.name end
    told()
  end

  -- In folder mode the arrows report where they are, for the button's
  -- sake: `on_select` there only tells, and chooses nothing.
  listing.arrows_choose = mode == "folder"
  listing.on_open  = function(_, e) act(e) end
  trail.on_visit   = function(path) show(path) end
  tree.on_select   = function(_, node) if node.path then show(node.path) end end

  win:add(trail)
  win:add(tree)
  win:add(header)
  win:add(listing)

  local bx = W - 12 - 96

  if mode == "save" then
    -- The name typed, saved where the list is - by the button, or by
    -- Return in the name.
    local function save()
      local chosen = tostring(name.text or ""):match("^%s*(.-)%s*$")

      if chosen == "" then
        status.text = "a name is needed"
        return
      end

      if chosen:find("/") then
        status.text = "a name, not a path - use the list to choose where"
        return
      end

      finish(files.join(where, chosen))
    end

    name = ui.field{ x = 12, y = H - FOOT + 10, w = W - 24 - 2 * 104, h = 24,
                     text = spec.name or "untitled",
                     on_enter = function() save() end }
    win:add(name)

    win:add(ui.button{
      x = bx, y = H - FOOT + 10, w = 96, h = 24, text = "Save",
      on_click = function() save() end,
    })
  elseif mode == "folder" then
    -- The folder selected, or the one shown when there is none in it.
    local chooser = ui.button{
      x = bx, y = H - FOOT + 10, w = 96, h = 24, text = spec.button or "Choose",
      on_click = function()
        local e = entries[listing.selected]

        finish(e and files.join(where, e.name) or where)
      end,
    }

    win:add(chooser)
    panel.button_at = { x = chooser.x + chooser.w // 2, y = chooser.y + chooser.h // 2 }
  else
    win:add(ui.button{
      x = bx, y = H - FOOT + 10, w = 96, h = 24, text = "Open",
      on_click = function() act(entries[listing.selected]) end,
    })
  end

  win:add(ui.button{
    x = bx - 104, y = H - FOOT + 10, w = 96, h = 24, text = "Cancel",
    on_click = function()
      win:close()

      if spec.on_cancel then spec.on_cancel() end
    end,
  })

  win:add(status)
  show(where)
  win:focus_on(mode == "save" and name or listing)

  return win
end

function panel.save(spec)  return open(spec, "save") end
function panel.open(spec)  return open(spec, "open") end
function panel.folder(spec) return open(spec, "folder") end

return panel
