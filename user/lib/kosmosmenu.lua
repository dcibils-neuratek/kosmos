-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Kosmos menu's items: the sections of `/Kosmos/Deskbar` and
-- `/Home/Deskbar` as submenus of launchers (`deskbarmenu.lua` reads the
-- tree), then Switch Desktop..., Restart and Shut Down.
--
--   local kosmosmenu = use("/Kosmos/Libraries/kosmosmenu.lua")
--   win:open_menu(x, y, kosmosmenu.items(sections, {
--     say = function(text) print("bar: " .. text) end,
--     launching = function(item) ... end,   -- optional, before the launch
--   }))
--
-- **The Deskbar's, moved rather than copied** (`docs/pewter.md` P3):
-- Pewter's menu bar opens the same menu from its left end, and two bars
-- building it each their own way would be two menus to keep alike.
--
-- Launching goes through the window manager rather than happening here,
-- because starting a windowed program means handing the new process an
-- endpoint to the desktop, and a bar does not hold that endpoint. The same
-- argument keeps the power items there: the authority to end every process
-- at once lives in one place, and what this menu sends is a request rather
-- than a right.

local menudata = use("/Kosmos/Libraries/deskbarmenu.lua")

local M = {}

--
-- A launcher's row. The program it starts and the arguments it starts with
-- are attributes of its file, and both go in the `launch` message: the
-- same message Tracker sends when a launcher on the desktop is opened.
-- `path` is the file, so a right press on the row has something to edit.
--
-- The request is not waited for: the window manager answers at once and
-- says in its list what happened, so the bar is free to breathe while the
-- program starts (`docs/launching.html`). `launching`, when the bar gives
-- one, is told first - the Deskbar puts the button up from what the row
-- already knows, its name and picture.
--
local function launcher(item, how)
  return {
    text = menudata.shown(item),
    icon = item.icon or "App_Generic",
    path = item.path,
    on_choose = function()
      if item.program == "" then
        how.say(item.name .. ": a launcher that names no program")
        return
      end

      if how.launching then how.launching(item) end

      local ok, why = fs.send("/Running/wm", { type = "launch",
                                               program = item.program,
                                               args = item.args, wait = false })

      how.say(ok and ("starting " .. item.program)
              or ("could not: " .. tostring(why)))
    end,
  }
end

--
-- A folder's rows, and a folder inside it is a submenu. The order is the
-- tree's - submenus first, then launchers, each by name - because the
-- order a person sees should be the order the tree has.
--
local function folder_items(items, how)
  local out = {}

  for _, item in ipairs(items) do
    if item.folder then
      out[#out + 1] = { text = item.name, icon = "Folder_generic",
                        folder = true, path = item.path,
                        submenu = folder_items(item.items, how) }
    else
      out[#out + 1] = launcher(item, how)
    end
  end

  if #out == 0 then out[1] = { text = "(nothing here)" } end

  return out
end

M.folder_items = folder_items

--
-- The menu's sections, read from the tree: three layers - the menu that
-- ships, the applications installed in `/Home/Apps` beside it, and the
-- person's own on top (`deskbarmenu.layers`). A launcher to a program
-- that is gone is not shown: `programs` is what `/Kosmos/Apps` declared,
-- read here when the bar has not read it already. About fifty messages to
-- the disk server, so a bar reads it once and again when told.
--
function M.sections(programs)
  local types = use("/Kosmos/Libraries/filetypes.lua")

  return menudata.layers(fs, menudata.installed(types.installed(fs), types.declared),
                         menudata.present(programs or menudata.programs(fs)))
end

function M.items(sections, how)
  local items = {}

  for _, section in ipairs(sections) do
    -- The folder's own name, which is what the menu shows: `seed` made it
    -- once, when a program's lower-case `kosmos: section` became a folder,
    -- and from then on the name on the screen is the name on the disk.
    items[#items + 1] = { text = section.name, icon = "Folder_generic",
                          submenu = folder_items(section.items, how) }
  end

  if #items == 0 then
    items[1] = { text = "(no " .. menudata.HOME .. ")" }
  end

  --
  -- Restart and Shut Down, at the bottom behind a separator. BeOS put them
  -- here and so does every desktop since - and the separator is doing real
  -- work: everything above it starts something and these end everything,
  -- and a menu that mixed the two would eventually be a machine somebody
  -- turned off while reaching for a calculator.
  --
  -- **Switch Desktop...** (`docs/pewter.md` P1): every window asked to
  -- close, and the chooser again on an empty screen - another desktop
  -- without restarting the machine. Above the two that end everything,
  -- since it ends only what is open.
  --
  items[#items + 1] = { separator = true }

  items[#items + 1] = {
    text = "Switch Desktop...",
    icon = "Prefs_Backgrounds",
    on_choose = function()
      how.say("switching desktop")
      fs.send("/Running/wm", { type = "desktop", name = false })
    end,
  }

  items[#items + 1] = {
    text = "Restart",
    icon = "App_Generic",   -- the three cubes (Diego: "like the ones used for demos like snes")
    on_choose = function()
      how.say("restarting")
      fs.send("/Running/wm", { type = "power", action = "restart" })
    end,
  }

  items[#items + 1] = {
    text = "Shut Down",
    icon = "App_Generic",
    on_choose = function()
      how.say("shutting down")
      fs.send("/Running/wm", { type = "power", action = "off" })
    end,
  }

  return items
end

return M
