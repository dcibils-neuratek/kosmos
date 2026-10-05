-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The file library's paths, on this computer (`user/lib/files.lua`): a path
-- typed made whole - `.` and `..` walked, the root kept the root, a mount's
-- own spelling - a folder made with every one above it, and a tree removed
-- with how much went counted, and how much before a refusal stopped it.
--
-- Each of these was written out again in the programs and libraries that
-- needed one, until the review before 0.11; this holds the one copy.
--
--   build/host/lua tools/test_files.lua

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

--------------------------------------------------------------------------
-- A filesystem kept here: a folder is made in one that is there, and a
-- folder with anything in it is not deleted - as the servers have it.
--------------------------------------------------------------------------

local nodes = { ["/"] = "directory", ["/Home"] = "directory" }
local refuse = {}                  -- paths whose delete is refused

local function parent(path) return path:match("^(.*)/[^/]+$") or "/" end

local function children(dir)
  local out = {}

  for path in pairs(nodes) do
    if path ~= "/" and parent(path) == (dir == "/" and "" or dir) then
      out[#out + 1] = path:match("([^/]+)$")
    end
  end

  table.sort(out)
  return out
end

fs = {
  getattr = function(path)
    return nodes[path] and { kind = nodes[path], size = 0 } or nil
  end,
  list = function(path)
    return nodes[path] == "directory" and children(path) or nil
  end,
  send = function(path, req)
    if req.type == "mkdir" then
      if nodes[parent(path)] ~= "directory" then return nil, "no such folder" end
      nodes[path] = "directory"
      return { ok = true }
    end

    if req.type == "delete" then
      if refuse[path] then return nil, "refused" end
      if nodes[path] == "directory" and #children(path) > 0 then
        return nil, "the directory is not empty"
      end
      nodes[path] = nil
      return { ok = true }
    end

    return nil, "not here"
  end,
  -- `/home/x` typed is `/Home/x`, as the namespace answers it.
  canonical = function(path)
    if path:lower():sub(1, 5) == "/home" and (#path == 5 or path:sub(6, 6) == "/") then
      return "/Home" .. path:sub(6)
    end
    return path
  end,
}

use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local files = dofile("user/lib/files.lua")

--------------------------------------------------------------------------
-- A path typed, made whole.
--------------------------------------------------------------------------

check(files.abs("notes.txt", "/Home/Desktop") == "/Home/Desktop/notes.txt", "a name, from where you are")
check(files.abs("/Kosmos/Apps", "/Home") == "/Kosmos/Apps", "a whole path, as it is")
check(files.abs(nil, "/Home/Desktop") == "/Home/Desktop" and files.abs("", "/Home") == "/Home",
      "nothing named is where you are")
check(files.abs("x", "/") == "/x", "from the root, one slash")
check(files.abs("../Music/song.mp3", "/Home/Desktop") == "/Home/Music/song.mp3",
      "`..` walked: " .. files.abs("../Music/song.mp3", "/Home/Desktop"))
check(files.abs("./a/./b/..", "/Home") == "/Home/a", "`.` dropped, and `..` after a name undoes it")
check(files.abs("../../..", "/Home/Desktop") == "/", "`..` at the root stays at the root")
check(files.abs("/home/desktop/../notes", "/") == "/Home/notes",
      "in the mount's own spelling: " .. files.abs("/home/desktop/../notes", "/"))
check(files.abs("/Home/", "/") == "/Home", "no slash left at the end")

check(files.join("/", "Home") == "/Home" and files.join("/Home/", "x") == "/Home/x",
      "a name joined to a folder, one slash between")

--------------------------------------------------------------------------
-- A folder and every one above it.
--------------------------------------------------------------------------

check(files.make_folder("/Home/Preferences/browser/history") == true
      and nodes["/Home/Preferences"] and nodes["/Home/Preferences/browser"]
      and nodes["/Home/Preferences/browser/history"],
      "a folder made with the two above it that were missing")
check(files.make_folder("/Home/Preferences") == true, "a folder that is there is made already")

do
  local ok, why = files.make_folder("/Nowhere/x")

  check(ok == nil and tostring(why):find("/Nowhere", 1, true),
        "a folder under something that cannot be made is refused, naming it: " .. tostring(why))
end

--------------------------------------------------------------------------
-- A tree removed, and counted.
--------------------------------------------------------------------------

nodes["/Home/t"] = "directory"
nodes["/Home/t/a"] = "file"
nodes["/Home/t/b"] = "directory"
nodes["/Home/t/b/c"] = "file"
nodes["/Home/t/b/d"] = "file"

do
  local ok, n = files.remove("/Home/t")

  check(ok == true and n == 5 and nodes["/Home/t"] == nil,
        "a tree of five removed, all five counted: " .. tostring(n))
end

nodes["/Home/u"] = "directory"
nodes["/Home/u/a"] = "file"
nodes["/Home/u/b"] = "file"
nodes["/Home/u/c"] = "file"
refuse["/Home/u/c"] = true

do
  local ok, why, before = files.remove("/Home/u")

  check(ok == nil and why == "refused" and before == 2 and nodes["/Home/u/c"] and nodes["/Home/u"],
        ("a refusal stops it, with the two that went before said: %s, %s"):format(tostring(why),
                                                                              tostring(before)))
end

if failed == 0 then
  print(("PASS: %d checks on the file library's paths (a path typed made whole, `..` "
         .. "walked and the root kept, a folder and every one above it, and a tree "
         .. "removed with what went counted)."):format(checks))
else
  print(("FAIL: %d of %d checks on the file library's paths"):format(failed, checks))
  os.exit(1)
end
