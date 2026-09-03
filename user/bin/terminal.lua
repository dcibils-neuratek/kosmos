-- kosmos: application
-- A terminal, in a window.
--
--   wm terminal
--
-- Type a program's name and it runs *inside this window*: its output comes
-- here rather than to the machine's console.
--
--------------------------------------------------------------------------
-- How, and why it is barely any code.
--
-- This is a console server. The real one - the process that owns the serial
-- port and the keyboard - answers `read`, `write` and a couple of other
-- verbs at `/dev/console`, and every program in this system prints by
-- sending `write` to whatever is mounted there. None of them knows or can
-- ask what is behind it.
--
-- So a terminal is a process that speaks the same three verbs and hands
-- itself to its children as their `/dev/console`. `run` already takes a
-- `shares` table naming capabilities to pass under a path, and a namespace
-- mount replaces what was at that path - so the child gets exactly one
-- console and it is this window.
--
-- That is the whole trick, and it is the point of the design rather than a
-- clever use of it: a name resolves to a capability, nothing has a global
-- meaning, so "the console" is whatever this process was handed. BeOS could
-- not do this; the Terminal there talked to a device.
--
-- What it does not do yet: no scrollback beyond the buffer, no VT100
-- emulation, no job control. The escape sequences are M12's prerequisite
-- and are worth doing properly rather than partly.
--------------------------------------------------------------------------

local ui = use("/lib/ui.lua")

-- Where this window is. A shell's working directory belongs to the shell,
-- never to a server: a server is always told a whole path and knows nothing
-- about where anybody thinks they are.
local cwd = "/home"
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local W, H = 640, 420
local SCROLLBACK = 400          -- lines kept

local win, err = ui.window{ title = "Terminal", w = W, h = H, x = 90, y = 70 }

if not win then
  print("terminal: " .. tostring(err))
  return
end

local ep = sys.endpoint()

if not ep then
  print("terminal: no endpoint")
  return
end

--------------------------------------------------------------------------
-- What is on screen.
--------------------------------------------------------------------------

-- A trailing empty line, because `emit` appends to the last one. Without
-- it the first thing typed lands on the end of the banner.
local lines = {
  "Kosmos terminal. Type a program's name; `help` lists them.",
  "",
}
local input = ""
local busy = nil                -- the child that currently owns this console

