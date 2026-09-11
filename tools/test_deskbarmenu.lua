-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Deskbar's menu, checked on this computer with no machine booted.
--
-- `user/lib/deskbarmenu.lua` turns `/home/Deskbar` into the rows the menu
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
  ["/D/Applications/editor"] = launcher("editor", "/home/notes.txt"),

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

if failed == 0 then
  print(("PASS: %d checks on the Deskbar's menu as it is read off the disk, "
         .. "on this machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
