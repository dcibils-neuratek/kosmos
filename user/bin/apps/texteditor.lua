-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_StyledEdit
-- kosmos: opens txt md conf log
--
-- **Text Editor**: documents, in plain text or Markdown (`roadmap.md` 6zs,
-- drawn first as `docs/texteditor.html`).
--
--   wm texteditor                            a new document
--   wm texteditor:/Home/Documents/notes.md   one that exists
--
-- It was Editor, which edited Lua and ran it; the IDE does both properly
-- now (6n), so this is for letters, notes and lists, and wears the icon it
-- always had - BeOS's StyledEdit, whose job this is. A `.lua` opens in the
-- IDE.
--
-- **The page is `/Kosmos/Libraries/docview.lua`**: the text in the look's
-- text face, wrapped to a column about seventy characters wide in the
-- middle of the window, with a caret between characters. A document can
-- ask for the mono face instead, and keeps that on itself, as an attribute.
--
-- **Plain text is step 1**, and a Markdown document is written as text until
-- step 2 styles it as it is written. Its format follows its name: `.md` is
-- Markdown and anything else is text, and `Text | Markdown` switches it.
--
-- **One window per document**, as StyledEdit had. Open in a window that
-- holds an untouched new document fills it; anywhere else it opens another.

local ui       = use("/Kosmos/Libraries/ui.lua")
local panel    = use("/Kosmos/Libraries/panel.lua")
local docview  = use("/Kosmos/Libraries/docview.lua")
local textsize = use("/Kosmos/Libraries/textsize.lua")
local files    = use("/Kosmos/Libraries/files.lua")

local L = ui.layout

local SETTINGS  = "/Home/Preferences/texteditor"
local DOCUMENTS = "/Home/Documents"
local RECENT    = 8          -- documents Open recent remembers
local FIND_H    = 42         -- the find bar, under the header

local W, H = 760, 560

--------------------------------------------------------------------------
-- The document: where it is, what it is, and how it is shown.
--------------------------------------------------------------------------

local path = tostring(args or ""):match("^%s*(.-)%s*$")

if path == "" then path = nil end

local function base(p) return p and (p:match("([^/]+)$") or p) or "Untitled" end

local function markdown_name(p)
  local lower = tostring(p or ""):lower()

  return lower:match("%.md$") ~= nil or lower:match("%.markdown$") ~= nil
end

-- A new document is Markdown; one that exists is what its name says.
local markdown = (path == nil) or markdown_name(path)

local settings = fs.read(SETTINGS)

settings = (type(settings) == "table") and settings or {}

local function keep_settings()
  local now = fs.read(SETTINGS)

  now = (type(now) == "table") and now or {}
  now.wrap = settings.wrap
  now.recent = settings.recent
  settings = now

  local ok, why = fs.write(SETTINGS, now)

  if not ok then print("texteditor: settings not kept: " .. tostring(why)) end
end

-- What was opened lately, the newest first and each once.
local function remember(p)
  local list = { p }

  for _, q in ipairs(settings.recent or {}) do
    if q ~= p and #list < RECENT then list[#list + 1] = q end
  end

  settings.recent = list
  keep_settings()
end

local size = textsize.new(ui, SETTINGS, nil, "ui")

-- The mono face, when this document asked for it: kept on the document.
local function asked_mono(p)
  local attrs = p and fs.getattr(p)

  return type(attrs) == "table" and attrs.face == "mono"
end

local mono = asked_mono(path)

local body, unread = "", nil

if path then
  local got, why = fs.read(path)

  if type(got) == "string" then
    body = got
  elseif fs.getattr(path) then
    unread = tostring(why or "not text")
  end
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local win, err = ui.window{ title = base(path) .. " - Text Editor",
                            w = W, h = H, x = 110, y = 70,
                            header = true }

if not win then
  print("texteditor: " .. tostring(err))
  return
end

local header, format, note

-- What the header says beside the name: the format, how long, and whether
-- it is saved - or, until the next change, what the last thing done did.
local function describe(page)
  if note then return note end

  local state = page.dirty and "edited" or (path and "saved" or "not saved")

  return ("%s \u{b7} %d words \u{b7} %s"):format(markdown and "Markdown" or "Text",
                                                 page:words(), state)
end

local page = docview.new(ui, {
  x = 0, y = L.head, w = W, h = H - L.head,
  follow = { "left", "right", "top", "bottom" },
  text = body,
  wrap = (settings.wrap ~= false),
  face = function()
    if mono then return ui.sized("mono", size:size()) end

    return size:face()
  end,
  on_change = function(self)
    note = nil
    if header then header.sub = describe(self) end
  end,
})

win:add(page)

--------------------------------------------------------------------------
-- Saving and opening.
--------------------------------------------------------------------------

local function retitle()
  header.title = base(path)
  header.sub = describe(page)
  win:retitle(base(path) .. " - Text Editor")
