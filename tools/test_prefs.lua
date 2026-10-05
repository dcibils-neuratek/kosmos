-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The settings kit on the host (`user/lib/prefs.lua`): a setting's default
-- until it is chosen, a choice written at once and kept with what another
-- program put in the same file, a default not written, a name that cannot
-- reach past /Home/Preferences, and the folder made when it is missing -
-- over a filesystem kept here, in copies, as a real one keeps them.

local files, dirs, writes = {}, {}, 0

local function copy(t)
  if type(t) ~= "table" then return t end
  local out = {}
  for k, v in pairs(t) do out[k] = copy(v) end
  return out
end

fs = {
  read = function(path)
    if files[path] == nil then return nil, "no such file" end
    return copy(files[path])
  end,
  write = function(path, value)
    local dir = path:match("^(.*)/[^/]+$")
    if not dirs[dir] then return nil, "no such folder: " .. dir end
    writes = writes + 1
    files[path] = copy(value)
    return true
  end,
  getattr = function(path)
    if dirs[path] then return { kind = "directory" } end
    if files[path] ~= nil then return { kind = "file" } end
    return nil
  end,
  -- One folder, in one that is there, as a filesystem makes them: a kit
  -- that made `browser/history` without `browser` is refused here too.
  send = function(path, req)
    if req.type == "mkdir" then
      if not dirs[path:match("^(.*)/[^/]+$")] then return nil, "no such folder" end
      dirs[path] = true
      return { ok = true }
    end
    return nil, "not here"
  end,
}

dirs["/Home"] = true

-- The file library the kit makes its folders with, as `use` reaches it.
use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local prefs = dofile("user/lib/prefs.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then checks = checks + 1 else fails = fails + 1 print("  " .. what) end
end

-- 1. Defaults until chosen; a choice written at once, the folder made.
local music = prefs.open("music", { volume = 80, shuffle = false, look = "vox" })

check(music.volume == 80 and music.shuffle == false, "a default was not what a setting read")
check(files["/Home/Preferences/music"] == nil, "opening wrote a file")

check(music:set("volume", 70) == true, "a setting was not written")
check(dirs["/Home/Preferences"] and files["/Home/Preferences/music"].volume == 70,
      "the setting is not in /Home/Preferences/music, or the folder was not made")
check(music.volume == 70, "a setting read the old value after it was set")

-- 2. Several at once; a default not written; set back, it leaves the file.
music:set{ shuffle = true, look = "vox" }
check(files["/Home/Preferences/music"].shuffle == true and files["/Home/Preferences/music"].look == nil,
      "a value equal to its default was written, or the other was not")
music:set("volume", 80)
check(files["/Home/Preferences/music"].volume == nil and music.volume == 80,
      "a setting set back to its default stayed in the file")
music:set("look", "classic")
music:set("look", nil)
check(files["/Home/Preferences/music"].look == nil and music.look == "vox",
      "a setting set to nil stayed in the file")
music:reset("shuffle")
check(files["/Home/Preferences/music"].shuffle == nil and music.shuffle == false,
      "reset did not take a setting back to its default")

-- 3. Another program's keys kept: Preferences writes, the application sets.
files["/Home/Preferences/music"] = { theme = "night" }
music:set("volume", 50)
check(files["/Home/Preferences/music"].theme == "night" and files["/Home/Preferences/music"].volume == 50,
      "a set lost what another program had put in the same file")
check(music:reload().theme == "night", "reload did not read what another program wrote")

local all = music:all()
check(all.volume == 50 and all.shuffle == false and all.look == "vox" and all.theme == "night",
      "all() was not the defaults under what was chosen")

-- 4. A table as a value is kept whole, whatever the default.
local dock = prefs.open("dock", { pins = { "tracker" } })
check(dock.pins[1] == "tracker", "a table default did not read")
dock:set("pins", { "tracker", "terminal" })
check(files["/Home/Preferences/dock"].pins[2] == "terminal", "a list was not kept")

-- 5. A name cannot reach past /Home/Preferences.
for _, bad in ipairs({ "../Apps/doom", "/etc", "a/b/c", "", "a/../b", "..", "with space" }) do
  check(not pcall(prefs.open, bad, {}), ("the name %q was taken"):format(bad))
end

check(pcall(prefs.open, "browser/settings", {}), "a word/word name was refused")

-- 6. A whole file, a folder, and a path.
check(prefs.write("filetypes", { mp4 = "play" }) == true
      and prefs.read("filetypes").mp4 == "play", "a whole table was not kept and read")
check(next((prefs.read("nothing"))) == nil, "a file that is not there did not read as {}")
check(prefs.folder("browser/history") == "/Home/Preferences/browser/history"
      and dirs["/Home/Preferences/browser"] and dirs["/Home/Preferences/browser/history"],
      "a folder was not made where it is said to be, with the one above it")
check(prefs.path("Authorities") == "/Home/Preferences/Authorities", "a path was not under Preferences")

-- 7. A method's name is read with :get.
local odd = prefs.open("odd", { set = "a setting called set" })
check(type(odd.set) == "function" and odd:get("set") == "a setting called set",
      "a setting named like a method could not be read with :get")

if fails == 0 then
  print(("PASS: %d checks on the settings kit (a default until chosen, a choice "
         .. "written at once and another's keys kept, a default never written, "
         .. "names held inside /Home/Preferences, folders made)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the settings kit."):format(fails, checks + fails))
os.exit(1)
