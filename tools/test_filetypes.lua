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

--------------------------------------------------------------------------
-- A program says what it is in its opening comment, and a Lua file opens by
-- running: an application as itself, anything else in a Terminal.
--------------------------------------------------------------------------

local app = "-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.\n"
         .. "-- clock: the time.\n--\n-- kosmos: application\n\nlocal ui = 1\n"
local console = "-- hello: says hello.\nprint(\"hello\")\n"
local late = "-- a program\nlocal s = \"-- kosmos: application\"\n"
local blank = "\n-- kosmos: application\n"

check(types.declares(app, "application"),
      "an application on the fourth line of its opening comment is one")

check(not types.declares(console, "application"),
      "a program that says nothing is not an application")

check(not types.declares(late, "application"),
      "the words in a string after the comment declare nothing")

check(types.declares(blank, "application"),
      "an empty line is still inside the opening comment, as binfs reads it")

check(not types.declares(nil, "application"),
      "no source declares nothing")

local how = types.how_to_open("/home/clock.lua", nil, app)

check(how and how.program == "/home/clock.lua" and how.args == "",
      "an application opens as itself")

how = types.how_to_open("/home/diego.lua", nil, console)

check(how and how.program == "terminal" and how.args == "/home/diego.lua",
      "a console program opens in a Terminal, which is handed its path")

how = types.how_to_open("/home/diego.lua", nil, nil)

check(how and how.program == "terminal",
      "a Lua file whose source could not be read still runs in a Terminal")

how = types.how_to_open("/home/notes.txt")

check(how and how.program == "editor" and how.args == "/home/notes.txt",
      "anything else opens in what handles its type, as before")

check(types.how_to_open("/home/nothing") == nil,
      "and a file nothing claims still opens in nothing")

check(types.opener("/home/diego.lua") == "editor",
      "the editor is still what handles a .lua, for Edit")

if failed == 0 then
  print(("PASS: %d checks on what a file is and what opens it, on this "
         .. "machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
