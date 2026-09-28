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
--   build/host/lua tools/test_filetypes.lua user/bin/apps/*.lua \
--       user/bin/apps/*/*.lua user/bin/programs/*.lua

local types = dofile("user/lib/filetypes.lua")

--
-- **The store, as the machine's serves it**: the applications and programs
-- in this tree, each reporting what its header says it opens - read here
-- the way `binfs.c` reads it, the opening comment block and no further. So
-- what is checked below is what the tree declares, not a table kept beside
-- it (`roadmap.md` 6z). An application in a folder of its own is served by
-- its main file's name, as `progs2c.py` serves it.
--
-- The files, as the Makefile hands them over: the host's Lua has no way
-- to list a directory.
local STORE = { ["/Kosmos/Apps"] = {}, ["/Kosmos/Programs"] = {},
                ["/Home/Apps"] = {} }

-- And the applications Kosmos builds to be installed (`docs/elf.md` step 5),
-- as a stick installs them: `user/installed/Doom/doom.lua` is
-- `/Home/Apps/Doom/doom.lua`, a folder with its program in it.
local INSTALLED = {}

for _, file in ipairs(arg) do
  local folder, main = file:match("^user/installed/([^/]+)/([^/]+)%.lua$")

  if folder and main == folder:lower() then
    STORE["/Home/Apps"][folder] = true
    INSTALLED["/Home/Apps/" .. folder .. "/" .. main .. ".lua"] = file
  end
end

for _, file in ipairs(arg) do
  local dir, name = file:match("^user/bin/(%a+)/(.+)%.lua$")
  local store = (dir == "apps") and STORE["/Kosmos/Apps"]
                or (dir == "programs") and STORE["/Kosmos/Programs"]

  -- `doom/doom.lua` is served as `doom.lua`; `doom/doomgame.lua` is not
  -- served at all.
  local folder, main = (name or ""):match("^([^/]+)/([^/]+)$")

  if store and name and (not folder or folder == main) then
    store[(main or name) .. ".lua"] = file
  end
end

local function opens_of(file)
  local words = nil

  for line in io.lines(file) do
    if line ~= "" and line:sub(1, 2) ~= "--" then break end

    local said = line:match("kosmos:%s*opens%s+(.*)$")

    if said then
      for w in said:lower():gmatch("[%w_]+") do
        words = words or {}
        words[#words + 1] = w
      end
    end
  end

  return words
end

-- A home with no choices in it, unless a check below makes one.
local saved = {}

fs = {
  list = function(dir)
    local names = {}

    for name in pairs(STORE[dir] or {}) do names[#names + 1] = name end

    return names
  end,
  getattr = function(path)
    local dir, name = path:match("^(.*)/([^/]+)$")
    local file = STORE[dir] and STORE[dir][name]

    return file and { opens = opens_of(file) } or nil
  end,
  read = function(path)
    local file = INSTALLED[path]

    if file then
      local f = io.open(file)
      local text = f:read("a")

      f:close()
      return text
    end

    return saved[path]
  end,
  write = function(path, value) saved[path] = value return true end,
  send = function() return true end,           -- the folder, made
}

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

check(types.kind_of("/Home/notes.txt") == "txt",
      "a .txt is a txt")

check(types.opener("/Home/notes.txt") == "editor",
      "and the editor opens it")

check(types.kind_of("/Home/nothing") == nil,
      "a file with no extension has no type")

check(types.opener("/Home/nothing") == nil,
      "so nothing claims it")

--
-- A leading dot is not an extension, which was a real bug: `.appearance`
-- read as a file of type "appearance" and put a word in Tracker's Kind
-- column that nothing in the system had heard of.
--
check(types.kind_of("/Home/.appearance") == nil,
      "a name that merely starts with a dot has no extension")

--------------------------------------------------------------------------
-- The attribute wins, and that is the branch that matters.
--------------------------------------------------------------------------

check(types.kind_of("/Home/notes.txt", { type = "book" }) == "book",
      "the type attribute beats the extension")

check(types.kind_of("/Home/no-extension-at-all", { type = "png" }) == "png",
      "and gives a type to a file whose name could not")

check(types.opener("/Home/anything", { type = "pdf" }) == "pdfview",
      "the program follows from the type, not from the name")

--------------------------------------------------------------------------
-- A launcher, which is the first real user of any of this.
--------------------------------------------------------------------------

check(types.kind_of("/Home/Deskbar/Demos/doom",
                    { kind = "launcher", type = "launcher" }) == "launcher",
      "a launcher written today is of type launcher")

--
-- And one written before the type attribute existed. There were
-- thirty-eight of those on the first machine this ran on, and `kind` is
-- what they carry - so reading it here is what saves a migration to teach
-- them a word they already knew.
--
check(types.kind_of("/Home/Desktop/Drive", { kind = "launcher" }) == "launcher",
      "a launcher with only `kind` is still a launcher")

check(types.opener("/Home/Desktop/Drive", { kind = "launcher" })
      == "launcheredit",
      "and the launcher editor is what handles the type")

--
-- The name says nothing, and must not have to. `Drive.launcher` under an
-- icon would be the machinery showing through, so a launcher is never
-- identified by its name.
--
check(types.kind_of("/Home/Desktop/Drive") == nil,
      "a launcher is not recognised by its name, because it has no mark "
      .. "in its name to recognise")

--
-- A directory that happens to be called something.txt is still a directory
-- to whoever asked; `kind_of` answers about the *name* unless told
-- otherwise, and Tracker tests `kind == "directory"` before it ever asks.
-- This pins the order the two are read in.
--
check(types.kind_of("/Home/odd.txt", { kind = "directory" }) == "txt",
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

local how = types.how_to_open("/Home/clock.lua", nil, app)

check(how and how.program == "/Home/clock.lua" and how.args == "",
      "an application opens as itself")

how = types.how_to_open("/Home/diego.lua", nil, console)

check(how and how.program == "terminal" and how.args == "/Home/diego.lua",
      "a console program opens in a Terminal, which is handed its path")

how = types.how_to_open("/Home/diego.lua", nil, nil)

check(how and how.program == "terminal",
      "a Lua file whose source could not be read still runs in a Terminal")

how = types.how_to_open("/Home/notes.txt")

check(how and how.program == "editor" and how.args == "/Home/notes.txt",
      "anything else opens in what handles its type, as before")

check(types.how_to_open("/Home/nothing") == nil,
      "and a file nothing claims still opens in nothing")

check(types.opener("/Home/diego.lua") == "editor",
      "the editor is still what handles a .lua, for Edit")

-- A film and a photograph (`roadmap.md` 6z): Video and Photo, whatever case
-- the extension is written in.
check(types.opener("/Home/magicword-clip.mp4") == "video",
      "an .mp4 is not opened by Video")
check(types.opener("/Home/think.JPG") == "photo" and types.opener("/Home/a.jpeg") == "photo",
      "a .jpg or .jpeg is not opened by Photo")

--------------------------------------------------------------------------
-- What opens what, from the headers (`roadmap.md` 6z).
--------------------------------------------------------------------------

check(types.opener("/Home/hello.wav") == "music"
      and types.opener("/Home/song.mp3") == "music",
      "a .wav and a .mp3 are not the Music's - the table this replaced sent a "
      .. ".wav to Play, which plays films")
check(types.opener("/Home/doom1.wad") == "doom"
      and types.opener("/Home/roms/zelda.sfc") == "snes"
      and types.opener("/Home/roms/mario.SMC") == "snes",
      "a Doom level and a cartridge are not opened by the applications that "
      .. "say they open them")
check(types.opener("/Home/notes.md") == "reader"
      and types.opener("/Home/page.html") == "browser"
      and types.opener("/Home/Desktop/Drive", { kind = "launcher" })
          == "launcheredit",
      "a note, a page and a launcher are not the Reader's, the Browser's and "
      .. "the launcher editor's")

check(types.opener("/Home/renders.zip") == "tracker",
      "a zip is not Tracker's, which opens one by extracting it")

local film = types.openers("mp4")

check(table.concat(film, ",") == "video,play",
      "a film is not opened by Video, then Play - an application before a "
      .. "program: " .. table.concat(film, ","))

-- A choice, and it is kept only while it differs from the default.
check(types.choose("mp4", "play") and types.opener("/Home/film.mp4") == "play"
      and saved[types.CHOICES].mp4 == "play",
      "choosing Play for a film did not make it what opens one")
check(types.choose("mp4", "video") and types.opener("/Home/film.mp4") == "video"
      and saved[types.CHOICES].mp4 == nil,
      "choosing the default back did not take the choice out of the file")
check(not types.choose("mp4", "doom"),
      "Doom was allowed to be what opens a film, which it says nothing of")

-- A choice for a program that no longer opens the type is not followed.
saved[types.CHOICES] = { mp4 = "gone" }
check(types.opener("/Home/film.mp4") == "video",
      "a choice naming a program that does not open the type was followed")
saved[types.CHOICES] = nil

-- The File types page: the drawing's groups, a row a type - two spellings
-- of one sharing it - and a choice only where there is one.
local page = types.page()
local group_names, by = {}, {}

for _, g in ipairs(page) do
  group_names[#group_names + 1] = g.name

  for _, r in ipairs(g.items) do by[r.tag] = r end
end

check(table.concat(group_names, ",") == "Documents,Pictures,Sound and film,Games,Kosmos",
      "the page's groups are not the drawing's: " .. table.concat(group_names, ","))
check(by[".jpg"] and by[".jpg"].note == ".jpeg the same" and not by[".jpeg"]
      and by[".jpg"].kind == "value" and by[".jpg"].value == "Photo",
      "a .jpeg is not in the .jpg's row, which names Photo")
check(by[".sfc"] and by[".sfc"].note == ".smc the same"
      and by[".sfc"].value == "Super Nintendo",
      "a cartridge's two spellings are not one row, the Super Nintendo's")
check(by[".mp4"] and by[".mp4"].kind == "choice"
      and by[".mp4"].note == "Play can open it too"
      and by[".mp4"].choices[1][1] == "video" and by[".mp4"].choices[2][2] == "Play",
      "a film's row is not a choice of Video, then Play, saying Play can too")
check(by[".lua"] and by[".lua"].note == "Opening runs it; this is what Edit uses",
      "a Lua file's row does not say that opening one runs it")
check(by[".mp4"].set("play") and saved[types.CHOICES].mp4 == "play"
      and by[".mp4"].set("video") and saved[types.CHOICES].mp4 == nil,
      "a choice made in the row is not the one kept, or the default not "
      .. "taken back out")

local found = types.page(nil, nil, "photo")
local tags = {}

for _, g in ipairs(found) do
  for _, r in ipairs(g.items) do tags[#tags + 1] = r.tag end
end

check(table.concat(tags, ",") == ".png,.jpg",
      "finding \"photo\" is not the two rows Photo opens: " .. table.concat(tags, ","))

-- What a kind of file is called, for Info (`roadmap.md` 6za): words, and
-- the extension named by itself where there are none.
check(types.describe("/Home/magicword-clip.mp4") == "Film"
      and types.describe("/Home/roms", { kind = "directory" }) == "Folder"
      and types.describe("/Home/Desktop/Doom", { kind = "launcher" }) == "Launcher"
      and types.describe("/Home/x.SFC") == "Super Nintendo cartridge",
      "a film, a folder, a launcher and a cartridge are not called what they are")
check(types.describe("/Home/Deskbar/Demos/quake", { kind = "hidden", type = "hidden" })
      == "Hidden from the Deskbar's menu"
      and types.opener("/Home/Deskbar/Demos/quake",
                       { kind = "hidden", type = "hidden" }) == nil,
      "a note that hides a shipped item is not called what it is, or opens "
      .. "something (`roadmap.md` 6zd)")
check(types.describe("/Home/data.xyz") == "XYZ file"
      and types.describe("/Home/README") == "File",
      "an extension with no words is not named by itself, or no extension not a File")

-- An opener by its window's name, not its file's.
check(types.app_name("pdfview") == "PDF" and types.app_name("video") == "Video"
      and types.app_name("newthing") == "Newthing",
      "an opener is not called what its window is called")

if failed == 0 then
  print(("PASS: %d checks on what a file is and what opens it, on this "
         .. "machine."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
