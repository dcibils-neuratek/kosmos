-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pe
-- kosmos: section applications
--
-- **Kosmos IDE**: where Lua for Kosmos is written and run.
--
--   wm ide                        the last project, or the first one
--   wm ide:/home/development      that folder as the project
--   wm ide:/home/development/a.lua    that file, and its folder as the project
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
-- **Kosmos is in the tree to be read**: `/bin`, `/lib` and the kits, under
-- the project, read only - so the way to learn what `ui.slider` does is to
-- open `ui.lua` and read it.

local ui = use("/lib/ui.lua")
local files = use("/lib/files.lua")
local panel = use("/lib/panel.lua")
local theme = ui.theme
local L = ui.layout

--------------------------------------------------------------------------
-- What is remembered: the project, its open files, the one in front.
-- A Lua table written with `fs.write`, like every settings file here.
--------------------------------------------------------------------------

local SETTINGS = "/home/.ide"

-- The first project: where the tutorial's lessons will be (`roadmap.md` 7),
-- made if it is not there, so the tree has somewhere to stand.
local FIRST = "/home/development"

local remembered = fs.read(SETTINGS)
if type(remembered) ~= "table" then remembered = {} end

local asked = tostring(args or ""):match("^%s*(%S+)")

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
local READ_ONLY = { "/bin", "/lib", "/kits" }

