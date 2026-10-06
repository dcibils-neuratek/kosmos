-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `unzip <archive> [<folder>]` - a zip's files, into a folder of their own
-- (`roadmap.md` 6v).
--
--   unzip /Home/roms.zip                   into /Home/roms, or roms 2 ...
--   unzip roms.zip /Home/games
--   unzip --job /Temporary/tracker-zip-2   what Tracker starts
--
-- Into a new folder, always: named after the archive and numbered when that
-- name is taken, as Tracker's New folder is, so opening a zip never writes
-- over anything - and a zip that names a place outside that folder is
-- refused before anything is written (`zip.lua`).

local zip = use("/Kosmos/Libraries/zip.lua")
local files = use("/Kosmos/Libraries/files.lua")

local words = {}

for _, w in ipairs(files.words(args)) do words[#words + 1] = w end

if words[1] == "--job" and words[2] then
  local ok, why = zip.job(words[2], zip.extract)

  print(ok and ("unzip: opened into " .. tostring((fs.read(words[2]) or {}).into))
        or ("unzip: " .. tostring(why)))
  return
end

if #words < 1 then
  print("usage: unzip <archive> [<folder>]")
  return
end

local here = cwd or "/Home"
local from = files.abs(words[1], here)
local into = words[2] and files.abs(words[2], here)

if not into then
  local dir = files.parent(from)
  local stem = (from:match("([^/]+)$") or "archive"):gsub("%.[Zz][Ii][Pp]$", "")

  into = files.join(dir, files.free_name(dir, stem) or stem)
end

local ok, why = zip.extract{ from = from, into = into }

print(ok and ("unzip: opened into " .. into) or ("unzip: " .. tostring(why)))