local function emit(text)
  -- Whatever arrives, split on newlines and appended to the last line if it
  -- did not start with one. A `write` is a stream, not a line: `print` sends
  -- one ending in a newline and `write_text` splits long output into pieces
  -- that can end anywhere.
  for piece, newline in tostring(text):gmatch("([^\n]*)(\n?)") do
    if piece ~= "" then
      lines[#lines] = (lines[#lines] or "") .. piece
    end

    if newline == "\n" then
      lines[#lines + 1] = ""
    end
  end

  while #lines > SCROLLBACK do
    table.remove(lines, 1)
  end
end

local view = ui.view{ x = 8, y = 8, w = W - 16, h = H - 20 }

function view:draw(g)
  g:fill(0, 0, self.w, self.h, "console")
  g:frame(0, 0, self.w, self.h, self.focused and theme.ring or "line")

  --
  -- Measured in the *monospace* font, which is the one this window draws
  -- in. `gfx.font` is the interface font and answers for that one only, so
  -- a terminal that sized its grid with it laid out rows and columns for a
  -- face it was not using.
  --
  local MH = gfx.height("mono")
  local MW = math.max(1, gfx.measure("0", "mono"))

  local rows = (self.h - 6) // MH
  local columns = (self.w - 8) // MW

  -- The prompt is the last row, so the visible history is one short.
  local shown = {}

  for i = math.max(1, #lines - rows + 2), #lines do
    shown[#shown + 1] = lines[i]
  end

  for i, line in ipairs(shown) do
    -- `console_text`, not `text`: `text` is chosen to read against the
    -- window colour, and in a light theme that is black - which on a black
    -- console is nothing at all.
    g:text(4, 3 + (i - 1) * MH, line:sub(1, columns),
           "console_text", "console", "mono")
  end

  local y = 3 + #shown * MH
  local prompt = "> " .. input

  g:text(4, y, prompt:sub(1, columns), "good", "console", "mono")

  if busy then
    g:text(self.w - 12 * MW, 3, "running " .. busy,
           "text_dim", "console", "mono")
  end

  if self.focused then
    local cx = 4 + math.min(#prompt, columns) * MW
    g:fill(cx, y, MW, MH, "ring")
  end
end

view.focusable = true

--------------------------------------------------------------------------
-- Running something.
--------------------------------------------------------------------------

local function launch(text)
  local name, rest = text:match("^(%S+)%s*(.*)$")

  if not name then return end

  --------------------------------------------------------------------------
  -- Built in, because they change *this* process.
  --
  -- `cd` was the thing that made the Terminal look like a shell and not be
  -- one: it ran programs out of /bin, and a program cannot change its
  -- parent's working directory - so `cd` could only ever have been a
  -- builtin, and its absence gave "cd: no such program", which is true and
  -- unhelpful.
  --
  -- The rest are here for the same reason: they are about where you are,
  -- which is a fact this window owns.
  --------------------------------------------------------------------------
  local function resolve(p)
    if not p or p == "" then return cwd end
    if p:sub(1, 1) == "/" then return p end
    if p == "." then return cwd end

    if p == ".." then
      return cwd:match("^(.*)/[^/]+$") or "/"
    end

    return (cwd == "/" and "/" or cwd .. "/") .. p
  end

  if name == "cd" then
    local target = resolve(rest:match("^%s*(%S*)"))

    -- Asked rather than assumed: a path is a directory exactly when
    -- whoever serves it will list it, which is the only definition that
    -- means anything across four different servers.
    local entries, why = fs.list(target)

    if not entries then
      emit("cd: " .. target .. ": " .. tostring(why) .. "\n")
    else
      cwd = target
      emit(cwd .. "\n")
    end

    return
  end

  if name == "pwd" then
    emit(cwd .. "\n")
    return
  end

  if name == "clear" then
    lines = { "" }
    return
  end

  if name == "help" then
    local names = fs.list("/bin") or {}
    local out = {}

    for _, f in ipairs(names) do
      out[#out + 1] = f:gsub("%.lua$", "")
    end

    emit(table.concat(out, "  ") .. "\n")
    return
  end

  local path = name:sub(1, 1) == "/" and name or ("/bin/" .. name .. ".lua")

  if not fs.getattr(path) then
    emit(name .. ": no such program\n")
    return
  end

  --
  -- Handed this window as its console. `run` does not return until the
  -- child is finished, which would freeze this window - so it is started
  -- detached and `busy` says so, and the child's output arrives as `write`
  -- messages while it runs.
  --
  local ok, why = run(path, rest, true, { ["/dev/console"] = ep }, cwd)

  if ok then
    busy = name
  else
    emit(name .. ": " .. tostring(why) .. "\n")
  end
end

--------------------------------------------------------------------------
-- The console protocol, as far as a program can tell.
--------------------------------------------------------------------------

local function serve_console()
  local changed = false

  while true do
    local req, who = sys.receive(ep, true)

    if not req then return changed end

    local reply

    if req.type == "write" then
      emit(req.value)
      changed = true
      reply = { ok = true }

    elseif req.type == "read" then
      -- A program asking this window for a line. Not supported yet, and
      -- said rather than hung: a child blocked for ever on a reply nobody
      -- is going to send is the worst failure shape there is.
      reply = { ok = false, error = "this terminal cannot be read from yet" }

    elseif req.type == "poll" or req.type == "keys" then
      reply = { ok = true, value = (req.type == "poll") and false or {} }

    else
      reply = { ok = false, error = "no such operation: " .. tostring(req.type) }
    end

    pcall(sys.reply, who, reply)
  end
end

--------------------------------------------------------------------------

--
-- This window answers its children, so it cannot sleep a second between
-- passes: a program's `write` blocks until this loop gets to it, and `ls`
-- came out one line a second because of exactly that.
--
-- One scheduler tick. Input is still interrupt-driven and arrives sooner
-- than that; this is only the ceiling on how long a program waits to be
-- answered.
--
win.poll_wait = 1

win:add(view)

function win:on_key(c)
  --
  -- Typing is never blocked, even while something is running.
  --
  -- It was, on the reasoning that the child owns the console - and the
  -- effect was that one program which had not been noticed as finished
  -- locked the window for ever. A terminal that can stop accepting input is
  -- worse than one whose output interleaves, and interleaving is what every
  -- terminal does until it has job control.
  --
  if c == 10 or c == 13 then
    emit("> " .. input .. "\n")
    local text = input
    input = ""
    launch(text)
    return true
  end

  if c == 8 or c == 127 then
    input = input:sub(1, #input - 1)
    return true
  end

  if c >= 32 and c < 127 then
    input = input .. string.char(c)
    return true
  end

  return false
end

--
-- The child's output, and noticing when it has finished.
--
local pump = ui.view{ x = 0, y = 0, w = 0, h = 0 }

--
-- Noticing that the child has finished.
--
-- Every pass rather than on the tick, and in `on_frame` rather than here,
-- because a second of "..." after a program that printed one line and left
-- is a second of looking like something is wrong.
--
function pump:tick()
end

win:add(pump)

-- Serving the console cannot wait for the tick: a program that prints a
-- screenful would arrive one line a second. It happens every pass, which is
-- what `on_frame` is for.
function win:on_frame()
  local changed = serve_console()

  --
  -- Collecting the child, which also clears the "..." line.
  --
  -- This process is its parent: `run` spawns it from here, so its exit code
  -- comes back here. Non-blocking, so a terminal with nothing running does
  -- not stop.
  --
  if busy and sys.wait(true) then
    busy = nil
    changed = true
  end

  return changed
end

win:run()
