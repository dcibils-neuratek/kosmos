-- A file panel: choosing where something goes, or which one to open.
--
-- BeOS called this a BFilePanel and Tracker provided it, so that every
-- application's open-and-save looked the same because it *was* the same.
-- Here it is a library rather than a service for one reason: a panel served
-- by another process would have to draw into this application's window or
-- open one of its own, and the second is a window the application cannot
-- place, size, or close when it goes away.
--
-- So it is a window of its own that this application owns, and what it
-- shares with Tracker is `files.lua` - the answers to what is in a
-- directory - rather than the pixels.
--
--   panel.save{ start = "/home", name = "untitled.lua",
--               on_choose = function(path) ... end }
--
-- The callback gets a whole path or is never called. There is no "cancel"
-- branch to write in every caller: closing the panel is cancelling, and a
-- caller that wants to know can pass `on_cancel`.

local ui    = use("/lib/ui.lua")
local files = use("/lib/files.lua")

local panel = {}

local W, H = 400, 320

local function open(spec, mode)
  local win, err = ui.window{
    title = spec.title or (mode == "save" and "Save" or "Open"),
    w = W, h = H,
    x = spec.x or 260, y = spec.y or 160,
  }

  if not win then return nil, err end

  local where   = spec.start or "/home"
  local entries = {}

  local here    = ui.label{ x = 12, y = 10, w = W - 24, text = where }
  local status  = ui.label{ x = 12, y = H - 32, w = W - 24, text = "" }
  local listing = ui.list{ x = 12, y = 56, w = W - 24,
                           h = H - (mode == "save" and 140 or 110), items = {} }

  local name

  local function show(path)
    local found, why = files.entries(path)

    if not found then
      status.text = tostring(why)
      return
    end

    where, entries = path, found

    local labels = {}
    for i, e in ipairs(found) do labels[i] = files.label(e) end

    here.text        = path
    listing.items    = labels
    listing.selected = 1
    listing.top      = 1
    status.text      = (#found == 0) and "empty" or ""
  end

  -- A directory is entered. A file is *chosen* when opening and its name is
  -- taken when saving, which is the whole difference between the two modes
  -- and the reason they are one function.
  listing.on_select = function(self, item, index)
    local entry = entries[index]

    if not entry then return end

    if entry.kind == "directory" then
      show(files.join(where, entry.name))
    elseif mode == "save" then
      name.text = entry.name
    else
      win:close()

      if spec.on_choose then
        spec.on_choose(files.join(where, entry.name))
      end
    end
  end

  win:add(here)

  win:add(ui.button{
    x = 12, y = 28, w = 60, h = 22, text = "Up",
    on_click = function()
      if where ~= "/" then show(files.parent(where)) end
    end,
  })

  win:add(ui.button{
    x = 80, y = 28, w = 70, h = 22, text = "Home",
    on_click = function() show("/home") end,
  })

  win:add(listing)

  if mode == "save" then
    name = ui.field{ x = 12, y = H - 62, w = W - 130, h = 24,
                     text = spec.name or "untitled" }
    win:add(name)

    win:add(ui.button{
      x = W - 108, y = H - 62, w = 96, h = 24, text = "Save",
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

        win:close()

        if spec.on_choose then
          spec.on_choose(files.join(where, chosen))
        end
      end,
    })
  end

  win:add(status)
  show(where)

  return win
end

function panel.save(spec)  return open(spec, "save") end
function panel.open(spec)  return open(spec, "open") end

return panel
