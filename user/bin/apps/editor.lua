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
-- The header band, the same numbers Tracker, Preferences, Processes and the
-- Terminal use. `docs/desktop.html` calls this window a *page of text*: the
-- text is the window, and there is one row above it.
--
local TOOLBAR_Y = 7
local TOOLBAR_H = 26
local CONTENT_Y = TOOLBAR_Y + TOOLBAR_H + 8
local FOOT_H    = 34

local function base(p) return p:match("([^/]+)$") or p end

--
-- **The name's control is as wide as the name**, between a floor and a
-- ceiling. A fixed width puts a short file name in the middle of a lot of
-- nothing, which is what `docs/desktop.html` does not draw; the floor keeps
-- `a.lua` from being a control too small to hit, and the ceiling keeps a
-- long name from reaching the buttons at the other end.
--
local function name_w(p)
  return math.max(96, math.min(260, gfx.measure(base(p)) + 28))
end

local win, err = ui.window{ title = base(path) .. " - Editor",
                            w = W, h = H, x = 100, y = 60 }

if not win then
  print("editor: " .. tostring(err))
  return
end

local existing = fs.read(path)
local status = ui.label{ x = 12, y = H - 26, follow = { "left", "bottom" },
                         text = (type(existing) == "string")
                                and ("opened " .. path)
                                or (path .. " is new"),
                         color = "text_dim" }

local text = ui.editor{ x = 12, y = CONTENT_Y, w = W - 24,
                        h = H - CONTENT_Y - FOOT_H,
                        follow = { "left", "right", "top", "bottom" },
                        text = (type(existing) == "string") and existing or "" }

win:add(text)

-- Declared here and filled below: the header's controls are written after
-- the actions they run, and two of the actions name each other.
local where, open_file, save_as, run_file

local function save()
  local ok, why = fs.write(path, text:content())

  if ok then
    text.dirty = false
    status.text = ("saved %d lines to %s"):format(#text.lines, path)
  else
    status.text = "could not save: " .. tostring(why)
  end
end

--------------------------------------------------------------------------
-- The header.
--
-- **Four buttons became two and a menu.** `docs/desktop.html`: three
-- controls is the rule, and a window that wants a fourth wants a `...`
-- instead. Save stays a button because it is the one thing done over and
-- over; Open, Save as and Run happen once each and go behind the press.
--
-- The file's name is a control rather than a label, and what it does is
-- open another one - which is the question a person is asking when they
-- look at a file name and reach for it.
--------------------------------------------------------------------------

where = ui.button{
  x = 12, y = TOOLBAR_Y, w = name_w(path), h = TOOLBAR_H, text = base(path),
  on_click = function() open_file() end,
}
win:add(where)

-- Both places the path changes go through here, so the name in the header,
-- the title bar and the path this window saves to cannot drift apart.
local function opened(p)
  path = p
  where.text = base(p)
  where.w = name_w(p)
  win:retitle(base(p) .. " - Editor")
end

win:add(ui.button{ x = W - 100, y = TOOLBAR_Y, w = 48, h = TOOLBAR_H,
                   text = "Save", follow = { "right", "top" },
                   on_click = save })

win:add(ui.button{
  x = W - 46, y = TOOLBAR_Y, w = 34, h = TOOLBAR_H, text = "...",
  follow = { "right", "top" },
  on_click = function()
    if not win.open_menu then return end

    win:open_menu(win.origin_x + W - 46,
                  win.origin_y + TOOLBAR_Y + TOOLBAR_H, {
      { text = "Open...",    on_choose = function() open_file() end },
      { text = "Save as...", on_choose = function() save_as() end },
      { separator = true },
      { text = "Run",        on_choose = function() run_file() end },
    })
  end,
})

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
    status.text = "only a .lua file can be run"
    return
  end

  save()

  local ok, why = fs.send("/app/wm", { type = "launch", program = path })

  status.text = ok and ("running " .. path)
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
        status.text = "could not open " .. chosen .. ": " .. tostring(why)
        return
      end

      opened(chosen)
      text:set(body)
      status.text = ("opened %s, %d lines"):format(path, #text.lines)
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

win:add(status)

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
