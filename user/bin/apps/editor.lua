-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_StyledEdit
-- A text editor, in a window.
--
--   wm editor                     a new file
--   wm editor:/ramfs/hello.lua     one that exists
--
-- Save with the button or Control-S. `run /ramfs/hello.lua` from the shell
-- runs what you wrote, which is the point: the machine can change itself
-- without a rebuild.
--
-- The full-screen `edit` is still there and still works. This is the same
-- thing as an application: its pixels live in the window manager, so an
-- editor that hangs is a window you can still drag out of the way - and it
-- sits beside everything else instead of taking the display.

local ui    = use("/lib/ui.lua")
local panel = use("/lib/panel.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local path = tostring(args or ""):match("^%s*(%S+)") or "/ramfs/untitled.lua"

local W, H = 560, 420

--
-- **A page of text** (`docs/apps.html`): the kit's header with the file's
-- name as its subject, Save as the verb and the rest behind the dots, and
-- the text from the header's rule to the window's edges. It was a row 33
-- tall with the name as a button and a status line along the bottom; what
-- the status line said is said beside the name now, where every converted
-- window says what it is doing.
--
local L = ui.layout

local function base(p) return p:match("([^/]+)$") or p end

local win, err = ui.window{ title = base(path) .. " - Editor",
                            w = W, h = H, x = 100, y = 60 }

if not win then
  print("editor: " .. tostring(err))
  return
end

local existing = fs.read(path)

-- The drawings' page: 10 above the text and 12 before the line numbers.
local text = ui.editor{ x = 0, y = L.head, w = W, h = H - L.head,
                        plain = true, inset = { 12, 10 },
                        follow = { "left", "right", "top", "bottom" },
                        text = (type(existing) == "string") and existing or "" }

win:add(text)

-- Declared here and filled below: the header's controls are written after
-- the actions they run, and two of the actions name each other.
local header, open_file, save_as, run_file

local function save()
  local ok, why = fs.write(path, text:content())

  if ok then
    text.dirty = false
    header.sub = ("saved %d lines to %s"):format(#text.lines, path)
  else
    header.sub = "could not save: " .. tostring(why)
  end
end

--------------------------------------------------------------------------
-- The header.
--
-- **Four buttons became one and the dots.** `docs/desktop.html`: three
-- controls is the rule, and a window that wants a fourth wants a menu
-- instead. Save stays a button because it is the one thing done over and
-- over; Open, Save as and Run happen once each and go behind the dots.
--------------------------------------------------------------------------

local more = ui.iconbutton{ icon = "more" }

header = ui.header{
  x = 0, y = 0, w = W, title = base(path),
  sub = (type(existing) == "string") and "" or "new",
  right = { ui.button{ text = "Save", on_click = save }, more },
}

-- Both places the path changes go through here, so the name in the header,
-- the title bar and the path this window saves to cannot drift apart.
local function opened(p)
  path = p
  header.title = base(p)
  win:retitle(base(p) .. " - Editor")
end

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "Open...",    on_choose = function() open_file() end },
    { text = "Save as...", on_choose = function() save_as() end },
    { separator = true },
    { text = "Run",        on_choose = function() run_file() end },
  })
end

-- Write a program here, run it here.
--
-- The desktop's own `launch`, with an absolute path instead of a program
-- name - the window manager already accepts one, because that is how `wm
-- tracker:/bin` works, so running a file somebody just wrote needs nothing
-- new anywhere.
--
-- It saves first, and that is not a convenience. Running the file while the
-- buffer holds something else means the thing that ran is not the thing on
-- screen, and every confusing minute that follows comes from there.
function run_file()
  if not path:match("%.lua$") then
    header.sub = "only a .lua file can be run"
    return
  end

  save()

  local ok, why = fs.send("/app/wm", { type = "launch", program = path })

  header.sub = ok and ("running " .. path)
                or ("could not run it: " .. tostring(why))
end

-- Somewhere else, chosen from a list rather than typed blind.
--
-- The panel is *modal by construction* rather than by a flag: opening it
-- from inside this window's own event loop nests a second loop, so this
-- window stops reading events until the panel closes. Its pixels stay on
-- screen the whole time because the window manager owns them, which is the
-- same property that lets a hung application keep its window.
--
-- This is its reading mode, where a directory is entered and a *file* is
-- chosen; `save_as` below is the same panel taking a name instead.
function open_file()
  local chooser = panel.open{
    start = path:match("^(.*)/") or "/home",
    on_choose = function(chosen)
      local body, why = fs.read(chosen)

      if not body then
        header.sub = "could not open " .. chosen .. ": " .. tostring(why)
        return
      end

      opened(chosen)
      text:set(body)
      header.sub = ("opened %s, %d lines"):format(path, #text.lines)
    end,
  }

  if chooser then chooser:run() end
end

-- The same panel, writing: a directory and a name rather than a file.
function save_as()
  local chooser = panel.save{
    start = path:match("^(.*)/") or "/home",
    name  = base(path),
    on_choose = function(chosen)
      opened(chosen)
      save()
    end,
  }

  if chooser then chooser:run() end
end

-- After the text, so the focus starts in it.
win:add(header)

--
-- Control-S anywhere in the window, not only when the editor has the focus.
-- A save that depends on which control you last clicked is a save you lose
-- work to.
--
function win:on_key(c)
  if c == 19 then
    save()
    return true
  end

  return false
end

win:run()
