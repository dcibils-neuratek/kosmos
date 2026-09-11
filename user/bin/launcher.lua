-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Make a launcher: an empty node whose attributes say what to start.
--
--   launcher /home/Desktop/Drive tracker /
--   launcher /home/Desktop/Doom doom
--   launcher --icon App_StyledEdit /home/Desktop/Notes editor /home/notes.txt
--
-- The first word after the path is the program, as the Deskbar names it -
-- `tracker` - or a whole path, and the rest of the line is its arguments,
-- spaces and all. Opening a launcher in Tracker asks the window manager to
-- start that program with those arguments, which is exactly what choosing
-- it in the Deskbar does, so a launcher can start nothing the Deskbar
-- cannot.
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

if not fs.getattr(path) then
  local ok, err = fs.write(path, "")

  if not ok then
    print("launcher: " .. tostring(err))
    return
  end
end

local ok, err = fs.setattr(path, {
  kind = "launcher",
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
