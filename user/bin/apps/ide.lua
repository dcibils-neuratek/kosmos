-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pe
-- kosmos: section applications
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
local lualex = use("/Kosmos/Libraries/lualex.lua")
local panel = use("/Kosmos/Libraries/panel.lua")
local theme = ui.theme
local L = ui.layout

--------------------------------------------------------------------------
-- What is remembered: the project, its open files, the one in front.
-- A Lua table written with `fs.write`, like every settings file here.
--------------------------------------------------------------------------

local SETTINGS = "/Home/.ide"

--
-- **Its text larger and smaller**, as Terminal's and Log View's is: Diego,
-- 27 September, "we need a way to increase font size like we have in the
-- terminal app". `/Kosmos/Libraries/textsize.lua`'s steps and its menu, in the dots, and
-- Ctrl = and Ctrl - besides; kept in a file of its own, since `/Home/.ide`
-- is the project's memory. Every editor asks for the face as it draws.
--
local textsize = use("/Kosmos/Libraries/textsize.lua")
local text                    -- declared first: the callback below names it

text = textsize.new(ui, "/Home/.ide-text", function()
  print(("ide: text %d px"):format(text:size()))
end)

-- The first project: where the tutorial's lessons will be (`roadmap.md` 7),
-- made if it is not there, so the tree has somewhere to stand.
local FIRST = "/Home/development"

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

local win, err = ui.window{ title = "Kosmos IDE", w = W, h = H, x = 70, y = 50 }

if not win then
  print("ide: " .. tostring(err))
  return
end

local BODY = L.head + TAB_H

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
  text = "", face = function() return text:face() end,
}

local problems = ui.editor{
  x = SIDE, y = H - FOOT - BOTTOM + TAB_H, w = W - SIDE, h = BOTTOM - TAB_H,
  follow = { "left", "right", "bottom" },
  read_only = true, gutter = false, plain = true, inset = { 14, 8 },
  text = "", face = function() return text:face() end,
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
    local counts = current.counts
    local said = "no problems"

    if counts and counts[1] + counts[2] > 0 then
      said = ("%d error%s, %d warning%s"):format(counts[1], counts[1] == 1 and "" or "s",
                                                counts[2], counts[2] == 1 and "" or "s")
    end

    words = ("Ln %d, Col %d    %s    2 spaces    %s%s"):format(b.cy, b.cx,
            current.editor.code and "Lua 5.4" or "Text",
            current.editor.code and said or "",
            current.editor.read_only and "    read only" or "")
  else
    words = "no file open - choose one in the tree"
  end

  g:text(14, ty, words, theme.text_dim, nil, "ui")

  local keys = "Ctrl Enter run    Shift F5 stop    Ctrl S save    Ctrl / comment    Ctrl P find"

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

-- Not `run`: that is the function a program starts another with.
local run_button = ui.button{ text = "Run", go = true, icon = "run",
                              hint = "Ctrl Enter" }
local stop = ui.button{ text = "Stop", icon = "stop", hint = "Shift F5",
                        disabled = true }
local check = ui.button{ text = "Check", icon = "check", hint = "F7" }

-- Finding a file by its name, at the header's right end where Tracker keeps
-- its Search; what it finds and how are further down, under "Finding a file".
local FIND_FIELD = 240
local find = ui.field{ w = FIND_FIELD, text = "", hint = "Find a file", icon = "search" }

local header = ui.header{
  x = 0, y = 0, w = W, title = "Kosmos IDE", sub = "",
  -- Room for the project and the file, so the bar after them stays where
  -- it is as the file in front changes; a longer path is cut. It was 300,
  -- and the field to find a file needed the rest: at the size the window
  -- opens, Run, Stop and Check with their keys leave little else.
  sub_room = 150,
  after = {
    icon("new", new_file), icon("open", open_chosen),
    icon("save", function() save() end), icon("saveall", save_all),
    divider(),
    icon("undo", function() if current then current.editor:undo() end end),
    icon("redo", function() if current then current.editor:redo() end end),
    divider(),
    run_button, stop, check, pill,
  },
  right = { find, icon("more", function(self)
    win:open_menu(win.origin_x + self.x, win.origin_y + self.y + self.h, text:items())
  end) },
}

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
-- you are looking at. The copy was in `/Temporary`, whose files hold 16 KB -
-- it keeps replicants' state, not programs - and `bench.lua` would not run
-- at all: "ramfs is full" (Diego, 27 September). `/Home` is the disk on a
-- real machine, and memory only on one that has none.
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

local function start()
  local f = current

  if not f or not f.editor.code then
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
         .. 'with use("%s") - applications and programs are in /bin')
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
    elseif req.op == con.READ then
      -- Nothing typed in the IDE reaches a program yet: said, not hung.
      reply = { error = con.ERR_NO_READER }
    elseif req.op ~= con.POLL and req.op ~= con.KEYS then
      reply = { error = con.ERR_BAD_OP }
    end

    pcall(sys.reply_raw, who, con.encode_reply(reply))
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
  if not (f and f.editor.code) then return end

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
    bottom_tabs.on = 2
    output.hidden, problems.hidden = true, false
  end