end

-- The attribute that says this document is shown in the mono face, or its
-- absence. A drive that keeps no attributes keeps no preference either.
local function keep_face()
  if path then pcall(fs.setattr, path, { face = mono and "mono" or "" }) end
end

local save_as

local function save()
  if not path then return save_as() end

  local ok, why = fs.write(path, page:content())

  if ok then
    page:saved()
    keep_face()
    remember(path)
    note = nil
    header.sub = describe(page)
    print(("texteditor: saved %d lines to %s"):format(#page.lines, path))
  else
    note = "could not save: " .. tostring(why)
    header.sub = note
  end
end

-- The name a document is offered to be saved as: its own, with the ending
-- its format has - a Markdown document `.md`, a text one its own ending if
-- it has one that is not Markdown's (`.conf`, `.log`), `.txt` otherwise.
local function name_to_save()
  local name = path and base(path) or "Untitled"
  local stem = name:match("^(.+)%.[^.]+$") or name
  local ending = name:match("%.([^.]+)$")

  if markdown then
    return markdown_name(name) and name or (stem .. ".md")
  end

  if ending and not markdown_name(name) then return name end

  return stem .. ".txt"
end

function save_as()
  --
  -- **`/Home/Documents` first**, made if it is missing, as Tracker makes the
  -- places it keeps: a folder that only exists once somebody thinks to make
  -- it is a folder nobody makes.
  --
  local start = path and files.parent(path) or DOCUMENTS

  if not path and not fs.getattr(DOCUMENTS) then
    fs.send(DOCUMENTS, { type = "mkdir" })
  end

  if not fs.getattr(start) then start = "/Home" end

  local chooser = panel.save{
    start = start,
    name = name_to_save(),
    on_choose = function(chosen)
      path = chosen

      -- A name with an ending that says a format decides it; any other
      -- keeps the one chosen.
      if markdown_name(chosen) then
        markdown = true
      elseif chosen:lower():match("%.txt$") then
        markdown = false
      end

      format.on = markdown and 2 or 1
      save()
      retitle()
    end,
  }

  if chooser then chooser:run() end
end

local function launch(p)
  local ok, why = fs.send("/Running/wm", { type = "launch", program = "texteditor",
                                           args = p or "" })

  if not ok then
    note = "could not open a window: " .. tostring(why)
    header.sub = note
  end
end

-- A document into this window when this one holds nothing yet; into a
-- window of its own otherwise.
local function open_document(p)
  if path or page.dirty or page:content() ~= "\n" then return launch(p) end

  local got, why = fs.read(p)

  if type(got) ~= "string" then
    note = ("could not open %s: %s"):format(base(p), tostring(why or "not text"))
    header.sub = note
    return
  end

  path = p
  markdown = markdown_name(p)
  mono = asked_mono(p)
  format.on = markdown and 2 or 1
  page:set(got)
  remember(p)
  note = nil
  retitle()
end

local function open_file()
  local chooser = panel.open{
    start = path and files.parent(path) or (fs.getattr(DOCUMENTS) and DOCUMENTS or "/Home"),
    on_choose = open_document,
  }

  if chooser then chooser:run() end
end

local function show_in_tracker()
  if not path then return end

  fs.send("/Running/wm", { type = "launch", program = "tracker",
                           args = files.parent(path) })
end

--------------------------------------------------------------------------
-- Find and replace, under the header when it is asked for.
--------------------------------------------------------------------------

local finding = false

local find_field = ui.field{ x = 12, y = L.head + 8, w = 190, text = "",
                             hint = "Find", icon = "search" }
local count = ui.label{ x = 212, y = L.head + 8, w = 66, text = "" }
local prev = ui.iconbutton{ x = 282, y = L.head + 8, icon = "back" }
local nextb = ui.iconbutton{ x = 310, y = L.head + 8, icon = "forward" }
local with_field = ui.field{ x = 348, y = L.head + 8, w = 170, text = "",
                             hint = "Replace with" }
local replace_one = ui.button{ x = 526, y = L.head + 8, text = "Replace" }
local replace_every = ui.button{ x = 526, y = L.head + 8, text = "All" }
local close_find = ui.iconbutton{ x = W - 38, y = L.head + 8, icon = "close",
                                  follow = { "right", "top" } }

replace_every.x = replace_one.x + replace_one.w + 6

-- A label's words are drawn from its top, so it is put where they sit level
-- with the field's.
count.y = find_field.y + (find_field.h - count.h) // 2

local bar = { find_field, count, prev, nextb, with_field, replace_one,
              replace_every, close_find }

local function counted()
  local n = page:match_count()

  if find_field.text == "" then return "" end
  if n == 0 then return "none" end

  return ("%d of %d"):format(page.current or 0, n)
end

local function show_bar(on)
  finding = on

  for _, v in ipairs(bar) do v.hidden = not on end

  local top = L.head + (on and FIND_H or 0)

  page.h = page.h + page.y - top
  page.y = top
  page._insets.top = top

  if on then
    win:focus_on(find_field)

    if find_field.text ~= "" then page:find(find_field.text) end
  else
    page:find(nil)
    win:focus_on(page)
  end

  count.text = counted()
end

find_field.on_change = function(_, text)
  page:find(text)
  count.text = counted()
end

find_field.on_enter = function()
  page:step_match(1)
  count.text = counted()
end

prev.on_click = function() page:step_match(-1) count.text = counted() end
nextb.on_click = function() page:step_match(1) count.text = counted() end

replace_one.on_click = function()
  page:replace_match(with_field.text)
  count.text = counted()
end

replace_every.on_click = function()
  local n = page:replace_all(with_field.text)

  note = ("replaced %d"):format(n)
  header.sub = note
  count.text = counted()
end

close_find.on_click = function() show_bar(false) end

for _, v in ipairs(bar) do
  v.hidden = true
  win:add(v)
end

--------------------------------------------------------------------------
-- The header: the name, what it is, Text | Markdown, Save and the dots.
--------------------------------------------------------------------------

format = ui.segments{ items = { "Text", "Markdown" }, on = markdown and 2 or 1,
                      on_change = function(_, i)
                        markdown = (i == 2)
                        header.sub = describe(page)
                      end }

local more = ui.iconbutton{ icon = "more" }
local doc = ui.iconbutton{ icon = "document", on_click = show_in_tracker }

header = ui.header{
  x = 0, y = 0, w = W, title = base(path), sub = "",
  left = { doc },
  right = { format, ui.button{ text = "Save", on_click = save }, more },
  title_bar = true,
}

header.sub = unread and ("could not read it: " .. unread) or describe(page)

local function recent_items()
  local items = {}

  for _, p in ipairs(settings.recent or {}) do
    items[#items + 1] = { text = base(p), hint = files.parent(p),
                          on_choose = function() open_document(p) end }
  end

  if #items == 0 then items[1] = { text = "Nothing yet", disabled = true } end

  return items
end

local function on_off(b) return b and "On" or "Off" end

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "New window",   hint = "Ctrl N", on_choose = function() launch(nil) end },
    { text = "Open...",      hint = "Ctrl O", on_choose = function() open_file() end },
    { text = "Open recent",  submenu = recent_items() },
    { text = "Save as...",   on_choose = function() save_as() end },
    { separator = true },
    { text = "Find and replace", hint = "Ctrl F",
      on_choose = function() show_bar(true) end },
    { separator = true },
    { text = "Wrap long lines", hint = on_off(page.wrap),
      on_choose = function()
        page.wrap = not page.wrap
        settings.wrap = page.wrap
        keep_settings()
      end },
    { text = "Monospaced text", hint = on_off(mono),
      on_choose = function()
        mono = not mono
        keep_face()
      end },
    { text = "Text size", submenu = size:items() },
    { separator = true },
    { text = "Show in Tracker", disabled = (path == nil),
      on_choose = show_in_tracker },
  })
