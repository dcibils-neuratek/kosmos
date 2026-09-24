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
-- System and Drives, from `/lib/sidebar.lua` - the path as a trail, and the
-- folder as Name, Size and Kind. What the application is handed is the file
-- at its real place, so no application has to know that MyPhotos exists.
--
-- **One click selects; a second click, or Enter, opens** - a folder is
-- entered and a file chosen - as in Tracker and its sidebar (`ui.md`
-- 16.8c). The button at the bottom right does the same to the selection.
--
--   panel.open{ start = "/home/roms/snes", title = "Open ROM",
--               filter = function(name) return name:match("%.sfc$") end,
--               on_choose = function(path) ... end }
--   panel.save{ start = "/home", name = "untitled.lua",
--               on_choose = function(path) ... end }
--
-- The callback gets a whole path or is never called: Cancel closes the
-- window and calls `on_cancel`, for a caller that wants to know. `filter`
-- hides files it answers false for; folders always show, because a folder
-- is how you reach the files.

local ui      = use("/lib/ui.lua")
local files   = use("/lib/files.lua")
local types   = use("/lib/filetypes.lua")
local sidebar = use("/lib/sidebar.lua")

local theme = ui.theme

local panel = {}

local W, H   = 640, 420
local SIDE_W = 190
local TOP    = 34                     -- the trail above, the panes below
local FOOT   = 72                     -- the name, the buttons, the status

--
-- A name cut to fit its column, ending in `...` when it was cut - and never
-- in the middle of a UTF-8 character, because a FAT long name arrives as
-- UTF-8 and half a character draws as rubbish.
--
local function fitted(text, room)
  if gfx.measure(text) <= room then return text end

  while #text > 1 and gfx.measure(text .. "...") > room do
    text = text:sub(1, -2)

    while #text > 1 and text:byte(-1) >= 0x80 and text:byte(-1) < 0xC0 do
      text = text:sub(1, -2)
    end

    if #text > 0 and text:byte(-1) >= 0xC0 then text = text:sub(1, -2) end
  end

  return text .. "..."
end

local function open(spec, mode)
  local win, err = ui.window{
    title = spec.title or (mode == "save" and "Save" or "Open"),
    w = W, h = H,
    x = spec.x or 200, y = spec.y or 120,
  }

  if not win then return nil, err end

  local where   = spec.start or "/home"
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

  local function show(path)
    local found, why = files.entries(path)

    if not found then
      status.text = tostring(why)
      return
    end

    local shown = {}

    for _, e in ipairs(found) do
      if e.kind == "directory" or not spec.filter or spec.filter(e.name) then
        shown[#shown + 1] = e
      end
    end

    where, entries = path, shown
    trail.text       = path
    listing.items    = shown
    listing.selected = 1
    listing.top      = 1
    status.text      = (#shown == 0) and "empty" or ""
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
  end

  listing.on_open  = function(_, e) act(e) end
  trail.on_visit   = function(path) show(path) end
  tree.on_select   = function(_, node) if node.path then show(node.path) end end

  win:add(trail)
  win:add(tree)
  win:add(header)
  win:add(listing)

  local bx = W - 12 - 96

  if mode == "save" then
    name = ui.field{ x = 12, y = H - FOOT + 10, w = W - 24 - 2 * 104, h = 24,
                     text = spec.name or "untitled" }
    win:add(name)

    win:add(ui.button{
      x = bx, y = H - FOOT + 10, w = 96, h = 24, text = "Save",
      on_click = function()
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
      end,
    })
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

  return win
end

function panel.save(spec)  return open(spec, "save") end
function panel.open(spec)  return open(spec, "open") end

return panel
