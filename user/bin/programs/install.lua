-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `pkg install`, said shorter.
--
--   install                    every package the server offers, as `pkg` lists them
--   install doom               `pkg install doom`: it, and what it depends on
--   install all                every one
--   install --from 192.168.1.38:47701 ...   a server other than the boot's
--
-- Where Kosmos's packages began, on 12 October 2026 (Diego: "perhaps add a
-- program in kosmos that requests those from the mac as needed?"); kept as
-- the short form of `pkg install` now that `pkg` lists, removes and updates
-- too (`docs/packages.md`). One library does the work for both.

local files = use("/Kosmos/Libraries/files.lua")
local packages = use("/Kosmos/Libraries/packages.lua")

local words = files.words(args)
local asked = false

for _, w in ipairs(words) do
  if w ~= "--from" and not w:find(":", 1, true) then asked = true end
end

if asked then table.insert(words, 1, "install") end

local ok = packages.command(words, function(line) print("install: " .. line) end)

if not ok then sys.exit(1) end