end

-- After the page, so the focus starts in it.
win:add(header)

--------------------------------------------------------------------------
-- A right click in the text: the clipboard, and all of it.
--------------------------------------------------------------------------

function page:on_context(x, y)
  win:open_menu(win.origin_x + self.x + x, win.origin_y + self.y + y, {
    { text = "Cut",        hint = "Ctrl X", on_choose = function() page:edit("cut") end },
    { text = "Copy",       hint = "Ctrl C", on_choose = function() page:edit("copy") end },
    { text = "Paste",      hint = "Ctrl V", on_choose = function() page:edit("paste") end },
    { separator = true },
    { text = "Select all", hint = "Ctrl A", on_choose = function() page:edit("selectall") end },
  })
  return true
end

--------------------------------------------------------------------------
-- Keys anywhere in the window, not only where the focus is: a save that
-- depends on which control was clicked last is a save work is lost to.
--------------------------------------------------------------------------

function win:on_key(c)
  if c == 19 then save() return true end                              -- Ctrl S
  if c == 14 then launch(nil) return true end                         -- Ctrl N
  if c == 15 then open_file() return true end                         -- Ctrl O
  if c == 6 then show_bar(true) return true end                       -- Ctrl F
  if c == ui.keywith(61, ui.CTRL) then size:step(1) return true end   -- Ctrl =
  if c == ui.keywith(45, ui.CTRL) then size:step(-1) return true end  -- Ctrl -
  if c == 27 and finding then show_bar(false) return true end         -- Escape

  return false
end

win:focus_on(page)
print(("texteditor: %s, %s, %d lines"):format(path or "a new document",
                                              markdown and "Markdown" or "text",
                                              #page.lines))
win:run()
