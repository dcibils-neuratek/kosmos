-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Appearance
-- kosmos: needs screen
-- Edit a launcher: what it starts, with what arguments, under what picture.
--
--   launcheredit /home/Deskbar/Demos/doom
--   launcheredit /home/Desktop/Drive
--
-- Opened by right-clicking an item in the Deskbar's menu, and by
-- right-clicking an icon on the desktop. Both are the same kind of file - an
-- empty node whose attributes say what to start - so both are this window.
--
-- **It edits the file, not the menu.** A launcher is `kind=launcher` with
-- `program`, `args` and `icon`, and everything that shows one reads those.
-- So this writes those attributes and tells the Deskbar to read its tree
-- again; it does not know what a menu is, and the Deskbar does not know this
-- exists.

local ui = use("/lib/ui.lua")
local theme = ui.theme

local path = tostring(args or ""):match("^%s*(%S+)")

if not path then
  print("usage: launcheredit <path to a launcher>")
  return
end

local attrs = fs.getattr(path)

if not attrs then
  print("launcheredit: no " .. path)
  return
end

--
-- The name is the file's, and is not edited here.
--
-- Renaming is Tracker's job and it already does it; two places that rename a
-- file is two places to disagree about what happens when the new name is
-- taken. So this window is about what the launcher *does*.
--
local name = path:match("([^/]+)$") or path

--
-- Every picture the image carries, which is what there is to choose from.
--
-- `sys.asset()` with no argument lists the table the build compiled in, and
-- the icons are the `.png` in it. The test pattern is the one that is not an
-- icon - it is there so the PNG decoder has something to decode before there
-- is a filesystem - so it is the one name taken out.
--
-- Read once. The table is fixed at build time and cannot change while this
-- window is open.
--
local ICONS = {}

do
  for _, asset in ipairs(sys.asset() or {}) do
    local base = tostring(asset):match("^(.+)%.png$")

    if base and base ~= "test-pattern" then ICONS[#ICONS + 1] = base end
  end

  table.sort(ICONS)
end

local W, H = 470, 420
local win, err = ui.window{ title = "Launcher", w = W, h = H, x = 180, y = 120 }

if not win then
  print("launcheredit: " .. tostring(err))
  return
end

win:add(ui.label{ x = 12, y = 10, w = W - 24, text = name })

win:add(ui.label{ x = 12, y = 40, w = 80, text = "Starts" })
local program = ui.field{ x = 96, y = 36, w = W - 108,
                          text = tostring(attrs.program or "") }
win:add(program)

--
-- What the field holds, said out loud.
--
-- The whole path of the Lua file to run, and it does not have to be in
-- `/bin` - `/home/mine.lua` is as ordinary as `/bin/doom.lua`. The window
-- manager would complete a bare name, and this stores the completed one on
-- save rather than the short one, so what is in the file is what runs.
--
win:add(ui.label{ x = 96, y = 58, w = W - 108,
                  text = "the Lua file to run, anywhere - /bin/doom.lua" })

win:add(ui.label{ x = 12, y = 96, w = 80, text = "With" })
local arguments = ui.field{ x = 96, y = 92, w = W - 108,
                            text = tostring(attrs.args or "") }
win:add(arguments)

--------------------------------------------------------------------------
-- The picture, chosen from what there is rather than typed.
--
-- A name typed into a box is a name that can be wrong, and the only way to
-- find out was to save and look at the menu. Forty-eight names is a list
-- somebody can read, and the one thing a list cannot show is what the
-- picture *looks like* - so the chosen one is drawn beside it at the size
-- the Deskbar draws it.
--------------------------------------------------------------------------

win:add(ui.label{ x = 12, y = 132, w = 80, text = "Picture" })

local chosen = tostring(attrs.icon or "")

local LIST_X, LIST_Y, LIST_H = 96, 132, 200

--
-- The preview, which is the whole reason this is not just a list of words.
--
-- `g:icon` takes the asset's own name, so the `.png` goes back on here: the
-- launcher's attribute is `App_Generic` and the file is `App_Generic.png`,
-- and every other reader of a launcher does the same join.
--
local preview = ui.view{ x = 12, y = LIST_Y + 34, w = 72, h = 72 }

function preview:draw(g)
  g:sunken(0, 0, self.w, self.h, "sunken")

  if chosen ~= "" then
    g:icon((self.w - 32) // 2, (self.h - 32) // 2, chosen .. ".png", 32)
  end
end

win:add(preview)

local picture = ui.list{
  x = LIST_X, y = LIST_Y, w = W - LIST_X - 12, h = LIST_H,
  items = ICONS,
  on_select = function(_, item)
    chosen = tostring(item or "")
    win.dirty = true
  end,
}

-- Open on the one it already has, so the list says what this launcher looks
-- like rather than starting at the top and implying the first name.
for i, one in ipairs(ICONS) do
  if one == chosen then picture.selected = i break end
end

win:add(picture)

local status = ui.label{ x = 12, y = H - 26, w = W - 24, text = path }
win:add(status)

--
-- Saved, and the Deskbar told.
--
-- `setprop /app/Deskbar/menu reload` is what a person would type; this
-- writes the same property directly, which is the same thing without the
-- program in between. Deliberately not an error when no Deskbar is running -
-- editing a launcher on the desktop is an ordinary thing to do with no menu
-- open anywhere.
--
local function save()
  --
  -- Typed short, stored whole - the same rule `launcher.lua` applies when
  -- it makes one at the prompt.
  --
  -- The completion belongs in the typing, not in the file. A launcher that
  -- records `doom` runs correctly and reads as broken to anybody who does
  -- not know that the window manager will put `/bin/` and `.lua` around it;
  -- one that records `/bin/doom.lua` says what it does.
  --
  local starts = program.text

  if starts ~= "" and not starts:find("/") then
    starts = "/bin/" .. starts .. ".lua"
    program.text = starts
    program.caret = #starts + 1
  end

  local ok, why = fs.setattr(path, {
    kind = "launcher",
    type = "launcher",
    program = starts,
    args = arguments.text,
    -- An empty choice means "no picture of its own", which is nil rather
    -- than an empty string: an empty string is a name, and nothing is
    -- called "".
    icon = (chosen ~= "") and chosen or nil,
  })

  if not ok then
    status.text = "could not save: " .. tostring(why)
    win.dirty = true
    return
  end

  fs.write("/app/Deskbar/menu", "reload")

  status.text = "saved - " .. name .. " starts " .. starts
  win.dirty = true
end

win:add(ui.button{ x = W - 190, y = H - 56, w = 80, text = "Revert",
                   on_click = function()
                     -- The caret with the text, or it is left pointing past
                     -- the end of a shorter string and the next key typed
                     -- lands nowhere.
                     local function put(field, text)
                       field.text = text
                       field.caret = #text + 1
                       field.all = false
                     end

                     put(program, tostring(attrs.program or ""))
                     put(arguments, tostring(attrs.args or ""))

                     chosen = tostring(attrs.icon or "")

                     for i, one in ipairs(ICONS) do
                       if one == chosen then picture.selected = i break end
                     end

                     status.text = path
                     win.dirty = true
                   end })

win:add(ui.button{ x = W - 100, y = H - 56, w = 80, text = "Save",
                   on_click = save })

win:run()
