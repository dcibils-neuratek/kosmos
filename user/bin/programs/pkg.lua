-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Packages from the package server: list, install, remove, update.
--
--   pkg                       every package the server offers, and its state here
--   pkg install doom quake    those, and what they depend on, first
--   pkg install all           every one
--   pkg remove doom           exactly the files it installed; refused while an
--                             installed package needs it, unless both go
--   pkg update                every installed package with another version
--   pkg ... --from 192.168.1.38:47701    a server other than the boot's
--   pkg ... --report /Temporary/x.txt     what it says, written there too
--
-- Diego, 12 October 2026: "where we can see all installed packages, remove
-- them, install new ones, update packages to new versions", "and packages
-- might have dependencies?", "like an APT package system?" (`docs/packages.md`).
-- All of it is `/Kosmos/Libraries/packages.lua`'s, which `install` and the
-- Packages window share; this is its text at the prompt.

local files = use("/Kosmos/Libraries/files.lua")
local packages = use("/Kosmos/Libraries/packages.lua")

--
-- **`--report FILE`**: everything said written to FILE as well, a line at a
-- time, and `done ok` or `done failed` last - how the Packages window follows
-- an install it started in a process of its own and does not wait for
-- (`docs/packages.md`; nothing on the desktop waits on a server).
--
local words, report = files.words(args), nil

for i = #words - 1, 1, -1 do
  if words[i] == "--report" then
    report = words[i + 1]
    table.remove(words, i + 1)
    table.remove(words, i)
  end
end

local said = {}

local function say(line)
  print("pkg: " .. line)

  if report then
    said[#said + 1] = line
    fs.write(report, table.concat(said, "\n") .. "\n")
  end
end

local ok = packages.command(words, say)

if report then
  said[#said + 1] = ok and "done ok" or "done failed"
  fs.write(report, table.concat(said, "\n") .. "\n")
end

if not ok then sys.exit(1) end
