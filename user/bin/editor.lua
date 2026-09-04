-- kosmos: application
-- A text editor, in a window.
--
--   wm editor                     a new file
--   wm editor:/data/hello.lua     one that exists
--
-- Save with the button or Control-S. `run /data/hello.lua` from the shell
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

local path = tostring(args or ""):match("^%s*(%S+)") or "/data/untitled.lua"

local W, H = 560, 420

local win, err = ui.window{ title = "Editor", w = W, h = H, x = 100, y = 60 }

if not win then
  print("editor: " .. tostring(err))
  return
end

local existing = fs.read(path)
local status = ui.label{ x = 12, y = H - 26,
                         text = (type(existing) == "string")
                                and ("opened " .. path)
                                or (path .. " is new"),
                         color = "text_dim" }

local text = ui.editor{ x = 12, y = 62, w = W - 24, h = H - 96,
                        text = (type(existing) == "string") and existing or "" }

local where = ui.label{ x = 12, y = 14, text = path, color = "text" }
win:add(where)
win:add(text)

local function save()
  local ok, why = fs.write(path, text:content())

  if ok then
    text.dirty = false
    status.text = ("saved %d lines to %s"):format(#text.lines, path)
  else
    status.text = "could not save: " .. tostring(why)
  end
end

win:add(ui.button{ x = 12, y = 30, w = 60, h = 24, text = "Save",
                   on_click = save })

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
win:add(ui.button{
  x = 246, y = 30, w = 56, h = 24, text = "Run",
  on_click = function()
    if not path:match("%.lua$") then
      status.text = "only a .lua file can be run"
      return
    end

    save()

    local ok, why = fs.send("/app/wm", { type = "launch", program = path })

    status.text = ok and ("running " .. path)
                  or ("could not run it: " .. tostring(why))
  end,
})

-- Somewhere else, chosen from a list rather than typed blind.
--
-- The panel is *modal by construction* rather than by a flag: opening it
-- from inside this window's own event loop nests a second loop, so this
-- window stops reading events until the panel closes. Its pixels stay on
-- screen the whole time because the window manager owns them, which is the
-- same property that lets a hung application keep its window.
-- Somewhere else, chosen from a list.
--
-- The same panel the Save button opens, in its reading mode: a directory is
-- entered and a *file* is chosen, where saving takes a name instead.
win:add(ui.button{
  x = 176, y = 30, w = 62, h = 24, text = "Open",
  on_click = function()
    local chooser = panel.open{
      start = path:match("^(.*)/") or "/home",
      on_choose = function(chosen)
        local body, why = fs.read(chosen)

        if not body then
          status.text = "could not open " .. chosen .. ": " .. tostring(why)
          return
        end

        path = chosen
        where.text = path
        text:set(body)
        status.text = ("opened %s, %d lines"):format(path, #text.lines)
      end,
    }

    if chooser then chooser:run() end
  end,
})

win:add(ui.button{
  x = 80, y = 30, w = 88, h = 24, text = "Save as",
  on_click = function()
    local chooser = panel.save{
      start = path:match("^(.*)/") or "/home",
      name  = path:match("([^/]+)$") or "untitled.lua",
      on_choose = function(chosen)
        path = chosen
        where.text = path
        save()
      end,
    }

    if chooser then chooser:run() end
  end,
})


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
