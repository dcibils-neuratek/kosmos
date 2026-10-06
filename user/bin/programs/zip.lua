-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `zip <archive> <path>...` - a zip made of files and folders (`roadmap.md`
-- 6v).
--
--   zip /Home/roms.zip /Home/roms
--   zip Archive.zip notes.txt photos
--   zip --job /Temporary/tracker-zip-1    what Tracker starts: what to do in
--                                         that file, how it is going beside it
--
-- A program of its own rather than work inside Tracker, which is one Lua
-- loop: compressing a large folder there would stop the window answering
-- until it was done. Here the window keeps working, Stop is a file this
-- program notices, and the prompt has the same program for free
-- (`docs/rightclick.html`, answer 3). The work is `zip.lua`'s; the bytes are
-- the compress kit's.

local zip = use("/Kosmos/Libraries/zip.lua")
local files = use("/Kosmos/Libraries/files.lua")

local words = {}

for _, w in ipairs(files.words(args)) do words[#words + 1] = w end

if words[1] == "--job" and words[2] then
  local ok, why = zip.job(words[2], zip.write)

  print(ok and ("zip: made " .. tostring((fs.read(words[2]) or {}).to))
        or ("zip: " .. tostring(why)))
  return
end

if #words < 2 then
  print("usage: zip <archive> <path>...")
  return
end

local here = cwd or "/Home"
local paths = {}

for i = 2, #words do paths[#paths + 1] = files.abs(words[i], here) end

local to = files.abs(words[1], here)

if fs.getattr(to) then
  print(("zip: %s is there already"):format(to))
  return
end

local ok, why = zip.write{ paths = paths, to = to }

print(ok and ("zip: made " .. to) or ("zip: " .. tostring(why)))
