-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What a file is, and which program handles it.
--
-- `user/lib/filetypes.lua` was written with a branch nothing used: a file's
-- *type* is an attribute set by whoever wrote it, and the extension is only
-- the fallback for files written before anything set one. The launcher is
-- the first thing to use it, so this is the first test of it.
--
-- Pure Lua over two tables, so it needs no machine.
--
--   build/host/lua tools/test_filetypes.lua

local types = dofile("user/lib/filetypes.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

--------------------------------------------------------------------------
-- The extension, for a file with nothing else to say.
--------------------------------------------------------------------------

check(types.kind_of("/home/notes.txt") == "txt",
      "a .txt is a txt")

check(types.opener("/home/notes.txt") == "editor",
      "and the editor opens it")

check(types.kind_of("/home/nothing") == nil,
      "a file with no extension has no type")

check(types.opener("/home/nothing") == nil,
      "so nothing claims it")

--
-- A leading dot is not an extension, which was a real bug: `.appearance`
-- read as a file of type "appearance" and put a word in Tracker's Kind
-- column that nothing in the system had heard of.
--
check(types.kind_of("/home/.appearance") == nil,
      "a name that merely starts with a dot has no extension")

--------------------------------------------------------------------------
-- The attribute wins, and that is the branch that matters.
--------------------------------------------------------------------------

check(types.kind_of("/home/notes.txt", { type = "book" }) == "book",
      "the type attribute beats the extension")

check(types.kind_of("/home/no-extension-at-all", { type = "png" }) == "png",
      "and gives a type to a file whose name could not")

check(types.opener("/home/anything", { type = "pdf" }) == "pdfview",
      "the program follows from the type, not from the name")

--------------------------------------------------------------------------
-- A launcher, which is the first real user of any of this.
--------------------------------------------------------------------------

check(types.kind_of("/home/Deskbar/Demos/doom",
                    { kind = "launcher", type = "launcher" }) == "launcher",
      "a launcher written today is of type launcher")

--
-- And one written before the type attribute existed. There were
-- thirty-eight of those on the first machine this ran on, and `kind` is
-- what they carry - so reading it here is what saves a migration to teach
-- them a word they already knew.
--
check(types.kind_of("/home/Desktop/Drive", { kind = "launcher" }) == "launcher",
      "a launcher with only `kind` is still a launcher")

check(types.opener("/home/Desktop/Drive", { kind = "launcher" })
      == "launcheredit",
      "and the launcher editor is what handles the type")

--
-- The name says nothing, and must not have to. `Drive.launcher` under an
-- icon would be the machinery showing through, so a launcher is never
-- identified by its name.
--
check(types.kind_of("/home/Desktop/Drive") == nil,
      "a launcher is not recognised by its name, because it has no mark "
      .. "in its name to recognise")

--
-- A directory that happens to be called something.txt is still a directory
-- to whoever asked; `kind_of` answers about the *name* unless told
-- otherwise, and Tracker tests `kind == "directory"` before it ever asks.
-- This pins the order the two are read in.
--
check(types.kind_of("/home/odd.txt", { kind = "directory" }) == "txt",
      "only a launcher is read out of `kind`; anything else falls through "
      .. "to the extension")

if failed == 0 then
  print(("PASS: %d checks on what a file is and what opens it, on this "
         .. "machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
