-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Make a launcher: an empty node whose attributes say what to start.
--
--   launcher /home/Desktop/Drive tracker /
--   launcher /home/Desktop/Doom doom
--   launcher --icon App_StyledEdit /home/Desktop/Notes editor /home/notes.txt
--
-- The first word after the path is what to start - a whole path like
-- `/bin/tracker.lua`, or a short name like `tracker`, which is stored as
-- the whole path either way. The rest of the line is its arguments, spaces
-- and all. Opening a launcher in Tracker asks the window manager to start
-- that program with those arguments, which is exactly what choosing it in
-- the Deskbar does, so a launcher can start nothing the Deskbar cannot -
-- including a Lua file that is nowhere near `/bin`.
--
-- The picture is the program's own, when its header declares one, and
-- `--icon` picks another: an asset from `assets/icons/` without its `.png`.
--
-- Nothing is inside the node. A launcher is what its attributes say, the
-- way a BeOS People file was a person with nothing in it.

local line = tostring(args or "")
local icon, rest = line:match("^%s*%-%-icon%s+(%S+)%s*(.*)$")

if not icon then rest = line end

local path, program, arguments = rest:match("^%s*(%S+)%s+(%S+)%s*(.-)%s*$")

if not path then
  print("usage: launcher [--icon Name] <path> <program> [arguments]")
  return
end

if not icon and not program:find("/") then
  local declared = fs.getattr("/bin/" .. program .. ".lua")

  icon = declared and declared.icon
end

--
-- Typed short, stored whole.
--
-- `handlers.launch` accepts either and completes a bare name to
-- `/bin/<name>.lua`, which is right at a prompt where somebody is typing.
-- It is wrong in a *file*: what a launcher records should say what it runs
-- without anybody having to know a rule the file cannot state. So the
-- convenience stays in the typing and the attribute is explicit.
--
-- This is also what makes `/home/mine.lua` and `/bin/doom.lua` look like
-- the same kind of thing in the editor, which they are.
--
if not program:find("/") then
  program = "/bin/" .. program .. ".lua"
end

if not fs.getattr(path) then
  local ok, err = fs.write(path, "")

  if not ok then
    print("launcher: " .. tostring(err))
    return
  end
end

local ok, err = fs.setattr(path, {
  kind = "launcher",

  -- The file's *type*, which is what `filetypes.lua` prefers and what
  -- Tracker's Kind column shows. `kind` says the same thing and is the
  -- floor under launchers written before this existed; new ones say it the
  -- general way, which is the way every other typed file will.
  type = "launcher",

  program = program,
  args = arguments,
  icon = icon,
})

if not ok then
  print("launcher: " .. tostring(err))
  return
end

print(("%s starts %s%s"):format(path, program,
                                (arguments ~= "") and (" " .. arguments) or ""))
