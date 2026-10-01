-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The browser's favorites, as files (`roadmap.md` 6zz d3, `docs/browser.html`).
--
-- **Each favorite is a file in `/Home/Favorites`**, NetPositive's way - Diego,
-- of the drawing: "yes". An empty file whose attributes say what it is:
-- `type = "favorite"`, which is what makes Tracker open it in the browser
-- (`-- kosmos: opens favorite`) and draw it as a web page, and `address`,
-- the page it keeps. Its name is the page's title, so Tracker shows,
-- renames, moves and deletes favorites as it does any file, and a folder in
-- `/Home/Favorites` is a folder of them - on the bar as a menu, and in the
-- sidebar as a branch.
--
-- **In the order they were starred**, as Places are in the order they were
-- pinned (`places.lua`): `order` is one past the highest when one is made.
-- One that has none - a file put there by hand, a folder Tracker made -
-- comes first, by name without regard to case.
--
-- Pure: `fs` is the global every process has, and the host test hands it
-- one kept in memory (`tools/test_favorites.lua`).

local favorites = {}

favorites.DIR = "/Home/Favorites"
favorites.TYPE = "favorite"

-- The longest name `/Home` keeps, in bytes (`KFS_NAME_MAX`).
local NAME_MOST = 64

-- A name cut to `most` bytes at a character, never inside one, and with no
-- space left at its end.
local function cut(name, most)
  if #name <= most then return name end

  local n = most

  while n > 0 and ((name:byte(n + 1) or 0) & 0xC0) == 0x80 do n = n - 1 end

  return (name:sub(1, n):match("^(.-)%s*$"))
end

--
-- **A file's name from a page's title**: what cannot be in a name - a slash,
-- a control character - made a space, spaces run together, a leading dot
-- dropped so the file is not hidden, and cut at a character to what the
-- disk keeps. A page with no title is named by its address's host.
--
function favorites.name_for(title, address)
  local name = tostring(title or ""):gsub("[%c/]", " "):gsub("%s+", " ")
                                    :match("^[%s.]*(.-)%s*$")

  if name == "" then
    local bare = tostring(address or ""):gsub("^%a[%w+.-]*://", "")

    name = (bare:match("^[^/]+") or bare):gsub("/", " "):match("^[%s.]*(.-)%s*$")
  end

  if name == "" then name = "Favorite" end

  return cut(name, NAME_MOST)
end

local function by_order(a, b)
  local x, y = tonumber(a.order) or 0, tonumber(b.order) or 0

  if x ~= y then return x < y end

  local p, q = a.name:lower(), b.name:lower()

  if p ~= q then return p < q end

  return a.name < b.name
end

--
-- **What a folder of favorites holds**, in order: each `{ name, path,
-- address }` for a favorite and `{ name, path, folder = true }` for a
-- folder. Anything else kept there is somebody's own business and is left
-- out, as Places and the Deskbar leave it.
--
function favorites.read(dir)
  dir = dir or favorites.DIR

  local out = {}

  for _, name in ipairs(fs.list(dir) or {}) do
    if name:sub(1, 1) ~= "." then
      local path = dir .. "/" .. name
      local a = fs.getattr(path)

      if a and a.kind == "directory" then
        out[#out + 1] = { name = name, path = path, folder = true, order = a.order }
      elseif a and a.type == favorites.TYPE and type(a.address) == "string"
             and a.address ~= "" then
        out[#out + 1] = { name = name, path = path, address = a.address,
                          order = a.order }
      end
    end
  end

  table.sort(out, by_order)
  return out
end

--
-- **Every favorite's address, and the file that keeps it**, folders and all:
-- what the star asks - whether the page on screen is one - without a walk
-- of the folder on every frame.
--
function favorites.all(dir, into)
  into = into or {}

  for _, e in ipairs(favorites.read(dir or favorites.DIR)) do
    if e.folder then
      favorites.all(e.path, into)
    elseif not into[e.address] then
      into[e.address] = e.path
    end
  end

  return into
end

local function made(dir)
  if fs.getattr(dir) then return true end

  local at = ""

  for part in dir:gmatch("[^/]+") do
    at = at .. "/" .. part

    if not fs.getattr(at) then fs.send(at, { type = "mkdir" }) end
  end

  return fs.getattr(dir) ~= nil
end

--
-- **A page made a favorite**, at the end of the folder: its file, or nil and
-- why. One already kept is that one rather than a second. A name taken by
-- another page's favorite is given a number - "Lua 2" - rather than taking
-- that one's place.
--
function favorites.add(address, title, dir)
  dir = dir or favorites.DIR

  if type(address) ~= "string" or address == "" then
    return nil, "there is no address to keep"
  end

  if not made(dir) then return nil, ("%s could not be made"):format(dir) end

  local list = favorites.read(dir)
  local top = 0

  for _, e in ipairs(list) do
    if e.address == address then return e.path end

    top = math.max(top, tonumber(e.order) or 0)
  end

  local base = favorites.name_for(title, address)
  local name, n = base, 1

  while fs.getattr(dir .. "/" .. name) do
    n = n + 1

    local tail = " " .. n

    name = cut(base, NAME_MOST - #tail) .. tail
  end

  local path = dir .. "/" .. name

  if not fs.write(path, "") then return nil, "the file could not be written" end

  fs.setattr(path, { type = favorites.TYPE, address = address, order = top + 1 })
  return path
end

--
-- **A page a favorite no longer**: every file that keeps it, wherever in the
-- folders it is - the star says the page is a favorite, and a star that
-- stayed lit after it was pressed would be lying. How many went.
--
function favorites.remove(address, dir)
  local gone = 0

  for _, e in ipairs(favorites.read(dir or favorites.DIR)) do
    if e.folder then
      gone = gone + favorites.remove(address, e.path)
    elseif e.address == address then
      fs.send(e.path, { type = "delete" })
      gone = gone + 1
    end
  end

  return gone
end

--
-- **A file's page, when the file is a favorite**: what the browser does when
-- Tracker opens one - it is handed the file and shows the page.
--
function favorites.address_of(path)
  local a = type(path) == "string" and fs.getattr(path)

  if a and a.type == favorites.TYPE and type(a.address) == "string"
     and a.address ~= "" then
    return a.address
  end

  return nil
end

return favorites
