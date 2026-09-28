-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What a right click offers, checked on this computer with no machine booted.
--
-- `user/lib/filemenu.lua` decides the items; Tracker only says what each one
-- does. These are the decisions `docs/rightclick.html` drew and Diego agreed
-- on 27 September - above all, that nothing is offered that does not apply:
-- the reason for the menu is that "roms is not a launcher" was the answer on
-- a folder.
--
--   build/host/lua tools/test_filemenu.lua

local filemenu = dofile("user/lib/filemenu.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

-- The ids of a menu, separators as "-", for comparing whole menus.
local function ids(items)
  local out = {}

  for _, it in ipairs(items or {}) do
    out[#out + 1] = it.separator and "-" or it.id
  end

  return table.concat(out, " ")
end

local function find(items, id)
  for _, it in ipairs(items or {}) do
    if it.id == id then return it end
  end
end

--------------------------------------------------------------------------
-- A folder.
--------------------------------------------------------------------------

local folder = filemenu.items{ what = "folder", name = "roms" }

check(ids(folder) == "open - pin - rename cut copy - delete - info",
      "a folder: Open, Pin to sidebar, Rename Cut Copy, Delete, Info: "
      .. ids(folder))

check(not find(folder, "edit") and not find(folder, "empty_trash")
      and not find(folder, "paste"),
      "a folder offers no Edit, no Empty Trash and no Paste")

check(find(filemenu.items{ what = "folder", pinned = true }, "unpin")
      and not find(filemenu.items{ what = "folder", pinned = true }, "pin"),
      "a folder already in the sidebar offers Unpin, not Pin")

check(find(folder, "delete").hint == "to the Trash"
      and find(filemenu.items{ what = "folder", in_trash = true },
               "delete").hint == "for good",
      "Delete says where it goes: the Trash, or for good inside it")

--------------------------------------------------------------------------
-- Files.
--------------------------------------------------------------------------

local film = filemenu.items{ what = "file", opener = "Video" }

check(find(film, "open").hint == "Video" and not find(film, "open").off,
      "a file's Open names what opens it")

local odd = filemenu.items{ what = "file" }

check(find(odd, "open").off == true,
      "a file nothing opens has Open, dim - not missing")

local lua = filemenu.items{ what = "lua" }

check(find(lua, "open").text == "Run" and find(lua, "edit"),
      "a Lua file: Run, which is what opening it does, and Edit beside it")

check(not find(film, "edit"),
      "Edit is not offered where it would do what Open does")

-- Open with (`roadmap.md` 6z): every application that opens it, the default
-- first and saying so, after Open - and not there when nothing opens it.
local with = { { program = "video", name = "Video" },
               { program = "play", name = "Play" } }
local chooser = find(filemenu.items{ what = "file", opener = "Video",
                                     with = with }, "open_with")

check(chooser and chooser.submenu and #chooser.submenu == 2
      and chooser.submenu[1].text == "Video"
      and chooser.submenu[1].hint == "default"
      and chooser.submenu[1].program == "video"
      and chooser.submenu[2].program == "play" and not chooser.submenu[2].hint,
      "a film's Open with is not Video, the default, then Play")
check(ids(filemenu.items{ what = "file", opener = "Video", with = with })
      == "open open_with - rename cut copy - delete - info",
      "Open with is not straight after Open")
check(not find(odd, "open_with"),
      "a file nothing opens offers an Open with with nothing in it")
check(find(filemenu.items{ what = "lua",
                           with = { { program = "editor", name = "Editor" } } },
           "open_with"),
      "a Lua file offers no Open with")

local launcher = filemenu.items{ what = "launcher" }

check(find(launcher, "open") and find(launcher, "edit")
      and find(launcher, "edit").hint == "Launcher editor",
      "a launcher: Open, and Edit in the launcher editor")

--------------------------------------------------------------------------
-- Several, the Trash, the window itself.
--------------------------------------------------------------------------

local three = filemenu.items{ what = "several", count = 3 }

check(ids(three) == "cut copy - delete - info"
      and find(three, "delete").text == "Delete 3 items",
      "several: Cut, Copy, Delete 3 items, Info - the count in the words: "
      .. ids(three))

check(not find(three, "rename") and not find(three, "open"),
      "several offer no Rename and no Open, which are one thing's")

local trash = filemenu.items{ what = "trash" }

check(ids(trash) == "open empty_trash - info",
      "the Trash: Open, Empty Trash, Info: " .. ids(trash))

local empty_trash_elsewhere = false

for _, what in ipairs({ "folder", "file", "lua", "launcher", "several",
                        "space", "place", "builtin", "drive" }) do
  if find(filemenu.items{ what = what, count = 2 }, "empty_trash") then
    empty_trash_elsewhere = true
  end
end

check(not empty_trash_elsewhere,
      "Empty Trash is on the Trash and nowhere else")

local space = filemenu.items{ what = "space", here = "Home", icons = true,
                              paste = false }

check(ids(space) == "new_folder paste - select_all - icon_sizes - refresh - info",
      "the empty space: New folder, Paste, Select all, the icon sizes, "
      .. "Refresh, Info: " .. ids(space))

check(find(space, "paste").off == true
      and not find(filemenu.items{ what = "space", paste = true },
                   "paste").off,
      "Paste is dim with nothing to paste, and lit with something")

check(find(space, "info").text == "Info on Home",
      "Info on the empty space says which folder it is about")

check(not find(filemenu.items{ what = "space", icons = false }, "icon_sizes"),
      "a list has no icon sizes, since a list has no icons")

check(not find(filemenu.items{ what = "space", root = true }, "info"),
      "the root offers no Info, which would be a walk of every drive")

--------------------------------------------------------------------------
-- The sidebar.
--------------------------------------------------------------------------

check(ids(filemenu.items{ what = "place" }) == "open - unpin - info",
      "a place: Open, Unpin from sidebar, Info")

check(ids(filemenu.items{ what = "builtin" }) == "open - info",
      "Home, Desktop and the standard folders: Open and Info, nothing to "
      .. "unpin")

check(find(filemenu.items{ what = "drive" }, "pin")
      and find(filemenu.items{ what = "drive", pinned = true }, "unpin"),
      "a drive can be pinned, and unpinned once it is")

check(filemenu.items{ what = "nothing-known" } == nil,
      "something it does not know is nothing to offer, not an empty menu")

--------------------------------------------------------------------------
-- What an entry is.
--------------------------------------------------------------------------

local T = "/Home/Desktop/Trash"

check(filemenu.what_of({ name = "Trash", kind = "directory" }, T, T) == "trash"
      and filemenu.what_of({ name = "roms", kind = "directory" },
                           "/Home/roms", T) == "folder"
      and filemenu.what_of({ name = "Doom", kind = "launcher" },
                           "/Home/Desktop/Doom", T) == "launcher"
      and filemenu.what_of({ name = "hi.LUA", kind = "file" },
                           "/Home/hi.LUA", T) == "lua"
      and filemenu.what_of({ name = ".lua", kind = "file" },
                           "/Home/.lua", T) == "file"
      and filemenu.what_of({ name = "clip.mp4", kind = "file" },
                           "/Home/clip.mp4", T) == "file",
      "the Trash, a folder, a launcher, a Lua file in any case, a dot-file "
      .. "that is only a name, and a film")

if failed == 0 then
  print(("PASS: %d checks on what a right click offers (a folder, a file, a "
         .. "Lua file, a launcher, several, the Trash, the window, a place "
         .. "and a drive - each only what applies to it)."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
