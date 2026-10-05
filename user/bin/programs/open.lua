-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- open: start an application on the desktop, from anywhere.
--
--   open tracker
--   open /Home/Apps/Hello/hello.lua
--   open music /Home/Music
--
-- **Asked of the window manager, as Tracker and the Deskbar ask it.** A
-- program run at the machine's prompt, or over Telnet, is not the desktop's
-- child and cannot open a window of its own - so this sends `launch` to
-- `/Running/wm`, which every process can reach, and the window manager
-- starts it as it starts anything pressed in the Deskbar. So an application
-- written on the Mac and pushed here opens on the screen (`roadmap.md`,
-- remote; Diego: "We could also even write Lua apps in the Mac and push them
-- to the m700").

local program, rest = tostring(args or ""):match("^%s*(%S+)%s*(.-)%s*$")

if not program then
  print("open: open <application> [arguments]")
  return
end

-- A path from where this was typed, or a name the window manager finds.
if program:sub(1, 1) ~= "/" and (program:find("/", 1, true) or program:match("%.lua$")) then
  program = use("/Kosmos/Libraries/files.lua").abs(program, cwd)
end

if not fs.getattr("/Running/wm") then
  print("open: the desktop is not running, so there is nowhere to open " .. program)
  error("no desktop", 0)
end

local reply, why = fs.send("/Running/wm", { type = "launch", program = program, args = rest })

if not reply then
  print("open: the desktop did not answer: " .. tostring(why))
  error("no answer", 0)
end

if reply.ok == false then
  print("open: " .. tostring(reply.error))
  error("refused", 0)
end

print("open: started " .. program)
