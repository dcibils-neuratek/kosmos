-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- tcc: C compiled inside Kosmos, at the prompt (`docs/tinycc.md`, step C4).
--
--   tcc primes.c -o build/primes.elf        compiled and linked into an image
--   tcc a.c b.c -o build/both.elf           several files, one image
--
-- The same build the IDE does - `tccbuild.lua`, the C Kit, the developer
-- files in `/Home/Developer` - with its problems said as a compiler at a
-- prompt says them, `file:line: error: text`, one to a line. A program
-- starts in the image its header names (`-- kosmos: image build/primes.elf`),
-- as Doom does: this writes the file and starts nothing.

local files = use("/Kosmos/Libraries/files.lua")
local tccbuild = use("/Kosmos/Libraries/tccbuild.lua")

local words = files.words(args)
local sources, out, defines = {}, nil, {}
local i = 1

while i <= #words do
  if words[i] == "-o" then
    out = words[i + 1]
    i = i + 2
  elseif words[i]:match("^%-D.") then                    -- -Dname=value
    defines[#defines + 1] = words[i]:sub(3)
    i = i + 1
  else
    sources[#sources + 1] = words[i]
    i = i + 1
  end
end

if #sources == 0 or not out then
  print("usage: tcc file.c [more.c ...] [-Dname=value ...] -o build/name.elf")
  return
end

-- Paths as the prompt means them: from where this program was started.
for k, p in ipairs(sources) do sources[k] = files.abs(p, cwd) end
out = files.abs(out, cwd)

local r, why = tccbuild.build{ sources = sources, out = out, defines = defines }

if not r then
  print("tcc: " .. tostring(why))
  return
end

for _, p in ipairs(r.problems) do
  print(("%s:%d: %s: %s"):format(p.file or "tcc", p.line or 0, p.severity, p.text))
end

if r.ok then
  print(("tcc: %s, %.1f MB - built in %d ms"):format(out, (r.bytes or 0) / 1048576, r.milliseconds))
else
  print(("tcc: %d %s; nothing written"):format(#r.problems,
        #r.problems == 1 and "problem" or "problems"))
end
