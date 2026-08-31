-- kosmos: application
-- Tracker: the file manager.
--
--   wm tracker            opens at /home
--   wm tracker:/data      or wherever
--
-- The name is BeOS's and so is the job. What used to be called `tracker`
-- here was a replicant host and is now `adopt`, which named what it did
-- instead of the behaviour it borrowed - and left this name for the thing
-- it belongs to.
--
-- It knows nothing about disks. Every question it asks is the ordinary
-- filesystem protocol through its own namespace, so it browses `/home` on
-- the disk and `/data` in memory and `/bin` in the image with the same
-- code, and could browse a directory served from another machine without
-- noticing.

local ui    = use("/lib/ui.lua")
local files = use("/lib/files.lua")
local theme = ui.theme

local W, H = 420, 340

local where = args:match("^%s*(%S+)") or "/home"

local win, err = ui.window{ title = "Tracker", w = W, h = H, x = 90, y = 70 }

if not win then
  print("tracker: " .. tostring(err))
  return
end

local here     = ui.label{ x = 12, y = 10, w = W - 24, text = where }
local status   = ui.label{ x = 12, y = H - 34, w = W - 24, text = "" }
local listing  = ui.list{ x = 12, y = 58, w = W - 24, h = H - 110, items = {} }

local entries = {}

local function show(path)
  local found, why = files.entries(path)

  if not found then
    status.text = tostring(why)
    return
  end

  where   = path
  entries = found

  local labels = {}

  for i, e in ipairs(found) do
    labels[i] = files.label(e)
  end

  here.text        = path
  listing.items    = labels
  listing.selected = 1
  listing.top      = 1

  status.text = (#found == 0) and "empty"
                or ("%d item%s"):format(#found, #found == 1 and "" or "s")
end

-- Selecting a directory goes into it; selecting a file says what it is.
--
-- One click rather than two. BeOS wanted a double click and this kit has no
-- notion of one - and adding it to serve a single caller would be a widget
-- change made for an application, which is the wrong way round. A single
-- click is also what the keyboard does, so both roads arrive the same way.
listing.on_select = function(self, item, index)
  local entry = entries[index]

  if not entry then return end

  if entry.kind == "directory" then
    show(files.join(where, entry.name))
  else
    status.text = entry.name .. ": " .. files.describe(entry)
  end
end

win:add(here)

win:add(ui.button{
  x = 12, y = 28, w = 60, h = 24, text = "Up",
  on_click = function()
    if where ~= "/" then show(files.parent(where)) end
  end,
})

win:add(ui.button{
  x = 80, y = 28, w = 80, h = 24, text = "Home",
  on_click = function() show("/home") end,
})

win:add(ui.button{
  x = 168, y = 28, w = 80, h = 24, text = "Refresh",
  on_click = function() show(where) end,
})

win:add(listing)
win:add(status)

show(where)
win:run()
