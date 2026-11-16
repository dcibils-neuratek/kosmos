-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A filter list converted into WebKit's content rules when the image is
-- built (`docs/adblock.md` B2): `assets/filters/easylist.txt` into the
-- JSON the WebKit Kit compiles, by the same `filters.lua` a machine runs,
-- so the image blocks from its first boot. The Makefile gzips what this
-- writes; the kit inflates it.
--
--   build/host/lua tools/filters_build.lua easylist.txt easylist.json

use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local filters = use("/Kosmos/Libraries/filters.lua")

local from, to = arg[1], arg[2]

if not from or not to then
  io.stderr:write("usage: filters_build.lua <list.txt> <rules.json>\n")
  os.exit(2)
end

local f = assert(io.open(from, "rb"))
local text = f:read("a")

f:close()

local rules, counts = filters.convert(text)

if #rules > filters.MAX_RULES then
  io.stderr:write(("%s: %d rules, over WebKit's %d\n"):format(from, #rules, filters.MAX_RULES))
  os.exit(1)
end

local out = assert(io.open(to, "wb"))

out:write(filters.encode(rules))
out:close()

local left = 0

for k, n in pairs(counts) do
  if k ~= "converted" and k ~= "comments" and k ~= "rules" then left = left + n end
end

print(("%s: %d filters into %d rules, %d left out"):format(from, counts.converted, #rules, left))
