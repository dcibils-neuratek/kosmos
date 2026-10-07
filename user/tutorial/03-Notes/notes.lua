-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Notes
-- kosmos: opens note
--
-- Lesson 3 of the IDE's tutorial, finished: a page of text, kept in
-- /Home/Notes. Open and Save use the same window every application does,
-- and a note Tracker hands it arrives as its argument.

local ui    = use("/Kosmos/Libraries/ui.lua")
local panel = use("/Kosmos/Libraries/panel.lua")
local files = use("/Kosmos/Libraries/files.lua")

local NOTES = "/Home/Notes"

local win = ui.window{ title = "Notes", w = 560, h = 420, x = 220, y = 120 }

local new_button  = ui.button{ x = 16,  y = 12, text = "New" }
local open_button = ui.button{ x = 96,  y = 12, text = "Open" }
local save_button = ui.button{ x = 176, y = 12, text = "Save" }
local name = ui.label{ x = 270, y = 18, w = 274, text = "" }
local page = ui.editor{ x = 16, y = 56, w = 528, h = 348, gutter = false }

-- The note on the page: where it lives, or nil before it is first saved.
local path = files.words(args)[1]

local function show_name()
  local shown = path and path:match("([^/]+)$") or "Untitled"

  name.text = shown
  win:retitle(shown .. " - Notes")
  win.dirty = true
end

local function open(chosen)
  local body, why = fs.read(chosen)

  if type(body) ~= "string" then
    name.text = "could not open: " .. tostring(why)
    win.dirty = true
    return
  end

  path = chosen
  page:set(body)
  page:saved()
  show_name()
  print(("notes: opened %s, %d lines"):format(path, #page.lines))
end

local function write()
  local ok, why = fs.write(path, page:content())

  if ok then
    page:saved()
    print(("notes: saved %d lines to %s"):format(#page.lines, path))
  else
    name.text = "could not save: " .. tostring(why)
  end

  show_name()
end

-- The first save asks where; every one after writes where it was.
local function save()
  if path then return write() end

  if not fs.getattr(NOTES) then fs.send(NOTES, { type = "mkdir" }) end

  local chooser = panel.save{ start = NOTES, name = "Untitled.note",
                              on_choose = function(chosen) path = chosen; write() end }

  if chooser then chooser:run() end
end

new_button.on_click = function()
  path = nil
  page:set("")
  page:saved()
  show_name()
end

open_button.on_click = function()
  local chooser = panel.open{ start = fs.getattr(NOTES) and NOTES or "/Home",
                              filter = function(file) return file:match("%.note$") end,
                              on_choose = open }

  if chooser then chooser:run() end
end

save_button.on_click = save

win:add(new_button)
win:add(open_button)
win:add(save_button)
win:add(name)
win:add(page)

if path then open(path) else show_name() end

win:run()
