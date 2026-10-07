-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pe
-- kosmos: name Kosmos IDE
-- kosmos: section development
-- kosmos: opens lua
--
-- **Kosmos IDE**: where Lua for Kosmos is written and run.
--
--   wm ide                        the last project, or the first one
--   wm ide:/Home/development      that folder as the project
--   wm ide:/Home/development/a.lua    that file, and its folder as the project
--
-- **Step 3, Run and Stop**: the file runs as its own process, as it is on
-- the screen, with this window as its console - a program's `print` comes
-- to the Output panel as it would to a Terminal, an application's window
-- opens on the desktop - and how it ended is said, with the line of an
-- error marked and a click away.
--
-- **Step 4, checking**: Lua's own parser a moment after typing stops, and
-- luacheck - told what a Kosmos program is given - when a file opens, when
-- it is saved and on F7 (`/Kosmos/Libraries/lint.lua`). Each problem is marked on its
-- line and listed in Problems, a click from the line.
--
-- **Step 5, suggestions**: after `ui.` the names `ui.lua` defines, after
-- `win:` a window's methods, each with the comment above it in its source
-- as what it is (`/Kosmos/Libraries/libdoc.lua`) - read, never run. And the drawing's
-- third check: a name a library does not have, `ui.slidr`, asked of the
-- library and answered with the nearest name it does have.
--
-- **Find a file**, asked for after using it: Ctrl P and any part of a name,
-- over the project, `/bin` and `/Kosmos/Libraries`, each found file with where it lives
-- and what it is.
--
-- `docs/kosmos-ide.html` is the drawing, agreed as drawn ("the mockup is
-- perfect!!!"), and `roadmap.md` 6n the steps. This is step 2, the window:
-- the project as a tree with Kosmos itself under it to read, a tab a file,
-- the editor - `ui.editor` in its code look, the component step 1 grew -
-- the Output panel below, and the button bar along the header. Opening,
-- saving, and the project and its files remembered for next time.
-- Run and Stop are step 3, checking step 4, suggestions step 5; their
-- buttons are here, as the drawing has them, and say so until they work.
--
-- **An ordinary window**, resized, minimised and maximised as needed, which
-- is what settled that it is built from the kit's widgets: the window
-- manager resizes a window that sends it drawing and never one with a
-- surface of its own. Every piece follows the edges it should, so a bigger
-- window is a bigger editor.
--
-- **Kosmos is in the tree to be read**: `/bin`, `/Kosmos/Libraries` and the kits, under
-- the project, read only - so the way to learn what `ui.slider` does is to
-- open `ui.lua` and read it.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local lint = use("/Kosmos/Libraries/lint.lua")
local libdoc = use("/Kosmos/Libraries/libdoc.lua")
-- A project's C: coloured by `clex`, its headers read by `cdoc` for what is
-- suggested as it is typed.
local clex = use("/Kosmos/Libraries/clex.lua")
local cdoc = use("/Kosmos/Libraries/cdoc.lua")
local lualex = use("/Kosmos/Libraries/lualex.lua")
local panel = use("/Kosmos/Libraries/panel.lua")
local theme = ui.theme
local L = ui.layout

--------------------------------------------------------------------------
-- What is remembered: the project, its open files, the one in front.
-- A Lua table written with `fs.write`, like every settings file here.
--------------------------------------------------------------------------

local prefs = use("/Kosmos/Libraries/prefs.lua")

--
-- **Its text larger and smaller**, as Terminal's and Log View's is: Diego,
-- 27 September, "we need a way to increase font size like we have in the
-- terminal app". `/Kosmos/Libraries/textsize.lua`'s steps and its menu, in the dots, and
-- Ctrl = and Ctrl - besides; kept in settings of its own, `ide-text`, since
-- `ide` is the project's memory. Every editor asks for the face as it draws.
-- (It was `/Home/.ide-text`, outside Preferences altogether, until the
-- settings kit.)
--
local textsize = use("/Kosmos/Libraries/textsize.lua")
local text                    -- declared first: the callback below names it

text = textsize.new(ui, "ide-text", function()
  print(("ide: text %d px"):format(text:size()))
end)

-- The first project: where the tutorial's lessons will be (`roadmap.md` 7),
-- made if it is not there, so the tree has somewhere to stand.
local FIRST = "/Home/development"

local remembered = prefs.read("ide")

local asked = files.words(args)[1]

-- `ide new`: the remembered project, with New Project open over it.
local asked_new = asked == "new"
if asked_new then asked = nil end

-- A file asked for is opened, and the folder it is in is the project.
local asked_file = nil

if asked then
  local attrs = fs.getattr(asked)

  if attrs and attrs.kind ~= "directory" then
    asked_file = asked
    asked = asked:match("^(.*)/[^/]*$")
    if asked == "" then asked = "/" end
  end
end

local project = asked or remembered.project or FIRST

if not fs.getattr(project) then
  fs.send(project, { type = "mkdir" })
end

-- Places a Kosmos program reads and does not write.
local READ_ONLY = { "/Kosmos" }

-- `path` is `top` or inside it, whatever the case of either: a name is
-- found whatever its case (`roadmap.md` 6s), so `/kosmos/libraries/ui.lua`
-- typed is the same file, and just as read only.
local function under(path, top)
  local p, t = path:lower(), top:lower()

  return p == t or p:sub(1, #t + 1) == t .. "/"
end

local function read_only(path)
  for _, top in ipairs(READ_ONLY) do
    if under(path, top) then return true end
  end

  return false
end

local function base(p) return p:match("([^/]+)$") or p end

-- A path inside the project, as the header says it.
local function relative(path)
  if path:sub(1, #project + 1) == project .. "/" then
    return path:sub(#project + 2)
  end

  return path
end

--------------------------------------------------------------------------
-- The window, in the drawing's measures.
--------------------------------------------------------------------------

local W, H = 1180, 760
local SIDE = 260
local TAB_H = 32
local BOTTOM = 176
local FOOT = 30

local win, err = ui.window{ title = "Kosmos IDE", w = W, h = H, x = 70, y = 50,
                            header = true }

if not win then
  print("ide: " .. tostring(err))
  return
end

-- **The tools' row under the header, and the find bar under that when it
-- is open** (`docs/ide-layout.html`, 7 October): Kosmos Write's tools, at
-- `ui.TOOL_H` in a row 64 high, and a bar of the find's fields.
local TOOLS_H = 64
local FIND_H = 40

-- What is shown decides where everything goes: the sidebar, and the bar.
local side_on, bar_on = true, false
local place_body

local function top() return L.head + TOOLS_H + (bar_on and FIND_H or 0) end

local BODY = L.head + TOOLS_H + TAB_H

-- A widget put somewhere, and the distances to the window's edges it keeps
-- as the window is resized taken from there - as `view:add` takes them.
local function put(v, x, y, w, h)
  local rw, rh = win.root.w, win.root.h

  v.x, v.y, v.w, v.h = x, y, w, h
  v._insets = { left = x, top = y, right = rw - (x + w), bottom = rh - (y + h) }
end

-- Where an editor goes, now.
local function editor_box()
  local sx = side_on and SIDE or 0
  local y = top() + TAB_H

  return sx, y, win.root.w - sx, win.root.h - y - BOTTOM - FOOT
end

-- The open files, in their tabs' order: `{ path =, editor = }`.
local open = {}
local current = nil

-- Checking, which opening and saving ask for and which is written below.
local check_now

-- And suggestions, which each editor is given as it opens.
local wire_suggestions

-- Said in the Output panel, and in the log for whoever drives this.
local output

-- The Output panel keeps this many lines, and then starts again with a
-- line saying so, rather than growing as long as a program talks.
local OUTPUT_MOST = 5000

local function out(text, colour)
  if not output then return end

  if #output.lines > OUTPUT_MOST then
    output:set("")
    output:append(("(the Output was cleared at %d lines)\n"):format(OUTPUT_MOST),
                  theme.text_dim)
  end

  output:append(text, colour)
end

local function say(text, colour)
  out(text .. "\n", colour)
  print("ide: " .. text)
end

--------------------------------------------------------------------------
-- Remembering.
--------------------------------------------------------------------------

local function remember()
  local paths = {}

  for _, f in ipairs(open) do paths[#paths + 1] = f.path end

  prefs.write("ide", { project = project, files = paths,
                       current = current and current.path or nil })
end

--------------------------------------------------------------------------
-- The file tabs and the editors behind them. An editor a file, so each
-- keeps its own undo and its own place; only the one in front is shown.
--------------------------------------------------------------------------

local tabs = ui.tabs{ x = SIDE, y = L.head + TOOLS_H, w = W - SIDE, items = {} }
local show

local function sync_tabs()
  local items = {}

  for i, f in ipairs(open) do
    items[i] = { text = base(f.path), close = true,
                 changed = f.editor.dirty or nil }
  end

  tabs.items = items

  for i, f in ipairs(open) do
    if f == current then tabs.on = i end
  end
end

function show(f)
  for _, o in ipairs(open) do o.editor.hidden = (o ~= f) end

  current = f
  sync_tabs()

  if f then win:focus_on(f.editor) end

  remember()
end

local function find_open(path)
  for i, f in ipairs(open) do
    if f.path == path then return f, i end
  end
end

local function open_file(path)
  local already = find_open(path)

  if already then
    show(already)
    return already
  end

  local body, why = fs.read(path)

  if type(body) ~= "string" then
    say(("could not open %s: %s"):format(path, tostring(why or "not text")))
    return nil
  end

  --
  -- **At the window's size now**, not the size it opened at: a child keeps
  -- the distances to the edges it had when it was added, so an editor made
  -- at the first size in a window since maximised would stay that small.
  --
  local ex, ey, ew, eh = editor_box()
  local editor = ui.editor{
    x = ex, y = ey, w = ew, h = eh,
    follow = { "left", "right", "top", "bottom" },
    code = (path:match("%.lua$") and "lua") or (path:match("%.[ch]$") and "c") or nil,
    read_only = read_only(path),
    text = body,
    face = function() return text:face() end,
  }

  -- Every file shows the same place in the window; the drawing's editor
  -- has its text 6 in from the top.
  editor.hidden = true
  win:add(editor)

  -- Where it is, for whoever drives the IDE from outside - its scrollbar
  -- is at the right edge (`tools/run_ide.py`).
  print(("ide: editor at %d,%d %dx%d"):format(editor.x, editor.y, editor.w, editor.h))

  if wire_suggestions then wire_suggestions(editor) end

  local f = { path = path, editor = editor }

  open[#open + 1] = f
  show(f)
  say(("opened %s%s"):format(path, editor.read_only and ", read only" or ""))

  if check_now then check_now(f) end

  return f
end

-- A tab's file closed. One with changes asks first, by being refused once:
-- the second press closes it without them.
local function close_file(f)
  if f.editor.dirty and not f.asked then
    f.asked = true
    say(("%s is not saved - close it again to leave the changes"):format(base(f.path)))
    return
  end

  local _, i = find_open(f.path)

  win:remove(f.editor)
  table.remove(open, i)
  say(("closed %s"):format(base(f.path)))

  if current == f then
    show(open[math.min(i, #open)])
  else
    sync_tabs()
    remember()
  end
end

tabs.on_choose = function(_, i) show(open[i]) end
tabs.on_close = function(_, i) close_file(open[i]) end

--------------------------------------------------------------------------
-- Saving.
--------------------------------------------------------------------------

local function save(f)
  f = f or current

  if not f then return false end

  if f.editor.read_only then
    say(("%s is Kosmos's own and read only"):format(f.path))
    return false
  end

  local ok, why = fs.write(f.path, f.editor:content())

  if not ok then
    say(("could not save %s: %s"):format(f.path, tostring(why)))
    return false
  end

  f.editor:saved()
  f.asked = nil
  sync_tabs()

  if check_now then check_now(f) end
  say(("saved %s, %d lines"):format(relative(f.path), #f.editor.lines))
  return true
end

local function save_all()
  for _, f in ipairs(open) do
    if f.editor.dirty then save(f) end
  end
end

--------------------------------------------------------------------------
-- The side: the project and Kosmos, as a tree; the file's functions, as
-- an outline.
--------------------------------------------------------------------------

local function folder(path, name, note)
  return {
    text = name or base(path), path = path, note = note,
    children = function(node)
      local kids = {}

      for _, e in ipairs(files.entries(node.path) or {}) do
        local child = files.join(node.path, e.name)

        if e.kind == "directory" then
          kids[#kids + 1] = folder(child)
        elseif not e.name:match("^%.") then
          kids[#kids + 1] = { text = e.name, path = child }
        end
      end

      return kids
    end,
  }
end

local side = ui.tabs{ x = 0, y = L.head + TOOLS_H, w = SIDE,
                      follow = { "left", "top" },
                      items = { { text = "Project" }, { text = "Outline" },
                                { text = "System" } } }

-- **The project, and only the project** (Diego, 7 October: "the sidebar
-- shows a lot of kosmos apps and kosmos programs in the project view where
-- it should be empty").
local tree = ui.tree{
  x = 0, y = BODY, w = SIDE, h = H - BODY - FOOT,
  follow = { "left", "top", "bottom" },
  roots = {
    (function()
      local root = folder(project, base(project), project:match("^(.*)/"))

      root.open = true
      return root
    end)(),
  },
}

-- **Kosmos's own files, in a tab of their own** - "perhaps we can show
-- those files in another tab in the sidebar called system files so user can
-- read and access those easily": read only, opened as they were.
local system = ui.tree{
  x = 0, y = BODY, w = SIDE, h = H - BODY - FOOT,
  follow = { "left", "top", "bottom" },
  roots = {
    folder("/Kosmos/Apps", "/Kosmos/Apps", "read only"),
    folder("/Kosmos/Programs", "/Kosmos/Programs", "read only"),
    folder("/Kosmos/Libraries", "/Kosmos/Libraries", "read only"),
    -- The kits are C, reached with `use("/Kosmos/Kits/...")`: named here, with
    -- nothing to open, so a person can see what there is to use.
    {
      text = "/Kosmos/Kits", note = "C",
      children = function()
        local kids = {}

        for _, name in ipairs(sys.kit_names and sys.kit_names() or {}) do
          kids[#kids + 1] = { text = name, quiet = true, note = "C" }
        end

        return kids
      end,
    },
    folder("/Kosmos/Templates", "/Kosmos/Templates", "read only"),
    folder("/Kosmos/Examples", "/Kosmos/Examples", "read only"),
  },
}

system.hidden = true

-- A file chosen in either tree is opened; a folder opens with its arrow.
tree.on_select = function(_, node)
  if node.path and not node.children then open_file(node.path) end
end

system.on_select = tree.on_select

-- The outline: the file's functions, in order, a press away.
local outline = ui.tree{
  x = 0, y = BODY, w = SIDE, h = H - BODY - FOOT,
  follow = { "left", "top", "bottom" },
  roots = {},
}

outline.hidden = true

local function outline_of(f)
  local rows = {}

  if not f then return rows end

  local c = f.editor.code == "c"

  for n, line in ipairs(f.editor.lines) do
    local name

    if c then
      -- A function defined at the top: a head that starts the line, a name
      -- and its `(`, and no `;` - which would make it a declaration.
      name = not line:find(";%s*$") and not line:match("^%s*#")
             and line:match("^[%a_][%w_%s%*]-([%a_][%w_]*)%s*%(")

      if name and clex.KEYWORDS[name] then name = nil end
    else
      name = line:match("^%s*local%s+function%s+([%w_%.:]+)")
             or line:match("^%s*function%s+([%w_%.:]+)")
             or line:match("^%s*([%w_%.]+)%s*=%s*function")
    end

    if name then rows[#rows + 1] = { text = name, line = n, note = tostring(n) } end
  end

  if #rows == 0 then rows[1] = { text = "no functions", quiet = true } end

  return rows
end

outline.on_select = function(_, node)
  if node.line and current then
    current.editor:go_to(node.line, 1)
    win:focus_on(current.editor)
  end
end

side.on_choose = function(_, i)
  tree.hidden = (i ~= 1) or not side_on
  outline.hidden = (i ~= 2) or not side_on
  system.hidden = (i ~= 3) or not side_on

  if i == 2 then outline.roots = outline_of(current) end
end

--------------------------------------------------------------------------
-- The panel below: Output, and Problems for step 4.
--------------------------------------------------------------------------

local bottom_tabs = ui.tabs{
  x = SIDE, y = H - FOOT - BOTTOM, w = W - SIDE,
  follow = { "left", "right", "bottom" },
  items = { { text = "Output" }, { text = "Problems" }, { text = "Console" },
            { text = "Search" } },
}

output = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "", face = function() return text:face() end,
}

local problems = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "", face = function() return text:face() end,
}

problems.hidden = true

--
-- **The Console** (`docs/ide-layout.html`): what a running program prints,
-- and a line under it for what it asks for - a program that reads waits
-- until Enter, rather than being told nobody will answer.
--
local CONSOLE_IN = 30

local console_view = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H - CONSOLE_IN,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "", face = function() return text:face() end,
}

local console_in = ui.field{
  x = SIDE + 10, y = H - FOOT - CONSOLE_IN + 2, w = W - SIDE - 20, h = 26,
  follow = { "left", "right", "bottom" },
  text = "", hint = "nothing is running",
}

console_view.hidden, console_in.hidden = true, true

-- **Search**: what Find found in the project, a line a match; a click on
-- one opens its file there.
local search_view = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "", face = function() return text:face() end,
}

search_view.hidden = true

local function bottom_show(i)
  bottom_tabs.on = i
  output.hidden = (i ~= 1)
  problems.hidden = (i ~= 2)
  console_view.hidden, console_in.hidden = (i ~= 3), (i ~= 3)
  search_view.hidden = (i ~= 4)
end

bottom_tabs.on_choose = function(_, i) bottom_show(i) end

--------------------------------------------------------------------------
-- The foot: where the caret is, what the file is, and the keys.
--------------------------------------------------------------------------

local foot = ui.view{ x = 0, y = H - FOOT, w = W, h = FOOT,
                      follow = { "left", "right", "bottom" } }

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local ty = (self.h - gfx.height("ui")) // 2
  local words

  if current then
    local b = current.editor.buf
    local counts = current.counts
    local said = "no problems"

    if counts and counts[1] + counts[2] > 0 then
      said = ("%d error%s, %d warning%s"):format(counts[1], counts[1] == 1 and "" or "s",
                                                counts[2], counts[2] == 1 and "" or "s")
    end

    words = ("Ln %d, Col %d    %s    2 spaces    %s%s"):format(b.cy, b.cx,
            (current.editor.code == "lua" and "Lua 5.4")
              or (current.editor.code == "c" and "C, built by TinyCC") or "Text",
            current.editor.code == "lua" and said or "",
            current.editor.read_only and "    read only" or "")
  else
    words = "no file open - choose one in the tree"
  end

  g:text(14, ty, words, theme.text_dim, nil, "ui")

  local keys = "F6 build    Ctrl Enter run    Ctrl F find    Ctrl / comment    Ctrl P find a file"

  g:text(self.w - 14 - gfx.measure(keys, "ui"), ty, keys, theme.text_dim, nil, "ui")
end

--------------------------------------------------------------------------
-- The header and its button bar, as the drawing has it: the subject and
-- where you are, then the file verbs, the edit verbs, and Run, Stop and
-- Check with a word saying what is running; the dots at the far end.
--------------------------------------------------------------------------

local new_project                  -- below, once the dialog is made

local function new_file()
  local chooser = panel.save{
    start = project,
    name = "untitled.lua",
    on_choose = function(chosen)
      if not fs.getattr(chosen) then fs.write(chosen, "") end
      open_file(chosen)
    end,
  }

  if chooser then chooser:run() end
end

local function open_chosen()
  local chooser = panel.open{
    start = current and current.path:match("^(.*)/") or project,
    on_choose = function(chosen) open_file(chosen) end,
  }

  if chooser then chooser:run() end
end

local pill = ui.view{ w = gfx.measure("not running", "ui") + 18, h = 22 }

pill.words = "not running"

function pill:draw(g)
  local colour = self.live and theme.good or theme.text_dim

  g:frame_round(0, 0, self.w, self.h, self.live and theme.good or theme.line_soft, 11)
  g:text(9, (self.h - gfx.height("ui")) // 2, self.words, colour, nil, "ui")
end

local function icon(name, action)
  return ui.iconbutton{ icon = name, w = 30, h = 30, on_click = action }
end

-- **The tools**, Kosmos Write's (`docs/ide-layout.html`, 7 October; Diego:
-- "we should put buttons as the style of kosmos write toolbar which are
-- small and easy to read"): an icon and its word, the keys in the More
-- menu and the foot. Not `run`: that is the function a program starts
-- another with.
local run_button = ui.tool{ text = "Run", icon = "run", go = true }
local stop = ui.tool{ text = "Stop", icon = "stop", disabled = true }
local check = ui.tool{ text = "Check", icon = "check" }
local build_button = ui.tool{ text = "Build", icon = "settings" }

-- Finding a file by its name, at the header's right end where Tracker keeps
-- its Search; what it finds and how are further down, under "Finding a file".
local FIND_FIELD = 240
local find = ui.field{ w = FIND_FIELD, text = "", hint = "Find a file", icon = "search" }

-- The find bar's doors, written further down with the bar; and the
-- tutorial's, with New Project.
local open_bar, close_bar, bar_step
local TUTORIAL

-- **⋯ More**: what the tools do not show, grouped as a menu bar would group
-- it - Kosmos has no menu bars (`roadmap.md` 5zj) - each with its key.
local function more_items()
  local items = {
    { text = "Open\u{2026}", hint = "Ctrl O", on_choose = open_chosen },
    { text = "Save", hint = "Ctrl S", on_choose = function() save() end },
    { text = "Save All", on_choose = save_all },
    { text = "Close Tab", hint = "Ctrl W twice",
      on_choose = function() if current then close_file(current) end end },
    { separator = true },
    { text = "Find", hint = "Ctrl F", on_choose = function() open_bar("file") end },
    { text = "Replace", hint = "Ctrl R", on_choose = function() open_bar("file", true) end },
    { text = "Find in Project", hint = "Ctrl Shift F", on_choose = function() open_bar("project") end },
    { text = "Go to Line", hint = "Ctrl G", on_choose = function() open_bar("line") end },
    { text = "Comment", hint = "Ctrl /", on_choose = function()
      if current then current.editor:key(ui.keywith(47, ui.CTRL)) end
    end },
    { separator = true },
  }

  for _, it in ipairs(text:items()) do items[#items + 1] = it end

  -- Help: the tutorial's pages, and each lesson's project (`roadmap.md`
  -- item 7). The two after are the tutorial's later parts'.
  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Tutorial", hint = "F1", on_choose = function() TUTORIAL.open() end }
  items[#items + 1] = { text = "Lesson's Project", submenu = TUTORIAL.items() }
  items[#items + 1] = { text = "Kits and their calls", disabled = true }
  items[#items + 1] = { text = "Keyboard Shortcuts", disabled = true }

  return items
end

local header = ui.header{
  x = 0, y = 0, w = W, title = "Kosmos IDE", sub = "",
  -- Room for the project and the file, so what is after them stays where it
  -- is as the file in front changes; a longer path is cut.
  sub_room = 300,
  after = { pill },
  right = { find, icon("more", function(self)
    win:open_menu(win.origin_x + self.x, win.origin_y + self.y + self.h, more_items())
  end) },
  title_bar = true,
}

-- The row the tools stand in, under the header.
local toolbar = ui.view{ x = 0, y = L.head, w = W, h = TOOLS_H,
                         follow = { "left", "right", "top" } }

function toolbar:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, self.h - 1, self.w, 1, theme.line_soft)
end

local function tool(text_, icon_, action)
  return ui.tool{ text = text_, icon = icon_, on_click = action }
end

local function toggle_side()
  side_on = not side_on
  place_body()
end

local tools_left = {
  tool("New", "new", function(self)
    win:open_menu(win.origin_x + self.x, win.origin_y + self.y + self.h, {
      { text = "New File", hint = "Ctrl N", on_choose = new_file },
      { text = "New Project\u{2026}", on_choose = function() new_project() end },
    })
  end),
  tool("Open", "open", open_chosen),
  tool("Save", "save", function() save() end),
  false,
  tool("Undo", "undo", function() if current then current.editor:undo() end end),
  tool("Redo", "redo", function() if current then current.editor:redo() end end),
  tool("Find", "search", function() open_bar("file") end),
  false,
  build_button, run_button, stop, check,
}

local tools_right = {
  tool("Console", "keyboard", function() bottom_show(3) end),
  tool("Sidebar", "sidebar", function() toggle_side() end),
}

-- A thin line between groups of tools.
local tool_lines = {}

do
  local x, y = 12, L.head + (TOOLS_H - ui.TOOL_H) // 2

  for _, t in ipairs(tools_left) do
    if t then
      t.x, t.y = x, y
      x = x + t.w + 2
    else
      local line = ui.view{ x = x + 8, y = y + 9, w = 1, h = 30 }

      function line:draw(g) g:fill(0, 0, 1, self.h, theme.line_soft) end

      tool_lines[#tool_lines + 1] = line
      x = x + 17
    end
  end

  x = W - 12

  for i = #tools_right, 1, -1 do
    local t = tools_right[i]

    x = x - t.w
    t.x, t.y = x, y
    t.follow = { right = true, top = true }
    x = x - 2
  end
end

--------------------------------------------------------------------------
-- Running (step 3).
--
-- **This window is the console of what it runs**, as a Terminal is: its
-- own endpoint, mounted as the child's `/Devices/console` and speaking the
-- console's protocol through the Console Kit - so a program cannot tell it
-- is printing to an IDE, which is the namespace working as intended.
--
-- **The file as it is on the screen**, not as it was last saved. Unchanged,
-- it runs from where it is, so Processes names its real file; changed, a
-- copy is written to `/Home/.ide-run` under its own name and run from
-- there, in its own folder, with the copy's path turned back into the
-- file's in everything it says - so an error names the line in the file
-- you are looking at. The copy was in `/Temporary`, whose files held 16 KB
-- then - it keeps replicants' state, not programs - and `bench.lua` would
-- not run at all: "ramfs is full" (Diego, 27 September). `/Home` is the disk
-- on a real machine, and memory only on one that has none.
--------------------------------------------------------------------------

local con = use("/Kosmos/Kits/console")
local console = sys.endpoint()

local RUN_DIR = "/Home/.ide-run"
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- What is running: `{ id, path, scratch, started, printed, line, error }`.
local running = nil

local function literally(text)
  return (text:gsub("[%^%$%(%)%%%.%[%]%*%+%-%?]", "%%%0"))
end

local start_lua                       -- below: running a Lua file as it is

--------------------------------------------------------------------------
-- Building C (`docs/tinycc.md`, step C5).
--
-- **A project's C is built into its own image**, in its `build` folder
-- (Diego's decision 4): every `.c` file at the project's top, compiled and
-- linked by the C Kit through `tccbuild.lua` - the same build as `tcc` at
-- the prompt - into the image its Lua names, `-- kosmos: image
-- build/name.elf`, or the folder's own name when none does. Saved first:
-- what is built is what is on the screen.
--
-- **TinyCC's problems are the Problems**, in its words, each on its line in
-- its file, marked there, a click away. A build that fails writes nothing,
-- and Run runs nothing after it.
--
-- **Run builds first when a C file is newer than the image**, so F5 on a
-- Lua and C project is always the C on the screen; a change to the Lua
-- alone builds nothing.
--------------------------------------------------------------------------

local function c_sources()
  local list = {}

  for _, name in ipairs(fs.list(project) or {}) do
    if name:match("%.c$") then list[#list + 1] = project .. "/" .. name end
  end

  table.sort(list)
  return list
end

-- The project's Lua that runs in its image, and the image: the first Lua
-- file at the top whose header names one in `build/`.
local function project_image()
  for _, name in ipairs(fs.list(project) or {}) do
    if name:match("%.lua$") then
      local source = fs.read(project .. "/" .. name)
      local image = type(source) == "string"
                    and source:match("%-%-%s*kosmos:%s*image%s+build/([^%s/]+)")

      if image then return project .. "/build/" .. image, project .. "/" .. name end
    end
  end

  return project .. "/build/" .. base(project):lower():gsub("%s+", "-") .. ".elf", nil
end

local build_problems = {}

local function show_build_problems()
  problems:set("")

  for _, p in ipairs(build_problems) do
    problems:append(("line %-4d  %s    - tcc, %s\n"):format(p.line or 0, p.text, base(p.file or "")),
                    p.severity == "error" and theme.bad or ui.code_colours().warning)
  end

  bottom_tabs.items[2].count = (#build_problems > 0) and #build_problems or nil
end

-- Built, or not: true when an image that is the C on the screen is there.
local function build_now()
  local sources = c_sources()

  if #sources == 0 then
    say("Build builds the project's C - this project has no .c file")
    return false
  end

  save_all()

  local image, main = project_image()
  local tccbuild = use("/Kosmos/Libraries/tccbuild.lua")
  local r, why = tccbuild.build{ sources = sources, out = image,
                                 defines = tccbuild.defines_of(main and fs.read(main)) }

  for _, f in ipairs(open) do
    if f.path:match("%.c$") then f.editor:clear_marks() end
  end

  if not r then
    say("could not build: " .. tostring(why), theme.bad)
    return false
  end

  build_problems = r.problems

  for _, p in ipairs(r.problems) do
    local f = p.file and find_open(p.file)

    if f and (p.line or 0) > 0 then f.editor:mark(p.line, p.severity) end
  end

  show_build_problems()

  if r.ok then
    say(("built %s - %.1f MB in %d ms"):format(relative(image), (r.bytes or 0) / 1048576,
        r.milliseconds), theme.good)
    return true
  end

  local first = r.problems[1] or {}

  say(("build: %d %s, the first %s:%d - %s; nothing was written")
      :format(#r.problems, #r.problems == 1 and "problem" or "problems",
              base(first.file or "?"), first.line or 0, first.text or ""), theme.bad)

  bottom_show(2)

  if first.file then
    local f = find_open(first.file) or open_file(first.file)

    if f then show(f) f.editor:go_to(first.line or 1, 1) end
  end

  return false
end

-- Whether the image is older than some C: a build is wanted before a run.
local function stale(image)
  local made = fs.getattr(image)

  if not made then return true end

  for _, src in ipairs(c_sources()) do
    local a = fs.getattr(src)

    if a and (a.modified or 0) > (made.modified or 0) then return true end
  end

  return false
end

local function start()
  local f = current
  local sources = c_sources()

  -- **A Lua and C project**: build when the C is newer than its image, then
  -- run the Lua that names it - from whichever of its files is in front.
  if #sources > 0 then
    local image, main = project_image()

    for _, g in ipairs(open) do
      if g.path:match("%.c$") and g.editor.dirty then save(g) end
    end

    if stale(image) and not build_now() then return end

    if main and not (f and f.path == main) then
      f = find_open(main) or open_file(main)
      if f then show(f) end
    end
  end

  return start_lua(f)
end

function start_lua(f)
  if not f or f.editor.code ~= "lua" then
    say("Run runs a Lua file - open one first")
    return
  end

  if running then
    running.stopped = true
    sys.kill(running.id)
    running = nil
  end

  -- **A library says so first.** Running `/Kosmos/Libraries/clock.lua` builds the
  -- table it gives whoever uses it and nothing else, so it ended at once
  -- with nothing printed and looked broken (Diego, 27 September, on the
  -- M700). Not "the application is /Kosmos/Apps/clock.lua": that one does not use
  -- it - `/Kosmos/Libraries/clock.lua` is the local time, for the Deskbar's clock - and
  -- a name shared is not a relation.
  if under(f.path, "/Kosmos/Libraries") then
    say(('%s is a library: running it only builds what it gives whoever uses it, '
         .. 'with use("%s") - applications are in /Kosmos/Apps and programs in /Kosmos/Programs')
        :format(base(f.path), f.path), theme.text_dim)
  end

  local body = f.editor:content()
  local scratch = f.path

  if f.editor.dirty then
    fs.send(RUN_DIR, { type = "mkdir" })
    scratch = RUN_DIR .. "/" .. base(f.path)

    local ok, why = fs.write(scratch, body)

    if not ok then
      say(("could not run the changes to %s: %s - save them, and run it again")
          :format(base(f.path), tostring(why)), theme.bad)
      return
    end
  end

  local app = body:find("%-%- kosmos: application") ~= nil
  local started, err, id = run(scratch, "", true,
                               { ["/Devices/console"] = { cap = console,
                                                      proto = "console" } },
                               f.path:match("^(.*)/") or "/")

  if not started then
    say(("could not run %s: %s"):format(base(f.path), tostring(err)), theme.bad)
    return
  end

  f.editor:clear_marks()
  running = { id = id, path = f.path, scratch = scratch, started = sys.ticks(),
              printed = 0 }
  console_view:set("")
  console_in.hint = "the program is running"
  say(("%s, as process %s - %s"):format(base(f.path), tostring(id),
      app and "an application: its window opens on the desktop"
          or "a program: what it prints is below"), theme.accent)
end

local function stop_run()
  if not running then return end

  running.stopped = true

  local ok, why = sys.kill(running.id)

  if not ok then say("could not stop it: " .. tostring(why), theme.bad) end
end

-- How it ended, said, and an error's line marked in its file.
local function finish(r, code)
  local seconds = (sys.ticks() - r.started) // counter_hz
  local words, colour

  if r.stopped then
    words, colour = ("stopped, after %d s"):format(seconds), theme.text_dim
  elseif r.line then
    words, colour = ("ended with an error at line %d, after %d s: %s")
                      :format(r.line, seconds, r.error), theme.bad
  else
    words = ("ended, code %s, after %d s"):format(tostring(code), seconds)
    colour = (code == 0) and theme.good or theme.bad
  end

  say(("%s %s, %d lines printed"):format(base(r.path), words, r.printed), colour)

  local f = r.line and find_open(r.path)

  if f then
    f.editor:mark(r.line, "error")
    f.editor:go_to(r.line, 1)
  end
end

-- **A program asking for a line** (the Console, 7 October): its request
-- held, the Console brought forward, and answered when Enter is pressed in
-- the line under it - and refused if the program ends first, which it
-- cannot see, being gone.
local waiting_read = nil

function console_in.on_enter(_, typed)
  if not waiting_read then return end

  local who = waiting_read

  waiting_read = nil
  console_view:append(typed .. "\n", theme.accent)
  console_in.text, console_in.caret = "", 1
  console_in.hint = running and "the program is running" or "nothing is running"
  pcall(sys.reply_raw, who, con.encode_reply({ line = typed }))
  print(("ide: gave the program a line, %d bytes"):format(#typed))
end

-- A run over: a line it was waiting for is not, and the Console says so.
local function console_ended()
  waiting_read = nil
  console_in.hint = "nothing is running"
end

-- The console's protocol, as far as a program can tell.
local function serve()
  local changed = false

  while true do
    local bytes, who = sys.receive_raw(console, true)

    if not bytes then return changed end

    local req = con.decode_request(bytes)
    local reply = {}

    if not req then
      reply = { error = con.ERR_BAD_OP }
    elseif req.op == con.WRITE then
      local text = tostring(req.text or "")
      local colour = nil

      if running then
        text = text:gsub(literally(running.scratch), running.path)
        running.printed = running.printed + select(2, text:gsub("\n", ""))

        local line, why = text:match(literally(running.path) .. ":(%d+): ([^\n]*)")

        if line then
          running.line, running.error = tonumber(line), why
          colour = theme.bad
        end
      end

      out(text, colour)
      console_view:append(text, colour)
    elseif req.op == con.READ then
      -- Held: answered when a line is typed in the Console.
      waiting_read = who
      reply = nil
      bottom_show(3)
      console_in.hint = "the program is waiting for a line - type it, then Enter"
      win:focus_on(console_in)
      print("ide: the program asks for a line")
    elseif req.op ~= con.POLL and req.op ~= con.KEYS then
      reply = { error = con.ERR_BAD_OP }
    end

    if reply then pcall(sys.reply_raw, who, con.encode_reply(reply)) end
    changed = true
  end
end

run_button.on_click = function() start() end
stop.on_click = function() stop_run() end

--------------------------------------------------------------------------
-- What a file's names are bound to (step 5): `local ui = use("/Kosmos/Libraries/ui.lua")`
-- makes `ui` that library, and `local win = ui.window{ ... }` makes `win` a
-- window - the one kind of object the drawing asks methods of.
--------------------------------------------------------------------------

-- A library's names and comments, read once a library.
local docs = {}

local function doc_of(path)
  if docs[path] == nil then
    local source = path:match("%.lua$") and fs.read(path)

    if type(source) == "string" then
      docs[path] = libdoc.read(source)
    elseif path:lower():match("^/kosmos/kits/") then
      -- A kit is C and has no source to read: its names are the table's,
      -- which the kit gives anyone who asks, without comments.
      local ok, kit = pcall(use, path)
      local set = { owner = path:match("([^/]+)$"), names = {}, list = {}, tables = {} }

      if ok and type(kit) == "table" then
        local names = {}

        for k in pairs(kit) do
          if type(k) == "string" then names[#names + 1] = k end
        end

        table.sort(names)

        for _, k in ipairs(names) do
          local e = { name = k, signature = set.owner .. "." .. k, summary = "",
                      doc = "", kind = type(kit[k]) == "function" and "function" or "value" }

          set.names[k] = e
          set.list[#set.list + 1] = e
        end
      end

      docs[path] = set
    else
      docs[path] = false
    end
  end

  return docs[path] or nil
end

-- A table's own names, from the running system, as `sys`, `fs` and `gfx`
-- are - the C bindings every program is born with, which have no source.
local born = {}

local function born_with(name)
  if born[name] == nil then
    local t = ({ sys = sys, fs = fs, gfx = gfx })[name]
    local set = false

    if type(t) == "table" then
      set = { owner = name, names = {}, list = {}, tables = {} }

      local names = {}

      for k in pairs(t) do
        if type(k) == "string" then names[#names + 1] = k end
      end

      table.sort(names)

      for _, k in ipairs(names) do
        local e = { name = k, signature = name .. "." .. k, summary = "", doc = "",
                    kind = type(t[k]) == "function" and "function" or "value" }

        set.names[k] = e
        set.list[#set.list + 1] = e
      end
    end

    born[name] = set
  end

  return born[name] or nil
end

-- The file's bindings: name to library path, and name to object kind.
local function bindings(lines)
  local libs, objects = {}, {}

  for _, line in ipairs(lines) do
    local name, path = line:match("^%s*local%s+([%a_][%w_]*)%s*=%s*use%s*%(?%s*[\"']([^\"']+)[\"']")

    if name then libs[name] = path end
  end

  for _, line in ipairs(lines) do
    local name, owner, made = line:match("^%s*local%s+([%a_][%w_]*)%s*=%s*([%a_][%w_]*)%.([%a_][%w_]*)%s*[%({]")

    if name and libs[owner] and made == "window" then
      objects[name] = { path = libs[owner], kind = "window" }
    end
  end

  return libs, objects
end

-- What `owner` followed by `sep` offers, in this file: a library's names, a
-- window's methods, or what a program is born with.
local function members(lines, owner, sep)
  local libs, objects = bindings(lines)

  if sep == "." then
    if libs[owner] then return doc_of(libs[owner]) end
    return born_with(owner)
  end

  local object = objects[owner]
  local doc = object and doc_of(object.path)

  return doc and doc.tables[object.kind] or nil
end

--
-- **The drawing's third check**: a name the file asks a library for that
-- the library has not got - `ui.slidr` - which Lua would take and run as a
-- nil. Asked of the library, answered with the nearest name it does have.
-- Not inside a string or a comment, and not a name the file gives the
-- library itself (`ui.mine = ...`).
--
local function library_problems(f)
  local out = {}
  local lines = f.editor.lines
  local libs = bindings(lines)
  local given = {}
  local state = nil

  for _, line in ipairs(lines) do
    for owner, name in line:gmatch("([%a_][%w_]*)%.([%a_][%w_]*)%s*=[^=]") do
      given[owner .. "." .. name] = true
    end
  end

  for n, line in ipairs(lines) do
    local spans, after = lualex.line(line, state)
    local quiet = {}

    for _, sp in ipairs(spans) do
      if sp[3] == "string" or sp[3] == "comment" then
        for i = sp[1], sp[2] do quiet[i] = true end
      end
    end

    state = after

    for at, owner, name in line:gmatch("()([%a_][%w_]*)%.([%a_][%w_]*)") do
      local path = libs[owner]
      local doc = path and not quiet[at] and doc_of(path)

      if doc and doc.owner and not doc.names[name] and not given[owner .. "." .. name]
         and (at == 1 or line:sub(at - 1, at - 1) ~= ".") then
        local near = libdoc.nearest(doc, name)
        local column = at + #owner + 1

        out[#out + 1] = {
          line = n, column = column, last = column + #name - 1, kind = "error",
          text = ("%s has no %s%s It would be nil when the line runs."):format(owner, name,
                 near and (" - did you mean " .. near .. "?") or "."),
          by = "asked of " .. base(path),
        }
      end
    end
  end

  return out
end

--------------------------------------------------------------------------
-- Checking (step 4).
--
-- **Two parts, as the drawing has them.** What would stop it running is Lua's
-- own parser, run a moment after typing stops - the same parser that will run
-- it, so a line it refuses is exactly the line Lua would. What would go wrong
-- once it runs is luacheck, run when a file opens, when it is saved and on F7:
-- it reads the whole file, and under emulation that is worth doing when asked
-- rather than between two letters.
--------------------------------------------------------------------------

-- How long typing has to stop before the parser looks.
local PAUSE = counter_hz * 2 // 5

local WARNING = function() return ui.code_colours().warning end

-- A file's problems, marked in its editor and - for the one in front -
-- listed in Problems with a count on its tab.
local function show_problems(f)
  local list = {}

  if f.parse_problem then
    list[1] = f.parse_problem
  else
    for _, p in ipairs(f.lint or {}) do list[#list + 1] = p end
  end

  f.editor:clear_marks()

  -- Warnings first, so an error on the same line is the mark that stays.
  for _, kind in ipairs({ "warning", "error" }) do
    for _, p in ipairs(list) do
      if p.kind == kind then f.editor:mark(p.line, p.kind, p.column, p.last) end
    end
  end

  local errors, warnings = 0, 0

  for _, p in ipairs(list) do
    if p.kind == "error" then errors = errors + 1 else warnings = warnings + 1 end
  end

  f.counts = { errors, warnings }

  if f ~= current then return end

  problems:set("")

  for _, p in ipairs(list) do
    problems:append(("line %-4d  %s    - %s\n"):format(p.line, p.text, p.by),
                    p.kind == "error" and theme.bad or WARNING())
  end

  if not f.parse_problem then
    problems:append(("It parses: Lua itself read all %d lines.\n"):format(#f.editor.lines),
                    theme.good)
  end

  bottom_tabs.items[2].count = (#list > 0) and #list or nil
end

-- Lua's parser, and what changed said in the log when it did.
local function parse_now(f)
  local p = lint.parse(f.editor:content(), base(f.path), f.editor.lines)
  local was = f.parse_problem and f.parse_problem.text

  f.parse_problem = p

  if (p and p.text) ~= was then
    if p then
      print(("ide: %s does not parse: line %d: %s"):format(base(f.path), p.line, p.text))
    else
      print(("ide: %s parses"):format(base(f.path)))
    end
  end

  show_problems(f)
end

-- Both parts; with `open_panel`, Problems in front, as F7 does. Kosmos's own
-- files, read only, get the parser and not luacheck: they are not yours to
-- change, and luacheck over `ui.lua` would stop the window for a while.
function check_now(f, open_panel)
  -- Lua's parser and luacheck are Lua's; C is checked by TinyCC, at F6.
  if not (f and f.editor.code == "lua") then return end

  parse_now(f)

  if not f.parse_problem and not f.editor.read_only then
    local list, why = lint.check(f.editor:content(), fs.read, "/Kosmos/Libraries/")

    if not list then
      say("luacheck could not load: " .. tostring(why), theme.bad)
      list = {}
    end

    -- And the names the file asks its libraries for that they have not got.
    for _, p in ipairs(library_problems(f)) do list[#list + 1] = p end

    table.sort(list, function(a, b)
      if a.line ~= b.line then return a.line < b.line end
      return (a.column or 0) < (b.column or 0)
    end)

    f.lint = list
    show_problems(f)
  end

  print(("ide: checked %s: %d errors, %d warnings"):format(base(f.path),
        f.counts[1], f.counts[2]))

  if open_panel then
    bottom_show(2)
  end
end

check.on_click = function() check_now(current, true) end
build_button.on_click = function() build_now() end

-- A click on a problem goes to its line.
local problems_mouse = problems.mouse

function problems:mouse(action, x, y)
  local handled = problems_mouse(self, action, x, y)

  if action == "press" and current then
    local n = (self.buf.lines[self.buf.cy] or ""):match("^line (%d+)")

    if n then
      current.editor:go_to(tonumber(n), 1)
      win:focus_on(current.editor)
    end
  end

  return handled
end

--------------------------------------------------------------------------
-- Suggestions (step 5): a list beside the caret of what the name before it
-- offers, narrowing as you type, and beside the list what the chosen one is,
-- from the comment above it in its library. Up and Down choose, Tab or Enter
-- takes it, Escape closes; Ctrl+Space asks where nothing was offered.
--------------------------------------------------------------------------

local SUGGEST_ROWS, SUGGEST_ROW = 8, 25

-- The pane beside the list is never shorter than this many rows, so one
-- name narrowed to still has room for what it is.
local SUGGEST_LEAST = 5

local function suggest_height(n)
  return math.max(SUGGEST_LEAST, math.min(n, SUGGEST_ROWS)) * SUGGEST_ROW + 8
end
local LIST_W, DOC_W = 300, 360

local suggest = ui.view{ x = 0, y = 0, w = LIST_W + DOC_W, h = SUGGEST_ROWS * SUGGEST_ROW + 8 }

suggest.hidden = true
suggest.items, suggest.on, suggest.first, suggest.prefix = {}, 1, 1, ""

function suggest:draw(g)
  local colours = ui.code_colours()
  local rows = math.min(#self.items, SUGGEST_ROWS)
  local h = suggest_height(#self.items)
  local fh = gfx.height("ui")

  g:fill_round(0, 0, self.w, h, theme.line, 8)
  g:fill_round(1, 1, self.w - 2, h - 2, theme.sunken, 7)
  g:fill(LIST_W, 1, DOC_W - 1, h - 2, theme.window)
  g:fill(LIST_W, 1, 1, h - 2, theme.line_soft)

  for i = 1, rows do
    local e = self.items[self.first + i - 1]
    local y = 4 + (i - 1) * SUGGEST_ROW

    if self.first + i - 1 == self.on then
      g:fill(2, y, LIST_W - 3, SUGGEST_ROW, colours.selection)
    end

    local badge = ({ method = "m", value = "v", type = "t", macro = "#",
                     keyword = "k" })[e.kind] or "fn"
    local bw = gfx.measure(badge, "ui") + 8

    g:frame_round(10, y + (SUGGEST_ROW - 16) // 2, bw, 16, colours.call, 4)
    g:text(14, y + (SUGGEST_ROW - fh) // 2, badge, colours.call, nil, "ui")

    local nx = 10 + bw + 8
    local my = y + (SUGGEST_ROW - gfx.height("mono")) // 2

    g:text(nx, my, self.prefix, theme.accent, nil, "mono")
    g:text(nx + gfx.measure(self.prefix, "mono"), my, e.name:sub(#self.prefix + 1),
           theme.text, nil, "mono")
  end

  -- What the chosen one is: how it is called, and the words above it.
  local e = self.items[self.on]

  if e then
    local x, y = LIST_W + 12, 8
    -- Two lines of how it is called at the most, the second cut when there
    -- is more; and under it the words, as many lines as fit.
    for _, line in ipairs(ui.wrapped(e.signature or e.name, DOC_W - 24, "mono", 2)) do
      g:text(x, y, line, theme.text, nil, "mono")
      y = y + gfx.height("mono")
    end

    y = y + 6

    local words = (e.doc and e.doc ~= "") and e.doc:gsub("\n%s*\n", "\n\n")
                  or "Nothing is written above it in its source."

    for _, para in ipairs({ words:match("^(.-)\n\n") or words }) do
      for _, line in ipairs(ui.wrapped(para:gsub("\n", " "), DOC_W - 24, "ui")) do
        if y + fh > h - 4 then break end
        g:text(x, y, line, theme.text_dim, nil, "ui")
        y = y + fh + 2
      end
    end
  end
end

local function close_suggestions()
  suggest.hidden = true
  suggest.items = {}
end

-- Shown beside the caret of `editor`: below its line, or above it where
-- there is no room below, and inside the window.
local function open_suggestions(editor, items, prefix, what)
  local was_hidden = suggest.hidden

  suggest.items, suggest.prefix, suggest.on, suggest.first = items, prefix, 1, 1

  local cx, cy, lh = editor:caret_at()
  local x = editor.x + cx - 10 - gfx.measure(prefix, "mono")
  local y = editor.y + cy + lh + 2
  local h = suggest_height(#items)

  if y + h > win.root.h - FOOT then y = editor.y + cy - h - 2 end
  if x + suggest.w > win.root.w then x = win.root.w - suggest.w end

  suggest.x, suggest.y, suggest.h = math.max(0, x), math.max(0, y), h
  suggest.hidden = false

  -- On top of every editor, which were added after it.
  win:remove(suggest)
  win:add(suggest)

  if was_hidden then
    print(("ide: suggesting %d names after %s"):format(#items, what))
  end
end

-- The rest of the chosen name, put in at the caret.
local function take_suggestion(editor)
  local e = suggest.items[suggest.on]

  if e then
    editor:insert(e.name:sub(#suggest.prefix + 1))
    print(("ide: took %s"):format(e.name))
  end

  close_suggestions()
end

--------------------------------------------------------------------------
-- **C, suggested as it is typed** (7 October; Diego: "Make sure c has
-- coloring and syntax highlighting and editor suggestions as you type").
-- What a C file can call is what its headers declare, so the names are
-- read from them - `#include "kosmos_window.h"` from the project's folder
-- or the developer files, and what those include in turn - with the file's
-- own functions and C's words. Two letters of a name open the list; a `.`
-- or `->` after a variable whose struct is known offers its fields.
--------------------------------------------------------------------------

local headers = {}                      -- a header's path: its names, or false

local function header_names(name, folder)
  for _, dir in ipairs({ folder, use("/Kosmos/Libraries/tccbuild.lua").DEVELOPER .. "/include" }) do
    local path = dir .. "/" .. name

    if headers[path] == nil then
      local source = fs.read(path)

      headers[path] = (type(source) == "string") and { set = cdoc.read(source), source = source }
                      or false
    end

    if headers[path] then return headers[path] end
  end

  return nil
end

-- Everything `editor`'s file can name, kept until the file changes.
local function c_names(editor, folder)
  if editor.c_names and editor.c_names_version == editor.version then
    return editor.c_names
  end

  local all = { list = {}, names = {}, fields = {} }

  local function merge(set)
    for _, e in ipairs(set.list) do
      local had = all.names[e.name]

      if not had or (had.kind == "type" and e.kind ~= "type") then
        if not had then all.list[#all.list + 1] = e end
        all.names[e.name] = e
      end
    end

    for tag, fields in pairs(set.fields or {}) do all.fields[tag] = all.fields[tag] or fields end
  end

  -- The headers, and theirs, three deep.
  local seen = {}

  local function include(name, depth)
    if seen[name] or depth > 3 then return end
    seen[name] = true

    local h = header_names(name, folder)

    if not h then return end

    merge(h.set)

    for line in h.source:gmatch("[^\n]+") do
      local inner = line:match('^%s*#%s*include%s*["<]([^">]+)[">]')

      if inner then include(inner, depth + 1) end
    end
  end

  merge(cdoc.own(editor.lines))

  for _, name in ipairs(cdoc.includes(editor.lines)) do include(name, 1) end

  for word in pairs(clex.KEYWORDS) do
    if not all.names[word] then
      local e = { name = word, kind = "keyword", signature = word, doc = "" }

      all.names[word] = e
      all.list[#all.list + 1] = e
    end
  end

  table.sort(all.list, function(a, b) return a.name < b.name end)

  editor.c_names, editor.c_names_version = all, editor.version
  return all
end

-- The struct a variable of the file was declared as: `struct kw_surface s`,
-- `struct kw_event *e`, or a typedef'd name the headers give fields to.
local function c_type_of(lines, var, names)
  for _, line in ipairs(lines) do
    local tag = line:match("struct%s+([%a_][%w_]*)%s*%**%s*" .. var .. "%f[^%w_]")

    if tag and names.fields[tag] then return tag end

    for ty in line:gmatch("([%a_][%w_]*)%s+%**%s*" .. var .. "%f[^%w_]") do
      if names.fields[ty] then return ty end
    end
  end

  return nil
end

-- Whether column `col` of line `n` is inside a string or a comment.
local function in_words(lines, n, col)
  local state = nil

  for i = 1, n - 1 do
    local _, after = clex.line(lines[i], state)
    state = after
  end

  local spans = clex.line(lines[n], state)

  for _, s in ipairs(spans) do
    if (s[3] == "string" or s[3] == "comment") and col >= s[1] and col <= s[2] then
      return true
    end
  end

  return false
end

local function refresh_c(editor, c, forced)
  local b = editor.buf
  local before = b.lines[b.cy]:sub(1, b.cx - 1)

  if #before > 0 and in_words(b.lines, b.cy, #before) then return close_suggestions() end

  local names = c_names(editor, project or "/Home")
  local var, sep, prefix = before:match("([%a_][%w_]*)%s*(%.)([%w_]*)$")

  if not var then var, sep, prefix = before:match("([%a_][%w_]*)%s*(%->)([%w_]*)$") end

  local set, what

  if var then
    local tag = c_type_of(b.lines, var, names)

    set = tag and names.fields[tag]
    what = var .. sep
  else
    prefix = before:match("([%a_][%w_]*)$")

    -- Two letters of a word, or any after Ctrl+Space; never a number.
    if prefix and (#prefix >= 2 or forced) and not before:match("%d[%w_]*$") then
      set, what = names, "a word"
    elseif forced and not prefix then
      set, what, prefix = names, "a word", ""
    end
  end

  if not set then return close_suggestions() end

  -- A letter, a `.` or `>` typed opens it; any other key only narrows or
  -- closes what is open.
  local typed = c and ((c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95
                        or (c >= 48 and c <= 57) or c == 46 or c == 62)

  if suggest.hidden and not (forced or typed) then return end

  local items = libdoc.matching(set, prefix)

  if #items == 0 or (#items == 1 and items[1].name == prefix) then
    return close_suggestions()
  end

  open_suggestions(editor, items, prefix, what)
end

-- What the caret is after, asked again after every key: opened by a `.` or
-- a `:` typed, or by Ctrl+Space, and narrowed by typing while it is open.
local function refresh_suggestions(editor, c, forced)
  if not editor.code or editor.read_only then return close_suggestions() end

  if editor.code == "c" then return refresh_c(editor, c, forced) end

  local b = editor.buf
  local before = b.lines[b.cy]:sub(1, b.cx - 1)
  local owner, sep, prefix = before:match("([%a_][%w_]*)([%.:])([%w_]*)$")
  local set = owner and members(b.lines, owner, sep)
  local what = owner and (owner .. sep)

  if not set and forced then
    -- A plain word, Ctrl+Space: the file's own locals and what a program
    -- is born with.
    prefix = before:match("([%a_][%w_]*)$") or ""
    set = { list = {} }

    local seen = {}

    for _, line in ipairs(b.lines) do
      for name in line:gmatch("local%s+function%s+([%a_][%w_]*)") do seen[name] = "function" end
      for names in line:gmatch("local%s+([%a_][%w_,%s]*)=") do
        for name in names:gmatch("[%a_][%w_]*") do seen[name] = seen[name] or "value" end
      end
    end

    for _, name in ipairs(lint.KOSMOS) do seen[name] = seen[name] or "value" end

    local names = {}

    for name in pairs(seen) do names[#names + 1] = name end

    table.sort(names)

    for _, name in ipairs(names) do
      set.list[#set.list + 1] = { name = name, kind = seen[name], signature = name,
                                  doc = "", summary = "" }
    end

    what = "a word"
  end

  if not set then return close_suggestions() end

  local opening = forced or c == 46 or c == 58
  if suggest.hidden and not opening then return end

  local items = libdoc.matching(set, prefix)

  if #items == 0 or (#items == 1 and items[1].name == prefix) then
    return close_suggestions()
  end

  open_suggestions(editor, items, prefix, what)
end

-- The keys the list takes while it is open, before the editor sees them.
local function suggestion_key(editor, c)
  if suggest.hidden then return false end

  if c == ui.UP or c == ui.DOWN then
    local n = #suggest.items

    suggest.on = math.max(1, math.min(n, suggest.on + ((c == ui.UP) and -1 or 1)))

    if suggest.on < suggest.first then suggest.first = suggest.on end
    if suggest.on > suggest.first + SUGGEST_ROWS - 1 then
      suggest.first = suggest.on - SUGGEST_ROWS + 1
    end

    return true
  end

  if c == 9 or c == 13 then
    take_suggestion(editor)
    return true
  end

  if c == 27 then
    close_suggestions()
    return true
  end

  return false
end

function wire_suggestions(editor)
  local editor_key = editor.key

  function editor:key(c)
    if suggestion_key(self, c) then return true end

    local forced = (c == ui.keywith(32, ui.CTRL))
    local done = editor_key(self, c)

    refresh_suggestions(self, c, forced)
    return done or forced
  end

  -- A click anywhere in the editor closes the list.
  local editor_mouse = editor.mouse

  function editor:mouse(action, x, y)
    if action == "press" then close_suggestions() end
    return editor_mouse(self, action, x, y)
  end
end

--------------------------------------------------------------------------
-- Finding a file (27 September). Diego: "an ide wide search field to find
-- files easily by name or part of name". Every file the tree reaches - the
-- project, `/bin`, `/Kosmos/Libraries` - whose name has what is typed in it, whatever
-- its case; the names that begin with it first, and the shorter before the
-- longer, so `cloc` puts `clock.lua` above `clock-replicant.lua`. Beside
-- each, where it lives and what it is, because two files can share a name
-- and be unrelated: `/Kosmos/Libraries/clock.lua` is the local time, not the Clock.
--
-- **Read when a search starts, and not kept**: the places are walked at the
-- first letter and held until the field is emptied, so a file made since is
-- found by the next search, and a letter typed costs only the matching.
-- Enter opens the one chosen, Up and Down choose, Escape closes; Ctrl P
-- goes to the field from anywhere in the window.
--------------------------------------------------------------------------

local FIND_W, FIND_ROWS, FIND_ROW = 520, 10, 28

-- The columns: the name from the left, what it is against the right edge,
-- and where it is between.
local FIND_IN, FIND_KIND = 250, 86

-- How deep into the project a search looks and how many files it holds: a
-- project is a folder of programs, and one opened on a folder of photographs
-- should cost a moment and not a minute.
local FIND_DEPTH, FIND_MOST = 6, 3000

local found = ui.view{ x = 0, y = 0, w = FIND_W, h = FIND_ROW }

found.hidden = true
found.items, found.on, found.first = {}, 1, 1

-- Every file a search can find, while one is being typed.
local findable = nil

-- What a file is, from where it is: `/Kosmos/Apps` holds the applications,
-- `/Kosmos/Programs` the programs, and `/Kosmos/Libraries` the libraries.
-- The project's are yours.
local function kind_of(path, e)
  if under(path, "/Kosmos/Apps") then return "application" end
  if under(path, "/Kosmos/Programs") then return "program" end

  if under(path, "/Kosmos/Libraries") then return "library" end

  return "yours"
end

local function walk(dir, depth, place, seen, list)
  if depth > FIND_DEPTH then return end

  for _, e in ipairs(files.entries(dir) or {}) do
    local path = files.join(dir, e.name)

    if #list >= FIND_MOST then return end

    -- Not what is hidden, as the tree does not show it; and a file once,
    -- when the project is `/Kosmos/Libraries` itself.
    if e.name:sub(1, 1) ~= "." and not seen[path] then
      seen[path] = true

      if e.kind == "directory" then
        walk(path, depth + 1, place, seen, list)
      else
        list[#list + 1] = { name = e.name, lower = e.name:lower(), path = path,
                            dir = dir, kind = kind_of(path, e), place = place }
      end
    end
  end
end

local function read_places()
  local list, seen = {}, {}

  for place, top in ipairs({ project, "/Kosmos/Apps", "/Kosmos/Programs",
                             "/Kosmos/Libraries" }) do
    walk(top, 1, place, seen, list)
  end

  print(("ide: find reads %d files"):format(#list))
  return list
end

-- The files whose names have `typed` in them, in the order they are listed.
local function matching(typed)
  local want = typed:lower()
  local items = {}

  for _, f in ipairs(findable) do
    local at = f.lower:find(want, 1, true)

    if at then
      items[#items + 1] = { name = f.name, path = f.path, dir = f.dir, kind = f.kind,
                            place = f.place, lower = f.lower, at = at }
    end
  end

  table.sort(items, function(a, b)
    if (a.at == 1) ~= (b.at == 1) then return a.at == 1 end
    if #a.name ~= #b.name then return #a.name < #b.name end
    if a.lower ~= b.lower then return a.lower < b.lower end
    if a.place ~= b.place then return a.place < b.place end
    return a.path < b.path
  end)

  return items
end

function found:draw(g)
  local colours = ui.code_colours()
  local fh, mh = gfx.height("ui"), gfx.height("mono")
  local n = #self.items
  local foot_y = self.h - FIND_ROW
  local typed = #find.text

  g:fill_round(0, 0, self.w, self.h, theme.line, 8)
  g:fill_round(1, 1, self.w - 2, self.h - 2, theme.sunken, 7)

  if n == 0 then
    g:text(12, 4 + (FIND_ROW - fh) // 2, ui.fitted(("no file's name has %s in it"):format(find.text),
           self.w - 24, "ui"), theme.text_dim, nil, "ui")
  end

  for i = 1, math.min(n, FIND_ROWS) do
    local k = self.first + i - 1
    local e = self.items[k]
    local y = 4 + (i - 1) * FIND_ROW
    local my = y + (FIND_ROW - mh) // 2

    if not e then break end

    if k == self.on then g:fill(2, y, self.w - 4, FIND_ROW, colours.selection) end

    -- The name, with what was typed in it in the accent.
    local x = 12
    local room = FIND_IN - 12 - x

    for part, piece in ipairs({ e.name:sub(1, e.at - 1), e.name:sub(e.at, e.at + typed - 1),
                                e.name:sub(e.at + typed) }) do
      if piece ~= "" and room > 0 then
        local shown = ui.fitted(piece, room, "mono")

        g:text(x, my, shown, (part == 2) and theme.accent or theme.text, nil, "mono")
        x = x + gfx.measure(shown, "mono")
        room = room - gfx.measure(shown, "mono")
      end
    end

    g:text(FIND_IN, my, ui.fitted(e.dir, self.w - 24 - FIND_KIND - FIND_IN, "mono", true),
           theme.text_dim, nil, "mono")
    g:text(self.w - 12 - gfx.measure(e.kind, "ui"), y + (FIND_ROW - fh) // 2, e.kind,
           theme.text_dim, nil, "ui")
  end

  g:fill(1, foot_y, self.w - 2, 1, theme.line_soft)
  g:text(12, foot_y + (FIND_ROW - fh) // 2,
         ("%d file%s  \u{b7}  Enter opens  \u{b7}  Up and Down choose  \u{b7}  Esc closes")
           :format(n, (n == 1) and "" or "s"), theme.text_dim, nil, "ui")
end

local function close_finder()
  find.text, find.caret, find.all = "", 1, false
  findable = nil
  found.items = {}
  found.hidden = true
end

local function choose_found(d)
  local n = #found.items

  if n == 0 then return end

  found.on = math.max(1, math.min(n, found.on + d))

  if found.on < found.first then found.first = found.on end
  if found.on > found.first + FIND_ROWS - 1 then found.first = found.on - FIND_ROWS + 1 end
end

-- The one chosen, opened - in front of the others, with the focus in it.
local function open_found()
  local e = found.items[found.on]

  if not e then return end

  close_finder()
  open_file(e.path)
end

function find:on_change(typed)
  if typed == "" then
    findable, found.items = nil, {}
    return
  end

  findable = findable or read_places()
  found.items, found.on, found.first = matching(typed), 1, 1
  found.h = math.max(1, math.min(#found.items, FIND_ROWS)) * FIND_ROW + 8 + FIND_ROW

  -- On top of every editor, some of which were opened after it was added.
  win:remove(found)
  win:add(found)

  local first = {}

  for i = 1, math.min(3, #found.items) do
    first[i] = found.items[i].path .. " " .. found.items[i].kind
  end

  print(("ide: find %s: %d files%s"):format(typed, #found.items,
        (#first > 0) and (": " .. table.concat(first, ", ")) or ""))
end

function find:on_enter() open_found() end

local find_key = find.key

function find:key(c)
  if c == ui.UP or c == ui.DOWN then
    choose_found((c == ui.UP) and -1 or 1)
    return true
  end

  if c == 27 then
    close_finder()
    if current then win:focus_on(current.editor) end
    return true
  end

  return find_key(self, c)
end

-- The drawing's `Ctrl P`, at the field's far end while there is room for it
-- beside what the field shows - at the size the window opens there is not,
-- and the foot says it instead.
local find_draw = find.draw

function find:draw(g)
  find_draw(self, g)

  local keys = "Ctrl P"
  local kw = gfx.measure(keys, "ui")
  local shown = (self.text == "" and not self.focused) and self.hint or self.text

  if not self.all and self:text_inset() + gfx.measure(shown) + 16 < self.w - kw - 10 then
    g:text(self.w - kw - 10, (self.h - gfx.height("ui")) // 2, keys, theme.text_dim, nil, "ui")
  end
end

function found:mouse(action, _, y)
  local k = self.first + (y - 4) // FIND_ROW

  if action == "press" and y >= 4 and y < self.h - FIND_ROW and self.items[k] then
    self.on = k
    open_found()
  end

  return true
end

function found:wheel(n)
  local most = math.max(1, #self.items - FIND_ROWS + 1)

  self.first = math.max(1, math.min(most, self.first - n))
  return true
end

--
-- Every pass: the console served, the child collected when it ends, and
-- the pill's seconds - awake while something runs, asleep otherwise.
--
function win:on_frame()
  local changed = serve()

  local front = current

  if front and front.shown_top ~= front.editor.top then
    front.shown_top = front.editor.top
    print(("ide: %s shows line %d of %d"):format(base(front.path), front.editor.top,
          #front.editor.lines))
  end

  if running then
    local id, code = sys.wait(true)

    if id and running and id == running.id then
      local r = running

      running = nil
      finish(r, code)
      console_ended()
      changed = true
    end
  end

  -- The parser, once typing has stopped for a moment.
  local f = current

  if f and f.editor.code == "lua" then
    local now = sys.ticks()

    if f.editor.version ~= f.seen_version then
      f.seen_version, f.edited_at = f.editor.version, now
    elseif f.parsed_version ~= f.seen_version and now - (f.edited_at or 0) > PAUSE then
      f.parsed_version = f.seen_version
      parse_now(f)
      changed = true
    end
  end

  -- And awake while it is waiting to look.
  local waiting = f and f.editor.code == "lua" and f.parsed_version ~= f.seen_version

  local shown = running and ((sys.ticks() - running.started) // counter_hz)

  if shown ~= self.shown_seconds then
    self.shown_seconds = shown
    changed = true
  end

  self.poll_wait_ticks = (running or waiting) and 1 or nil
  return changed
end

-- A click on a line of Output that names a line of an open file goes there.
local output_mouse = output.mouse

function output:mouse(action, x, y)
  local handled = output_mouse(self, action, x, y)

  if action == "press" then
    local line = self.buf.lines[self.buf.cy] or ""

    for _, f in ipairs(open) do
      local n = line:match(literally(f.path) .. ":(%d+):")

      if not n and line:find(base(f.path), 1, true) then
        n = line:match("error at line (%d+)")
      end

      if n then
        show(f)
        f.editor:go_to(tonumber(n), 1)
        break
      end
    end
  end

  return handled
end

--------------------------------------------------------------------------
-- What changes with every key: a tab's dot, the header's words. Read at
-- each is drawn, from what the editors already know, so nothing has to
-- tell anything - the same division `ui.tabs` keeps with its items.
--------------------------------------------------------------------------

local tabs_draw = tabs.draw

function tabs:draw(g)
  for i, f in ipairs(open) do
    if self.items[i] then self.items[i].changed = f.editor.dirty or nil end
  end

  tabs_draw(self, g)
end

local header_measure = header.measure

function header:measure()
  self.sub = base(project)
             .. (current and ("  \u{b7}  " .. relative(current.path)) or "")

  -- Run again while something runs, Stop offered only then, and the pill
  -- saying for how long - as the drawing's running state has them.
  local text = running and "Run again" or "Run"

  if run_button.text ~= text then
    run_button.text = text
    run_button:fit()
  end

  stop.disabled = (running == nil)
  pill.words = running and ("running, %d s"):format(win.shown_seconds or 0)
               or "not running"
  pill.live = running ~= nil
  pill.w = gfx.measure(pill.words, "ui") + 18

  header_measure(self)

  -- The field to find a file takes what the bar leaves it, up to its drawn
  -- width, so a narrower window has a shorter field rather than one over
  -- Check; its right end stays against the dots.
  local last = self.after[#self.after]
  local right = find.x + find.w
  local w = math.max(80, math.min(FIND_FIELD, right - (last.x + last.w + 12)))

  find.x, find.w = right - w, w

  -- And what it found, under it while it has the focus and something typed.
  found.hidden = not (find.focused and find.text ~= "")
  found.x = math.max(0, right - FIND_W)
  found.y = L.head + 2
end

--------------------------------------------------------------------------
-- **Find** (`docs/ide-layout.html`, 7 October): a bar under the tools - what
-- to find, which match of how many, up and down, Match case, This file or
-- the Project, and Replace beside it. In a file the matches are marked in
-- the editor and the one chosen selected; in the project they are listed
-- in Search, a click on one opening its file there. And Go to Line, the
-- same field asking for a number.
--------------------------------------------------------------------------

do

local seek = ui.field{ w = 240, text = "", hint = "Find" }
local swap = ui.field{ w = 200, text = "", hint = "Replace with\u{2026}" }
local up = ui.iconbutton{ icon = "ascending", w = 26, h = 26 }
local down = ui.iconbutton{ icon = "descending", w = 26, h = 26 }
local case = ui.checkbox{ text = "Match case" }
local scope = ui.segments{ items = { "This file", "Project" } }
local replace_one = ui.button{ text = "Replace" }
local replace_every = ui.button{ text = "All" }
local unbar = ui.iconbutton{ icon = "close", w = 26, h = 26 }

local bar_mode = "file"                -- "file", "project" or "line"
local bar_said = ""                    -- "2 of 3", "12 in 4 files", ...

local findbar = ui.view{ x = 0, y = L.head + TOOLS_H, w = W, h = FIND_H,
                         follow = { "left", "right", "top" } }

function findbar:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:fill(0, self.h - 1, self.w, 1, theme.line_soft)

  if bar_said ~= "" then
    g:text(seek.x + seek.w + 10, (self.h - gfx.height("ui")) // 2, bar_said,
           theme.text_dim, nil, "ui")
  end
end

local bar_parts = { findbar, seek, up, down, case, scope, swap, replace_one,
                    replace_every, unbar }

local function opts() return { case = case.checked } end

-- The matches in the file in front, marked, and which of them is chosen.
local told_last = nil                  -- what the log was last told

local function mark_found()
  if not current then bar_said = "" return {} end

  local e = current.editor
  local list = (bar_mode == "file" and seek.text ~= "") and e.buf:matches(seek.text, opts()) or {}

  e:show_found(bar_mode == "file" and seek.text or nil, case.checked)

  local y1, x1 = e.buf:selection()
  local at = 0

  for i, m in ipairs(list) do
    if y1 == m[1] and x1 == m[2] then at = i end
  end

  if bar_mode == "file" then
    local was = bar_said

    bar_said = (seek.text == "" and "") or (#list == 0 and "no matches")
               or (at > 0 and ("%d of %d"):format(at, #list)) or ("%d matches"):format(#list)

    local told = seek.text .. "\0" .. bar_said

    if told ~= told_last and bar_said ~= "" then
      told_last = told
      print(("ide: find %s: %s"):format(seek.text, bar_said))
    end
  end

  return list
end

local function find_step(back)
  if not current or seek.text == "" then return end

  local b = current.editor.buf
  local y, x = b.cy, b.cx

  if back then
    local y1, x1 = b:selection()
    if y1 then y, x = y1, x1 end
  end

  local fy, fx1, fx2 = b:find(seek.text, y, x, { case = case.checked, back = back })

  if fy then
    current.editor:select_range(fy, fx1, fx2)
    print(("ide: found %s at line %d"):format(seek.text, fy))
  end

  mark_found()
end

-- Every match in the project's files, in Search.
local SEARCHED = { lua = true, c = true, h = true, md = true, txt = true, html = true }

local function search_project()
  local needle = seek.text
  local lines, hits, in_files = {}, 0, 0

  if needle == "" then return end

  local low = not case.checked and needle:lower()

  local function walk(dir)
    for _, e in ipairs(files.entries(dir) or {}) do
      local path = files.join(dir, e.name)

      if e.kind == "directory" then
        if e.name ~= "build" and not e.name:match("^%.") then walk(path) end
      elseif SEARCHED[(e.name:match("%.(%w+)$") or ""):lower()] then
        local body = fs.read(path)

        if type(body) == "string" then
          local n, any = 0, false

          for line in (body .. "\n"):gmatch("(.-)\n") do
            n = n + 1

            local hay = low and line:lower() or line

            if hay:find(low or needle, 1, true) then
              hits, any = hits + 1, true
              lines[#lines + 1] = ("%s:%d:  %s"):format(relative(path), n,
                                                         (line:gsub("^%s+", "")))
            end
          end

          if any then in_files = in_files + 1 end
        end
      end
    end
  end

  walk(project)

  search_view:set(#lines > 0 and (table.concat(lines, "\n") .. "\n")
                  or ("nothing in %s has %q\n"):format(base(project), needle))
  bar_said = ("%d in %d file%s"):format(hits, in_files, in_files == 1 and "" or "s")
  bottom_show(4)
  print(("ide: searched %s for %s: %d in %d files"):format(base(project), needle, hits, in_files))
end

-- A click on a line of Search: its file, at its line, the match chosen.
local search_mouse = search_view.mouse

function search_view:mouse(action, x, y)
  local handled = search_mouse(self, action, x, y)

  if action == "press" then
    local path, n = (self.buf.lines[self.buf.cy] or ""):match("^(.-):(%d+):  ")

    if path then
      local f = open_file(path:sub(1, 1) == "/" and path or (project .. "/" .. path))

      if f then
        local row = f.editor.buf.lines[tonumber(n)] or ""
        local hay = case.checked and row or row:lower()
        local at = hay:find(case.checked and seek.text or seek.text:lower(), 1, true)

        if at then f.editor:select_range(tonumber(n), at, at + #seek.text)
        else f.editor:go_to(tonumber(n), 1) end

        win:focus_on(f.editor)
      end
    end
  end

  return handled
end

function seek.on_change() if bar_mode == "file" then mark_found() end end

function seek.on_enter()
  if bar_mode == "line" then
    local n = tonumber(seek.text)

    if n and current then
      current.editor:go_to(math.max(1, math.min(n, #current.editor.buf.lines)), 1)
      print(("ide: went to line %d"):format(n))
    end

    close_bar()
  elseif bar_mode == "project" then
    search_project()
  else
    find_step(false)
  end
end

up.on_click = function() find_step(true) end
down.on_click = function() find_step(false) end
case.on_change = function() mark_found() end
scope.on_change = function(_, i)
  bar_mode = (i == 2) and "project" or "file"
  place_body()
  mark_found()
end

replace_one.on_click = function()
  if not current or seek.text == "" then return end

  local e = current.editor
  local chosen = e.buf:selected()

  if chosen and (case.checked and chosen == seek.text
                 or (not case.checked and chosen:lower() == seek.text:lower())) then
    local y1, x1, y2, x2 = e.buf:selection()

    e.buf:replace(y1, x1, y2, x2, swap.text)
    print(("ide: replaced %s with %s at line %d"):format(seek.text, swap.text, y1))
  end

  find_step(false)
end

-- Enter in Replace replaces the match chosen, and goes to the next.
function swap.on_enter() replace_one.on_click() end

replace_every.on_click = function()
  if not current or seek.text == "" then return end

  local n = current.editor.buf:replace_all(seek.text, swap.text, opts())

  print(("ide: replaced %d"):format(n))
  mark_found()
  bar_said = ("replaced %d"):format(n)
end

unbar.on_click = function() close_bar() end

-- The keys both fields keep: Escape closes the bar at once, rather than
-- emptying the field first; Shift+Enter goes back where a board can say it.
for _, field in ipairs({ seek, swap }) do
  local field_key = field.key

  function field:key(c)
    if c == 27 then close_bar() return true end
    if c == ui.keywith(13, ui.SHIFT) then find_step(true) return true end

    return field_key(self, c)
  end
end

-- F3 and Shift+F3, the next match and the one before, wherever the focus is.
bar_step = find_step

function open_bar(mode, replacing)
  bar_mode = mode
  bar_on = true

  -- What is selected is what to find, as every editor's Find starts.
  local chosen = current and current.editor.buf:selected()

  if mode ~= "line" and chosen and not chosen:find("\n") then
    seek.text, seek.caret = chosen, #chosen + 1
  end

  if mode == "line" then seek.text, seek.caret = "", 1 end

  seek.hint = (mode == "line") and "Line number" or "Find"
  scope.on = (mode == "project") and 2 or 1
  place_body()
  win:focus_on(replacing and swap or seek)
  seek.all = (mode ~= "line") and seek.text ~= ""
  mark_found()
  print(("ide: find bar, %s"):format(mode))
end

function close_bar()
  bar_on = false
  bar_said = ""
  print("ide: find bar closed")

  if current then current.editor:show_found(nil) end

  place_body()

  if current then win:focus_on(current.editor) end
end

--
-- **Where everything goes**, from what is shown: the sidebar or not, the
-- find bar or not - every part below the tools moved to fit, with the
-- distances to the window's edges it keeps from there (`put`).
--
function place_body()
  local rw, rh = win.root.w, win.root.h
  local t = top()
  local sx = side_on and SIDE or 0

  put(side, 0, t, SIDE, side.h)
  side.hidden = not side_on

  for _, v in ipairs({ tree, outline, system }) do
    put(v, 0, t + TAB_H, SIDE, rh - (t + TAB_H) - FOOT)
  end

  side.on_choose(side, side.on or 1)
  put(tabs, sx, t, rw - sx, tabs.h)

  for _, f in ipairs(open) do
    local x, y, w, h = editor_box()

    put(f.editor, x, y, w, h)
  end

  local by = rh - FOOT - BOTTOM

  put(bottom_tabs, sx, by, rw - sx, bottom_tabs.h)

  for _, v in ipairs({ output, problems, search_view }) do
    put(v, sx, by + TAB_H, rw - sx, BOTTOM - TAB_H)
  end

  put(console_view, sx, by + TAB_H, rw - sx, BOTTOM - TAB_H - CONSOLE_IN)
  put(console_in, sx + 10, rh - FOOT - CONSOLE_IN + 2, rw - sx - 20, 26)

  -- The bar's parts, in a row; Replace's only for a file.
  for _, v in ipairs(bar_parts) do v.hidden = not bar_on end

  local y = L.head + TOOLS_H + (FIND_H - 26) // 2
  local x = 12

  put(findbar, 0, L.head + TOOLS_H, rw, FIND_H)
  put(seek, x, y, 240, 26)
  x = x + 240 + gfx.measure("12 in 34 files", "ui") + 20

  for _, v in ipairs({ up, down }) do put(v, x, y, 26, 26) x = x + 28 end

  x = x + 8
  local cw = (case.w > 0) and case.w or (gfx.measure("Match case", "ui") + 30)

  put(case, x, y + 2, cw, (case.h > 0) and case.h or 22)
  x = x + cw + 12
  put(scope, x, y - 2, scope.w, scope.h)
  x = x + scope.w + 16

  local replacing = bar_on and bar_mode == "file"

  swap.hidden, replace_one.hidden, replace_every.hidden =
    not replacing, not replacing, not replacing

  put(swap, x, y, 200, 26)
  x = x + 208
  replace_one:fit()
  put(replace_one, x, y - 2, replace_one.w, replace_one.h)
  x = x + replace_one.w + 6
  replace_every:fit()
  put(replace_every, x, y - 2, replace_every.w, replace_every.h)
  put(unbar, rw - 12 - 26, y, 26, 26)

  -- In a line's mode the field alone.
  if bar_mode == "line" then
    up.hidden, down.hidden, case.hidden, scope.hidden = true, true, true, true
  end
end

for _, v in ipairs(bar_parts) do v.hidden = true win:add(v) end

end

--------------------------------------------------------------------------
-- Keys the window keeps, whichever part has the focus.
--------------------------------------------------------------------------

function win:on_key(c)
  local k, mods = ui.keyparts(c)

  -- Ctrl+Enter or F5 runs, Shift+F5 stops: the drawing's keys, and the
  -- same as Cafesa3D's Script panel will have.
  if (k == 13 and mods == ui.CTRL) or c == ui.F[5] then start() return true end
  if k == ui.F[5] and mods == ui.SHIFT then stop_run() return true end
  if c == ui.F[7] then check_now(current, true) return true end
  if c == ui.F[6] then build_now() return true end

  if c == ui.keywith(61, ui.CTRL) then text:step(1) return true end    -- Ctrl =
  if c == ui.keywith(45, ui.CTRL) then text:step(-1) return true end   -- Ctrl -

  -- Ctrl P, to the field that finds a file, with what was in it chosen so a
  -- letter typed starts again.
  if c == 16 then
    win:focus_on(find)
    find.all = find.text ~= ""
    return true
  end

  -- Find (`docs/ide-layout.html`): Ctrl F in the file, Ctrl R with Replace,
  -- Ctrl Shift F in the project - which the board says apart from Ctrl F
  -- since 7 October (`hal/keys.c`) - Ctrl G a line; Enter or F3 the next
  -- match and Shift F3 the one before; Escape closes it.
  if c == 6 then open_bar("file") return true end           -- Ctrl F
  if c == 18 then open_bar("file", true) return true end    -- Ctrl R
  if c == 7 then open_bar("line") return true end           -- Ctrl G
  if c == ui.keywith(102, ui.CTRL | ui.SHIFT) then open_bar("project") return true end
  if c == ui.F[3] then
    if bar_on and bar_step then bar_step(false) else open_bar("file") end
    return true
  end
  if c == ui.keywith(ui.F[3], ui.SHIFT) and bar_step then bar_step(true) return true end
  if c == 27 and bar_on then close_bar() return true end

  if c == ui.F[1] then TUTORIAL.open() return true end      -- F1, the tutorial
  if c == 19 then save() return true end                    -- Ctrl S
  if c == 14 then new_file() return true end                -- Ctrl N
  if c == 15 then open_chosen() return true end             -- Ctrl O

  -- Ctrl W is the window manager's prefix; pressed twice it reaches here,
  -- and closes the tab in front, as the drawing's Ctrl W does.
  if c == 23 then
    if current then close_file(current) end
    return true
  end

  return false
end

--------------------------------------------------------------------------
-- In the order they are drawn, the header last.
--------------------------------------------------------------------------

win:add(side)
win:add(tree)
win:add(outline)
win:add(system)
win:add(tabs)
win:add(bottom_tabs)
win:add(output)
win:add(problems)
win:add(console_view)
win:add(console_in)
win:add(search_view)
win:add(foot)
win:add(toolbar)
for _, line in ipairs(tool_lines) do win:add(line) end
for _, t in ipairs(tools_left) do if t then win:add(t) end end
for _, t in ipairs(tools_right) do win:add(t) end
win:add(header)
place_body()

-- The files that were open, as they were; the project's first Lua file if
-- none were; and said, for whoever drives this from outside.
for _, path in ipairs(remembered.project == project and remembered.files or {}) do
  if fs.getattr(path) then open_file(path) end
end

if remembered.project == project and remembered.current then
  local f = find_open(remembered.current)

  if f then show(f) end
end

if asked_file then open_file(asked_file) end

say(("project %s, %d files open"):format(project, #open))

--------------------------------------------------------------------------
-- New Project (`docs/tinycc.md`, step C6; the drawing is `docs/tinycc.html`).
--
-- **What kind of application, first** - Diego's three, in his words - and
-- at least one template of each, a project that builds and runs as it is,
-- from `/Kosmos/Templates`. Create copies the template's folder whole into
-- `/Home/Projects/<name>` and makes it the project, its first Lua file open;
-- a Lua and C or a C one is then F6 away from its image and F5 from running.
--------------------------------------------------------------------------

local KINDS = {
  { id = "lua", name = "Lua app",
    what = "A desktop application in Lua alone. Nothing to build: Run runs it.",
    templates = { { "HelloWindow", "Hello Window", "A window with a button that counts." } } },
  { id = "luac", name = "Lua and C app",
    what = "Lua for the window and the orchestrating; C for the work that wants every cycle.",
    templates = { { "Mandelbrot", "Mandelbrot", "Lua opens the window; C computes every pixel." },
                  { "SumBothWays", "Sum, both ways", "One loop in Lua and in C, timed side by side." } } },
  { id = "c", name = "C app",
    what = "A program that is C: a computation, a tool, a window drawn from C.",
    templates = { { "Primes", "Primes", "Counts the primes below a limit, and says how many." },
                  { "Plasma", "Plasma", "A window from C: an animation, keys and the pointer." } } },
  -- **The examples** (7 October; Diego: "an example folder with code that
  -- can be used to learn", "And we can compile them with the ide", "Also
  -- cube3d to show how it can be done in Lua vS C code"), from
  -- `/Kosmos/Examples` (`tools/examples.py`).
  { id = "examples", name = "Examples", root = "/Kosmos/Examples",
    what = "To learn from: TinyGL's own demos in C, and one cube in Lua and in C to compare.",
    templates = {
      { "CubeLua",   "Cube in Lua", "The maths in Lua, the triangle fill in C: look at its time a frame." },
      { "CubeC",     "Cube in C",   "The same cube with all of it in C - the same time, measured beside it." },
      { "GLGears",   "GL Gears",    "Brian Paul's gears, the oldest OpenGL demo there is: TinyGL, in C." },
      { "GLTeapot",  "GL Teapot",   "The Utah teapot, lit: TinyGL, in C." },
      { "GLSpin",    "GL Spin",     "Two spinning shapes: TinyGL, in C." },
      { "GLBounce",  "GL Bounce",   "A bouncing ball: TinyGL, in C." },
      { "GLCube",    "GL Cube",     "A textured cube: TinyGL, in C." },
      { "GLMorph3D", "GL Morph3D",  "Morphing platonic solids: TinyGL, in C." },
      { "GLMech",    "GL Mech",     "A walking mech, the largest of them: TinyGL, in C." },
      { "GLTexObj",  "GL Textures", "Texture objects: TinyGL, in C." },
    } },
}

local new_kind, new_template = 2, 1
local veil = ui.view{ x = 0, y = 0, w = W, h = H, hidden = true,
                      follow = { "left", "right", "top", "bottom" } }
local name_field = ui.field{ w = 300, text = "Mandelbrot", hint = "the project's name", hidden = true }
local create = ui.button{ text = "Create", go = true, hidden = true }
local cancel = ui.button{ text = "Cancel", hidden = true }
local kind_buttons, template_buttons = {}, {}

local DW, DH = 760, 460

local function dialog_origin()
  return (veil.w - DW) // 2, (veil.h - DH) // 2
end

local function place_dialog()
  local x0, y0 = dialog_origin()

  -- The kinds in a row, and a kind's templates flowing over two rows at
  -- most - the examples are ten - each as wide as its name.
  local x = x0 + 24

  for i, b in ipairs(kind_buttons) do
    b.x, b.y = x, y0 + 64
    b.go = (i == new_kind)
    x = x + b.w + 12
  end

  for _, b in pairs(template_buttons) do b.hidden = true end

  local tx, ty = x0 + 24, y0 + 190

  for i in ipairs(KINDS[new_kind].templates) do
    local b = template_buttons[new_kind * 100 + i]

    if tx + b.w > x0 + DW - 24 then tx, ty = x0 + 24, ty + b.h + 8 end

    b.hidden = veil.hidden
    b.x, b.y = tx, ty
    b.go = (i == new_template)
    tx = tx + b.w + 8
  end

  name_field.x, name_field.y = x0 + 110, y0 + 336
  cancel.x, cancel.y = x0 + DW - 24 - create.w - 8 - cancel.w, y0 + DH - 52
  create.x, create.y = x0 + DW - 24 - create.w, y0 + DH - 52
  win.dirty = true
end

function veil:draw(g)
  local x0, y0 = dialog_origin()
  local k = KINDS[new_kind]
  local t = k.templates[new_template]

  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.text, 110))
  g:fill(x0, y0, DW, DH, theme.window)
  g:text(x0 + 24, y0 + 18, "New Project", theme.text, nil, "title")
  g:text(x0 + 24, y0 + 42, "What kind of application is it? Each starts from a template that builds and runs as it is.",
         theme.text_dim, nil, "ui")
  g:text(x0 + 24, y0 + 110, k.what, theme.text_dim, nil, "ui")
  g:text(x0 + 24, y0 + 168, k.root and "Examples" or "Templates", theme.text_dim, nil, "ui")
  g:text(x0 + 24, y0 + 290, t[3], theme.text_dim, nil, "ui")
  g:text(x0 + 24, y0 + 342, "Name", theme.text, nil, "ui")
  g:text(x0 + 24, y0 + 382, "/Home/Projects/" .. name_field.text, theme.text_dim, nil, "ui")
end

local function show_dialog(on)
  veil.hidden = not on
  name_field.hidden, create.hidden, cancel.hidden = not on, not on, not on

  for _, b in ipairs(kind_buttons) do b.hidden = not on end

  place_dialog()

  if on then win:focus_on(name_field) end
end

-- The template's files into `/Home/Projects/<name>`, and that the project.
--
-- **A folder of Kosmos's made a project**: copied to `to` unless it is
-- there already - a lesson opened twice is the one being worked on - then
-- made the project, in place of the one there was, its first Lua open.
-- New Project and Help's lessons both come here. Whether it has C.
--
local function adopt_project(from, to)
  local main, has_c = nil, false

  if not fs.getattr(to) then
    files.make_folder(to)

    for _, f in ipairs(fs.list(from) or {}) do
      local body = fs.read(from .. "/" .. f)

      if type(body) == "string" then fs.write(to .. "/" .. f, body) end
    end
  end

  for _, f in ipairs(fs.list(to) or {}) do
    if f:match("%.lua$") and not main then main = to .. "/" .. f end
    if f:match("%.c$") then has_c = true end
  end

  while #open > 0 do close_file(open[#open]) end

  project = to
  tree.roots[1] = folder(project, base(project), project:match("^(.*)/"))
  tree.roots[1].open = true
  build_problems = {}

  if main then open_file(main) end

  remember()
  return has_c
end

local function create_project()
  local name = (name_field.text:gsub("^%s+", ""):gsub("%s+$", ""))
  local t = KINDS[new_kind].templates[new_template]
  local from, to = (KINDS[new_kind].root or "/Kosmos/Templates") .. "/" .. t[1],
                   "/Home/Projects/" .. name

  if name == "" or name:find("/", 1, true) then
    say("a project wants a name, without a slash", theme.bad)
    return
  end

  if fs.getattr(to) then
    say(("%s is there already: another name"):format(to), theme.bad)
    return
  end

  show_dialog(false)

  local has_c = adopt_project(from, to)

  say(("created %s from the %s template - %s"):format(to, t[2],
      has_c and "F6 builds it, F5 runs it" or "F5 runs it"), theme.good)
end

--
-- **The tutorial** (`docs/ide-tutorial.html`, part one 7 October): its
-- pages, carried in the image and read by the browser where they lie, and
-- each lesson's finished project in `/Kosmos/Tutorial`, opened as a project
-- in `/Home/development` - the first project's folder - to be read, run and
-- changed.
--
TUTORIAL = { index = "asset:tutorial/ide/index.html", lessons = "/Kosmos/Tutorial" }

function TUTORIAL.open()
  local ok, why = fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/browser.lua",
                                       args = TUTORIAL.index })

  print(ok and ("ide: tutorial at " .. TUTORIAL.index)
        or ("ide: could not start the browser: " .. tostring(why)))
end

function TUTORIAL.items()
  local items = {}
  local names = fs.list(TUTORIAL.lessons) or {}

  table.sort(names)

  for _, name in ipairs(names) do
    local number, title = name:match("^(%d+)%-(.+)$")

    items[#items + 1] = {
      text = number and ("%d. %s"):format(tonumber(number), title) or name,
      on_choose = function()
        local to = FIRST .. "/" .. name

        adopt_project(TUTORIAL.lessons .. "/" .. name, to)
        say(("lesson %s, in %s - F5 runs it"):format(name, to), theme.good)
        print(("ide: lesson %s in %s"):format(name, to))
      end,
    }
  end

  if #items == 0 then items[1] = { text = "no lessons here", disabled = true } end

  return items
end

for i, k in ipairs(KINDS) do
  local b = ui.button{ text = k.name, hidden = true }

  b.on_click = function()
    new_kind, new_template = i, 1
    name_field.text = k.templates[1][1]
    place_dialog()
  end

  kind_buttons[i] = b

  for j, t in ipairs(k.templates) do
    local tb = ui.button{ text = t[2], hidden = true }

    tb.on_click = function()
      new_template = j
      name_field.text = t[1]
      place_dialog()
    end

    template_buttons[i * 100 + j] = tb
  end
end

create.on_click = create_project
cancel.on_click = function() show_dialog(false) end
name_field.on_enter = create_project

win:add(veil)
for _, b in ipairs(kind_buttons) do win:add(b) end
for _, b in pairs(template_buttons) do win:add(b) end
win:add(name_field)
win:add(cancel)
win:add(create)

new_project = function() show_dialog(true) end

if asked_new then show_dialog(true) end

win:run()
