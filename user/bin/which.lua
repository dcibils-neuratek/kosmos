-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- which: where a program is, if it is anywhere.
--
--   which grep
--
-- A name at the prompt becomes `/bin/<name>.lua`, and this is that rule
-- said out loud plus the one question worth asking about it: does the
-- namespace answer for it.
--
-- **It cannot see the shell's own words.** `cd`, `pwd` and `help` are the
-- shell's, held in a table inside that process, and nothing outside can
-- ask what is in it. So a name this does not find may still be something
-- you can type - `/commands` is the list of those - and saying so is
-- better than reporting "not found" about a word that plainly works.

local name = args:match("^%s*(%S+)")

if not name then
  print("which: which <name>")
  return
end

local path = (name:sub(1, 1) == "/") and name or ("/bin/" .. name .. ".lua")
local attrs = fs.getattr(path)

if not attrs then
  print(name .. " is not in /bin")
  print("  `/commands` lists the words the shell answers to itself")
  return
end

print(path)