local function read_only(path)
  for _, top in ipairs(READ_ONLY) do
    if path == top or path:sub(1, #top + 1) == top .. "/" then return true end
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

local win, err = ui.window{ title = "Kosmos IDE", w = W, h = H, x = 70, y = 50 }

if not win then
  print("ide: " .. tostring(err))
  return
end

local BODY = L.head + TAB_H

-- The open files, in their tabs' order: `{ path =, editor = }`.
local open = {}
local current = nil

-- Said in the Output panel, and in the log for whoever drives this.
local output

local function say(text)
  if output then
    local was = output:content()

    output:set(((was == "\n") and "" or was) .. text .. "\n")
    output:go_to(#output.lines, 1)
  end

  print("ide: " .. text)
end

--------------------------------------------------------------------------
-- Remembering.
--------------------------------------------------------------------------

local function remember()
  local paths = {}

  for _, f in ipairs(open) do paths[#paths + 1] = f.path end

  fs.write(SETTINGS, { project = project, files = paths,
                       current = current and current.path or nil })
end

--------------------------------------------------------------------------
-- The file tabs and the editors behind them. An editor a file, so each
-- keeps its own undo and its own place; only the one in front is shown.
--------------------------------------------------------------------------

local tabs = ui.tabs{ x = SIDE, y = L.head, w = W - SIDE, items = {} }
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
  local rw, rh = win.root.w, win.root.h
  local editor = ui.editor{
    x = SIDE, y = BODY, w = rw - SIDE, h = rh - BODY - BOTTOM - FOOT,
    follow = { "left", "right", "top", "bottom" },
    code = path:match("%.lua$") and "lua" or nil,
    read_only = read_only(path),
    text = body,
  }

  -- Every file shows the same place in the window; the drawing's editor
  -- has its text 6 in from the top.
  editor.hidden = true
  win:add(editor)

  local f = { path = path, editor = editor }

  open[#open + 1] = f
  show(f)
  say(("opened %s%s"):format(path, editor.read_only and ", read only" or ""))
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

local side = ui.tabs{ x = 0, y = L.head, w = SIDE,
                      follow = { "left", "top" },
                      items = { { text = "Project" }, { text = "Outline" } } }

local tree = ui.tree{
  x = 0, y = BODY, w = SIDE, h = H - BODY - FOOT,
  follow = { "left", "top", "bottom" },
  roots = {
    (function()
      local root = folder(project, base(project), project:match("^(.*)/"))

      root.open = true
      return root
    end)(),
    { text = "Kosmos, to read", heading = true },
    folder("/bin", "/bin", "read only"),
    folder("/lib", "/lib", "read only"),
    -- The kits are C, reached with `use("/kits/...")`: named here, with
    -- nothing to open, so a person can see what there is to use.
    {
      text = "/kits", note = "C",
      children = function()
        local kids = {}

        for _, name in ipairs(sys.kit_names and sys.kit_names() or {}) do
          kids[#kids + 1] = { text = name, quiet = true, note = "C" }
        end

        return kids
      end,
    },
  },
}

-- A file chosen in the tree is opened; a folder opens with its arrow.
tree.on_select = function(_, node)
  if node.path and not node.children then open_file(node.path) end
end

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

  for n, line in ipairs(f.editor.lines) do
    local name = line:match("^%s*local%s+function%s+([%w_%.:]+)")
                 or line:match("^%s*function%s+([%w_%.:]+)")
                 or line:match("^%s*([%w_%.]+)%s*=%s*function")

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
  tree.hidden = (i ~= 1)
  outline.hidden = (i ~= 2)

  if i == 2 then outline.roots = outline_of(current) end
end

--------------------------------------------------------------------------
-- The panel below: Output, and Problems for step 4.
--------------------------------------------------------------------------

local bottom_tabs = ui.tabs{
  x = SIDE, y = H - FOOT - BOTTOM, w = W - SIDE,
  follow = { "left", "right", "bottom" },
  items = { { text = "Output" }, { text = "Problems" } },
}

output = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "",
}

local problems = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "Checking is the IDE's next step but two: Lua's own parser as you "
         .. "type, then luacheck.",
}

problems.hidden = true

bottom_tabs.on_choose = function(_, i)
  output.hidden = (i ~= 1)
  problems.hidden = (i ~= 2)
end

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

    words = ("Ln %d, Col %d    %s    2 spaces%s"):format(b.cy, b.cx,
            current.editor.code and "Lua 5.4" or "Text",
            current.editor.read_only and "    read only" or "")
  else
    words = "no file open - choose one in the tree"
  end

  g:text(14, ty, words, theme.text_dim, nil, "ui")

  local keys = "Ctrl S save    Ctrl N new    Ctrl O open    Ctrl / comment"

  g:text(self.w - 14 - gfx.measure(keys, "ui"), ty, keys, theme.text_dim, nil, "ui")
end

--------------------------------------------------------------------------
-- The header and its button bar, as the drawing has it: the subject and
-- where you are, then the file verbs, the edit verbs, and Run, Stop and
-- Check with a word saying what is running; the dots at the far end.
--------------------------------------------------------------------------

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

local function divider()
  local v = ui.view{ w = 13, h = 20 }

  function v:draw(g) g:fill(6, 0, 1, self.h, theme.line_soft) end

  return v
end

local later = "that is the IDE's next step"

local pill = ui.view{ w = gfx.measure("not running", "ui") + 18, h = 22 }

pill.words = "not running"

function pill:draw(g)
  g:frame_round(0, 0, self.w, self.h, theme.line_soft, 11)
  g:text(9, (self.h - gfx.height("ui")) // 2, self.words, theme.text_dim, nil, "ui")
end

local function icon(name, action)
  return ui.iconbutton{ icon = name, w = 30, h = 30, on_click = action }
end

local run = ui.button{ text = "Run", go = true, icon = "run", hint = "Ctrl Enter",
                       disabled = true }
local stop = ui.button{ text = "Stop", icon = "stop", hint = "Shift F5",
                        disabled = true }
local check = ui.button{ text = "Check", icon = "check", hint = "F7",
                         disabled = true }

local header = ui.header{
  x = 0, y = 0, w = W, title = "Kosmos IDE", sub = "",
  -- Room for the project and the file, so the bar after them stays where
  -- it is as the file in front changes; a longer path is cut.
  sub_room = 300,
  after = {
    icon("new", new_file), icon("open", open_chosen),
    icon("save", function() save() end), icon("saveall", save_all),
    divider(),
    icon("undo", function() if current then current.editor:undo() end end),
    icon("redo", function() if current then current.editor:redo() end end),
    divider(),
    run, stop, check, pill,
  },
  right = { icon("more", function() say("Run is " .. later) end) },
}

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
  header_measure(self)
end

--------------------------------------------------------------------------
-- Keys the window keeps, whichever part has the focus.
--------------------------------------------------------------------------

function win:on_key(c)
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
win:add(tabs)
win:add(bottom_tabs)
win:add(output)
win:add(problems)
win:add(foot)
win:add(header)

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

win:run()
