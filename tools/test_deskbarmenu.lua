-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Deskbar's menu, checked on this computer with no machine booted.
--
-- `user/lib/deskbarmenu.lua` turns `/Home/Deskbar` into the rows the menu
-- draws, and every decision in it is one a person would notice being wrong:
-- what counts as an item, which order things come in, how deep a folder may
-- go. None of that needs a screen to be checked, and the store it reads
-- through is a table here for exactly that reason.
--
--   build/host/lua tools/test_deskbarmenu.lua

package.path = "user/lib/?.lua;" .. package.path

local menu = dofile("user/lib/deskbarmenu.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

--
-- A store over a flat table of paths, which is what `fs` looks like from
-- here: `list` gives the names directly under a path, `getattr` the record.
--
local function store_of(tree)
  return {
    list = function(path)
      local names = {}

      for full in pairs(tree) do
        local rest = full:match("^" .. path:gsub("%-", "%%-") .. "/([^/]+)$")

        if rest then names[#names + 1] = rest end
      end

      table.sort(names)
      return names
    end,

    getattr = function(path) return tree[path] end,
  }
end

local DIR = { kind = "directory" }

local function launcher(program, args, icon)
  return { kind = "launcher", program = program, args = args, icon = icon }
end

--------------------------------------------------------------------------
-- The sections are the folders under the root, and nothing else is.
--------------------------------------------------------------------------

local tree = {
  ["/D/System"] = DIR,
  ["/D/Applications"] = DIR,
  ["/D/Demos"] = DIR,
  -- A launcher loose in the root. It belongs in a section and is in none,
  -- so it has nowhere to appear - and must not invent itself one.
  ["/D/stray"] = launcher("stray"),
  -- Somebody's note, in the root and in a section. Neither is a menu item.
  ["/D/notes.txt"] = { kind = "file" },
  ["/D/Demos/readme.txt"] = { kind = "file" },

  ["/D/Applications/calc"] = launcher("calc", "", "App_Calc"),
  ["/D/Applications/editor"] = launcher("editor", "/Home/notes.txt"),

  ["/D/Demos/quake"] = launcher("quake"),
  ["/D/Demos/doom"] = launcher("doom", "--scale 2"),
  ["/D/Demos/GLDemos"] = DIR,
  ["/D/Demos/GLDemos/teapot"] = launcher("teapot"),
  ["/D/Demos/GLDemos/gears"] = launcher("gears"),
}

local sections = menu.sections(store_of(tree), "/D")

check(#sections == 3,
      "three folders under the root are three sections, not "
      .. tostring(#sections))

check(sections[1] and sections[1].name == "Applications"
      and sections[2] and sections[2].name == "Demos"
      and sections[3] and sections[3].name == "System",
      "the sections come out in the folders' own order")

--------------------------------------------------------------------------
-- Only launchers are items.
--------------------------------------------------------------------------

local demos = sections[2].items

-- GLDemos, doom, quake - and not readme.txt.
check(#demos == 3, "a file that is not a launcher is not an item: Demos has "
      .. tostring(#demos) .. " rows")

local names = {}

for _, item in ipairs(demos) do names[#names + 1] = item.name end

check(table.concat(names, ",") == "GLDemos,doom,quake",
      "submenus come before launchers, each sorted: got "
      .. table.concat(names, ","))

--------------------------------------------------------------------------
-- A folder is a submenu, and it nests.
--------------------------------------------------------------------------

check(demos[1].folder and #demos[1].items == 2
      and demos[1].items[1].name == "gears",
      "a folder inside a section is a submenu holding its own launchers")

check(not demos[2].folder and demos[2].program == "doom",
      "a launcher carries the program it starts")

--------------------------------------------------------------------------
-- The arguments, which are the whole reason the menu is files.
--------------------------------------------------------------------------

check(demos[2].args == "--scale 2",
      "a launcher carries its arguments: got " .. tostring(demos[2].args))

check(demos[3].args == "",
      "a launcher with no arguments says so with an empty string rather "
      .. "than nothing, so the caller never sends nil")

--------------------------------------------------------------------------
-- And a tree that points into itself does not hang the desktop.
--
-- Nothing can make one today, and `read` guards anyway: a menu that
-- recurses for ever is a machine that does not come up, which is a much
-- worse failure than a menu that stops being interesting twelve folders
-- down.
--------------------------------------------------------------------------

local loop = { ["/L/Round"] = DIR }
local deep = store_of(loop)
local real_list = deep.list

deep.list = function(path)
  -- Every folder contains a folder called Round, for ever.
  if path:match("Round$") or path == "/L" then return { "Round" } end

  return real_list(path)
end

deep.getattr = function(_) return DIR end

local spun = menu.sections(deep, "/L")

check(type(spun) == "table" and #spun == 1,
      "a folder that contains itself is read to a depth and then stops")

--------------------------------------------------------------------------
-- The two trees merged (`roadmap.md` 6zd): the menu as it ships, and a
-- person's own on top of it.
--------------------------------------------------------------------------

local seeded_tree = {
  ["/D/Applications"] = DIR,
  ["/D/Applications/tracker"] = launcher("/Kosmos/Apps/tracker.lua", ""),
  ["/D/Applications/calc"] = launcher("calc", ""),
  ["/D/Applications/notes.txt"] = { kind = "file" },
  ["/D/Preferences"] = DIR,
  ["/D/Preferences/appearance"] = launcher("/Kosmos/Apps/appearance.lua", ""),
}
local seeded_store = store_of(seeded_tree)

local shipped_store = store_of({
  ["/K/Applications"] = DIR,
  ["/K/Applications/calc"] = launcher("/Kosmos/Apps/calc.lua", "", "App_Calc"),
  ["/K/Applications/tracker"] = launcher("/Kosmos/Apps/tracker.lua", "",
                                         "App_Tracker"),
  ["/K/Demos"] = DIR,
  ["/K/Demos/quake"] = launcher("/Kosmos/Apps/quake.lua", ""),
  ["/K/Demos/doom"] = launcher("/Kosmos/Apps/doom.lua", ""),
  ["/K/Demos/GLDemos"] = DIR,
  ["/K/Demos/GLDemos/glgears"] = launcher("/Kosmos/Apps/glgears.lua", ""),
  ["/K/System"] = DIR,
  ["/K/System/procs"] = launcher("/Kosmos/Apps/procs.lua", ""),
})

local home_store = store_of({
  -- Doom as the person likes it: the same name, their arguments.
  ["/H/Demos"] = DIR,
  ["/H/demos-not-a-section.txt"] = { kind = "file" },
  ["/H/Demos/Doom"] = launcher("/Kosmos/Apps/doom.lua", "--scale 2"),
  -- Quake taken out, by a note under its name.
  ["/H/Demos/quake"] = { kind = "hidden" },
  -- A folder of their own inside a shipped one, and a note in it that
  -- hides nothing and is still no row.
  ["/H/Demos/GLDemos"] = DIR,
  ["/H/Demos/GLDemos/mine"] = launcher("/Home/mine.lua", ""),
  ["/H/Demos/GLDemos/gone"] = { kind = "hidden" },
  -- A section of their own.
  ["/H/Games"] = DIR,
  ["/H/Games/snes"] = launcher("/Home/Apps/snes.lua", "/Home/zelda.sfc"),
})

local merged = menu.merge_sections(menu.sections(shipped_store, "/K"),
                                   menu.sections(home_store, "/H"))
local names = {}

for _, section in ipairs(merged) do names[#names + 1] = section.name end

check(table.concat(names, ",") == "Applications,Demos,Games,System",
      "the sections are both trees', each once: " .. table.concat(names, ","))

local function named(items, name)
  for _, item in ipairs(items or {}) do
    if item.name == name then return item end
  end
end

local demos_m = named(merged, "Demos")
local rows = {}

for _, item in ipairs(demos_m and demos_m.items or {}) do
  rows[#rows + 1] = item.name
end

check(table.concat(rows, ",") == "GLDemos,Doom",
      "Demos is the shipped section with the person's on it - GLDemos, "
      .. "their Doom and no Quake: " .. table.concat(rows, ","))

local doom_m = named(demos_m and demos_m.items, "Doom")

check(doom_m and doom_m.args == "--scale 2" and doom_m.path == "/H/Demos/Doom",
      "an item in both is the person's, whatever its case - their arguments "
      .. "and their file, which is what an edit changes")

local gl = named(demos_m and demos_m.items, "GLDemos")
rows = {}

for _, item in ipairs(gl and gl.items or {}) do rows[#rows + 1] = item.name end

check(table.concat(rows, ",") == "glgears,mine",
      "a folder in both is one folder, both trees' rows in it, and a note "
      .. "that hides nothing is no row: " .. table.concat(rows, ","))

local tracker_m = named(named(merged, "Applications").items, "tracker")

check(tracker_m and tracker_m.path == "/K/Applications/tracker"
      and tracker_m.icon == "App_Tracker",
      "an item only the menu that ships has is that one, with its picture")

check(named(merged, "Games") and #named(merged, "Games").items == 1,
      "a section only the person has is theirs")

--------------------------------------------------------------------------
-- What the seed left goes to the Trash once: a folder of nothing but its
-- launchers whole, and only its launchers from a folder with anything of
-- the person's.
--------------------------------------------------------------------------

local leftovers = store_of({
  ["/H/.seeded"] = { kind = "file" },
  ["/H/Applications"] = DIR,
  ["/H/Applications/tracker"] = launcher("/Kosmos/Apps/tracker.lua", ""),
  ["/H/Applications/calc"] = launcher("/bin/calc.lua", "", "App_Calc"),
  ["/H/Demos"] = DIR,
  ["/H/Demos/doom"] = launcher("doom", "--scale 2"),      -- changed, still the seed's
  ["/H/Demos/mine"] = launcher("/Home/mine.lua", ""),    -- the person's
  ["/H/Demos/GLDemos"] = DIR,
  ["/H/Demos/GLDemos/glgears"] = launcher("/Kosmos/Apps/glgears.lua", ""),
  ["/H/Empty"] = DIR,                                    -- the person's, empty
})
local moves = menu.seed_leftovers(leftovers, "/H",
                                  { tracker = true, calc = true, doom = true,
                                    glgears = true })

table.sort(moves)

check(table.concat(moves, ",")
      == "/H/Applications,/H/Demos/GLDemos,/H/Demos/doom",
      "the seed's go: Applications whole, GLDemos whole, and Doom from a "
      .. "Demos that holds something of the person's - " .. table.concat(moves, ","))

-- A launcher to a program that is gone is not shown; the rest are.
local exists = function(program)
  return program ~= "/Kosmos/Apps/appearance.lua"
end
local shown = menu.sections(seeded_store, "/D", exists)
local prefs = nil

for _, section in ipairs(shown) do
  if section.name == "Preferences" then prefs = section end
end

check(prefs and #prefs.items == 0,
      "a launcher whose program is gone is not in the menu")
check(#shown[1].items == 2,
      "and the launchers whose programs are there still are")

if failed == 0 then
  print(("PASS: %d checks on the Deskbar's menu as it is read off the disk, "
         .. "on this machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
