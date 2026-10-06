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

--------------------------------------------------------------------------
-- Arguments, written and read (`testing.md` 18.414): a path with a space
-- in it is one word, every awkward name comes back as it went, and every
-- string a program was started with before still reads as it did.
--------------------------------------------------------------------------

local function same(a, b)
  if #a ~= #b then return false end
  for i = 1, #a do if a[i] ~= b[i] then return false end end
  return true
end

local function show(list)
  local out = {}
  for i, w in ipairs(list) do out[i] = ("[%s]"):format(w) end
  return table.concat(out, " ")
end

-- Nothing that needs no quotes gets them: an invocation is what it was.
check(files.quote("/Home/notes.txt") == "/Home/notes.txt", "a plain path is not quoted")
check(files.quote("--size") == "--size" and files.quote("2") == "2", "nor a flag, nor a number")
check(files.quote("-leading-dash") == "-leading-dash", "nor a leading dash")
check(files.quote([[C:\back\slash]]) == [[C:\back\slash]], "nor a backslash alone")
check(files.quote("/Home/My Pictures/sea photo.png") == '"/Home/My Pictures/sea photo.png"',
      "a space is quoted: " .. files.quote("/Home/My Pictures/sea photo.png"))
check(files.quote("") == '""', "an empty word is a pair of quotes")
check(files.quote('say "hi"') == [["say \"hi\""]], "a quote inside is \\\" : " .. files.quote('say "hi"'))

-- Round trips: each awkward name, alone and among others, comes back whole.
local AWKWARD = {
  "/Home/My Pictures/sea photo.png",
  "/Home/Shares/MACPEER/diego’s Public Folder/a b.png",   -- UTF-8, a curly apostrophe
  'a "quoted" name',
  [[back\slash]], [[ends in a backslash\]], [[two \\ inside]], [[\"]],
  "tab\there", "  leading and trailing  ", "-n", "--size", "",
  "naïve café/日本語 ファイル.txt", '"', "\\", "x\"y",
}

for _, name in ipairs(AWKWARD) do
  local back = files.words(files.quote(name))

  check(#back == 1 and back[1] == name,
        ("one word round trip: %q -> %s -> %s"):format(name, files.quote(name), show(back)))
end

do
  local line = files.line(AWKWARD)
  local back = files.words(line)

  check(same(back, AWKWARD), "the whole list round trip: " .. line .. " -> " .. show(back))
end

-- What programs were started with before, read as before.
check(same(files.words("/Home/notes.txt"), { "/Home/notes.txt" }), "one path")
check(same(files.words("  --size 2  --carry /Temporary/x  --play "),
           { "--size", "2", "--carry", "/Temporary/x", "--play" }), "flags as before")
check(same(files.words("/bin icons desktop"), { "/bin", "icons", "desktop" }), "Tracker's words")
check(same(files.words(""), {}) and same(files.words("   "), {}), "nothing is no words")
check(same(files.words([[C:\a\b x]]), { [[C:\a\b]], "x" }), "a backslash outside quotes is itself")

-- Quoted and plain pieces of one word join, as a shell's do.
check(same(files.words([[--carry="/Home/a b"/c d]]), { "--carry=/Home/a b/c", "d" }),
      "pieces join into one word")
-- A quote left open runs to the end, rather than losing what was typed.
check(same(files.words([[open "/Home/My Pictures]]), { "open", "/Home/My Pictures" }),
      "an open quote runs to the end")
-- An unknown escape inside quotes is a backslash and the character.
check(same(files.words([["a\nb"]]), { [[a\nb]] }), "\\n inside quotes is two characters")

-- At most some words, and the rest exactly as written.
do
  local first, rest = files.words([[  photo   "/Home/My Pictures/a b.png" --fit  ]], 1)

  check(same(first, { "photo" }) and rest == [["/Home/My Pictures/a b.png" --fit]],
        ("the first word and the rest: %s | %s"):format(show(first), tostring(rest)))

  first, rest = files.words([["/Home/a b.lua" one "two three"]], 1)
  check(first[1] == "/Home/a b.lua" and same(files.words(rest), { "one", "two three" }),
        "a quoted first word, and the rest still quoted")

  first, rest = files.words("cd", 1)
  check(same(first, { "cd" }) and rest == "", "a word and nothing after it")
end

if failed == 0 then
  print(("PASS: %d checks on the file library's paths (a path typed made whole, `..` "
         .. "walked and the root kept, a folder and every one above it, a tree "
         .. "removed with what went counted, and arguments quoted and split "
         .. "back whole)."):format(checks))
else
  print(("FAIL: %d of %d checks on the file library's paths"):format(failed, checks))
  os.exit(1)
end
