-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's favorites, on the Mac (`user/lib/favorites.lua`, `roadmap.md`
-- 6zz d3): a file's name from a page's title - slashes, control characters,
-- a leading dot, a title of nothing, one too long cut at a character - and
-- the folder over an `fs` kept in memory: a favorite made with its type,
-- address and order, kept once, a name taken by another page numbered, the
-- order they were starred with one put there by hand first, folders, every
-- address found through them, removed wherever it is, and a file opened as
-- its page only when it is a favorite.

local favorites = assert(loadfile("user/lib/favorites.lua"))()

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

--------------------------------------------------------------------------
-- Names.
--------------------------------------------------------------------------

check(favorites.name_for("Dam - Wikipedia") == "Dam - Wikipedia", "a title as it is")
check(favorites.name_for("AC/DC: a / b") == "AC DC: a b",
      "a slash is no part of a name, and spaces run together")
check(favorites.name_for("  .hidden\ttitle\n ") == "hidden title",
      "no leading dot to hide it, no control characters, nothing at the ends")
check(favorites.name_for("", "https://www.lua.org/manual/5.4/") == "www.lua.org",
      "a page with no title is named by its host")
check(favorites.name_for(nil, "/Home/notes.html") == "Home notes.html",
      "and a file with none by its path")

do
  local long = ("é"):rep(40)                     -- 80 bytes
  local name = favorites.name_for(long)

  check(#name <= 64 and #name == 64 and name == ("é"):rep(32),
        "a long title cut to 64 bytes at a character: " .. #name)
end

--------------------------------------------------------------------------
-- The folder, over an fs in memory.
--------------------------------------------------------------------------

local files, dirs = {}, { ["/Home"] = true }

fs = {}

function fs.getattr(path)
  if dirs[path] then
    local out = { kind = "directory" }

    for k, v in pairs(type(dirs[path]) == "table" and dirs[path] or {}) do out[k] = v end

    return out
  end

  local f = files[path]

  if not f then return nil end

  local out = { kind = "file", size = #f.data }

  for k, v in pairs(f.attrs) do out[k] = v end

  return out
end

function fs.setattr(path, t)
  local f = files[path]

  if not f then return nil end

  for k, v in pairs(t) do f.attrs[k] = v end

  return true
end

function fs.write(path, data)
  files[path] = files[path] or { attrs = {} }
  files[path].data = data
  return true
end

-- Names in byte order, as `/Home`'s server answers.
function fs.list(dir)
  local out = {}
  local pat = "^" .. dir:gsub("%p", "%%%0") .. "/([^/]+)$"

  for path in pairs(files) do
    local name = path:match(pat)

    if name then out[#out + 1] = name end
  end

  for path in pairs(dirs) do
    local name = path:match(pat)

    if name then out[#out + 1] = name end
  end

  table.sort(out)
  return out
end

function fs.send(path, msg)
  if msg.type == "mkdir" then
    dirs[path] = true
  elseif msg.type == "delete" then
    files[path] = nil
  end

  return true
end

local DIR = favorites.DIR

do
  local path = favorites.add("https://en.wikipedia.org/wiki/Dam", "Dam - Wikipedia")
  local a = path and fs.getattr(path)

  check(dirs[DIR] and path == DIR .. "/Dam - Wikipedia",
        "/Home/Favorites made, the favorite named by its title: " .. tostring(path))
  check(a and a.type == "favorite" and a.address == "https://en.wikipedia.org/wiki/Dam"
        and a.order == 1 and a.size == 0,
        "an empty file, its type, address and order its attributes")
  check(favorites.add("https://en.wikipedia.org/wiki/Dam", "Another title") == path,
        "a page already kept is that favorite, not a second")
end

do
  favorites.add("https://www.lua.org/", "Lua")
  local second = favorites.add("https://lua.org/manual", "Lua")

  check(second == DIR .. "/Lua 2", "a name another page has is numbered: " .. tostring(second))

  -- One put there by hand, with no order, and a folder Tracker made.
  fs.write(DIR .. "/Zed", "")
  fs.setattr(DIR .. "/Zed", { type = "favorite", address = "http://zed.example/" })
  dirs[DIR .. "/Kosmos docs"] = true
  favorites.add("asset:tutorial/cafesa3d/index.html", "Cafesa3D tutorial",
                DIR .. "/Kosmos docs")

  -- And something that is not a favorite at all.
  fs.write(DIR .. "/notes.txt", "mine")

  local names = {}

  for _, e in ipairs(favorites.read()) do names[#names + 1] = e.name end

  check(table.concat(names, ",") == "Kosmos docs,Zed,Dam - Wikipedia,Lua,Lua 2",
        "those with no order first by name, then as they were starred, and "
        .. "only favorites and folders: " .. table.concat(names, ","))

  local inside = favorites.read(DIR .. "/Kosmos docs")

  check(#inside == 1 and inside[1].address == "asset:tutorial/cafesa3d/index.html",
        "a folder's own favorites")

  local all = favorites.all()

  check(all["asset:tutorial/cafesa3d/index.html"] == DIR .. "/Kosmos docs/Cafesa3D tutorial"
        and all["http://zed.example/"] == DIR .. "/Zed"
        and all["https://lua.org/manual"] == DIR .. "/Lua 2",
        "every address found, through the folders")
  check(all["mine"] == nil, "and nothing that is not a favorite")
end

do
  check(favorites.address_of(DIR .. "/Lua") == "https://www.lua.org/",
        "a favorite's file opens as its page")
  check(favorites.address_of(DIR .. "/notes.txt") == nil
        and favorites.address_of(DIR .. "/nothing") == nil,
        "and a file that is not one, or is not there, as nothing")
end

do
  favorites.add("asset:tutorial/cafesa3d/index.html", "Again")   -- top level too

  local gone = favorites.remove("asset:tutorial/cafesa3d/index.html")

  check(gone == 2 and favorites.all()["asset:tutorial/cafesa3d/index.html"] == nil,
        "removed wherever it was kept: " .. gone)
  check(favorites.all()["https://www.lua.org/"] ~= nil, "and nothing else")
end

--
-- **Moved, as dragging on the bar does** (6zz d3): a place in the order as
-- it is now - before what is there, or after the last - the folder's order
-- written again; and one added at a place, or added again where it already
-- is, put there.
--
do
  local dir = "/Home/Order"

  local function names()
    local out = {}

    for _, e in ipairs(favorites.read(dir)) do out[#out + 1] = e.name end

    return table.concat(out, " ")
  end

  local a = favorites.add("http://a.example/", "A", dir)
  local b = favorites.add("http://b.example/", "B", dir)
  local c = favorites.add("http://c.example/", "C", dir)

  check(names() == "A B C", "three in the order they came: " .. names())
  check(favorites.move(c, 1, dir) == 1 and names() == "C A B",
        "the last put first: " .. names())
  check(favorites.move(c, 4, dir) == 3 and names() == "A B C",
        "and past the end, last again: " .. names())
  check(favorites.move(a, 3, dir) == 2 and names() == "B A C",
        "put before the third, it is second: " .. names())
  check(favorites.move(b, 1, dir) == 1 and names() == "B A C", "where it is, it stays")
  check(favorites.move(dir .. "/Nothing", 1, dir) == nil, "one that is not there is nil")

  favorites.add("http://d.example/", "D", dir, 2)
  check(names() == "B D A C", "one added at a place is there: " .. names())

  favorites.add("http://c.example/", "C again", dir, 1)
  check(names() == "C B D A", "one kept already, added at a place, moves there: " .. names())

  local orders = {}

  for _, e in ipairs(favorites.read(dir)) do orders[#orders + 1] = tostring(e.order) end

  check(table.concat(orders, " ") == "1 2 3 4", "and the order is written one to four: "
        .. table.concat(orders, " "))
end

if failures == 0 then
  print(("PASS: %d checks on the browser's favorites, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the browser's favorites."):format(failures, checks))
os.exit(1)
