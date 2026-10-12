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
--
-- Diego, 12 October 2026: "where we can see all installed packages, remove
-- them, install new ones, update packages to new versions", "and packages
-- might have dependencies?", "like an APT package system?" (`docs/packages.md`).
-- All of it is `/Kosmos/Libraries/packages.lua`'s, which `install` and the
-- Packages window share; this is its text at the prompt.

local files = use("/Kosmos/Libraries/files.lua")
local packages = use("/Kosmos/Libraries/packages.lua")

local ok = packages.command(files.words(args), function(line) print("pkg: " .. line) end)

if not ok then sys.exit(1) end