end

check.on_click = function() check_now(current, true) end

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

-- Words that fit `w` pixels a line, as many lines as there are.
local function wrap(text, w, face)
  local lines, line = {}, ""

  for word in text:gmatch("%S+") do
    local try = (line == "") and word or (line .. " " .. word)

    if gfx.measure(try, face) <= w or line == "" then
      line = try
    else
      lines[#lines + 1] = line
      line = word
    end
  end

  if line ~= "" then lines[#lines + 1] = line end

  return lines
end

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

    local badge = (e.kind == "method") and "m" or (e.kind == "value") and "v" or "fn"
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
    local sig = wrap(e.signature or e.name, DOC_W - 24, "mono")

    for i = 1, math.min(2, #sig) do
      g:text(x, y, sig[i], theme.text, nil, "mono")
      y = y + gfx.height("mono")
    end

    y = y + 6

    local words = (e.doc and e.doc ~= "") and e.doc:gsub("\n%s*\n", "\n\n")
                  or "Nothing is written above it in its source."

    for _, para in ipairs({ words:match("^(.-)\n\n") or words }) do
      for _, line in ipairs(wrap(para:gsub("\n", " "), DOC_W - 24, "ui")) do
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

-- What the caret is after, asked again after every key: opened by a `.` or
-- a `:` typed, or by Ctrl+Space, and narrowed by typing while it is open.
local function refresh_suggestions(editor, c, forced)
  if not editor.code or editor.read_only then return close_suggestions() end

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

-- `s`, cut at its end - or with `from_left`, its start - to `room` pixels.
local function cut(s, room, face, from_left)
  if gfx.measure(s, face) <= room then return s end

  while #s > 1 and gfx.measure("..." .. s, face) > room do
    s = from_left and s:sub(2) or s:sub(1, -2)
  end

  return from_left and ("..." .. s) or (s .. "...")
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
    g:text(12, 4 + (FIND_ROW - fh) // 2, cut(("no file's name has %s in it"):format(find.text),
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
        local shown = cut(piece, room, "mono")

        g:text(x, my, shown, (part == 2) and theme.accent or theme.text, nil, "mono")
        x = x + gfx.measure(shown, "mono")
        room = room - gfx.measure(shown, "mono")
      end
    end

    g:text(FIND_IN, my, cut(e.dir, self.w - 24 - FIND_KIND - FIND_IN, "mono", true),
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
      changed = true
    end
  end

  -- The parser, once typing has stopped for a moment.
  local f = current

  if f and f.editor.code then
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
  local waiting = f and f.editor.code and f.parsed_version ~= f.seen_version

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
-- Keys the window keeps, whichever part has the focus.
--------------------------------------------------------------------------

function win:on_key(c)
  local k, mods = ui.keyparts(c)

  -- Ctrl+Enter or F5 runs, Shift+F5 stops: the drawing's keys, and the
  -- same as Cafesa3D's Script panel will have.
  if (k == 13 and mods == ui.CTRL) or c == ui.F[5] then start() return true end
  if k == ui.F[5] and mods == ui.SHIFT then stop_run() return true end
  if c == ui.F[7] then check_now(current, true) return true end

  if c == ui.keywith(61, ui.CTRL) then text:step(1) return true end    -- Ctrl =
  if c == ui.keywith(45, ui.CTRL) then text:step(-1) return true end   -- Ctrl -

  -- Ctrl P, to the field that finds a file, with what was in it chosen so a
  -- letter typed starts again.
  if c == 16 then
    win:focus_on(find)
    find.all = find.text ~= ""
    return true
  end

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
