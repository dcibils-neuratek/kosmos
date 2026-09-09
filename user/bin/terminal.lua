-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
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

--
-- Tall enough for the banner it opens with, and no taller.
--
-- 420 was the right height when the first thing in the window was one line
-- of greeting. `neofetch` runs here now, so a window that could not hold its
-- art would scroll the banner off while it was still being drawn - which
-- looks like a fault rather than a picture.
--
-- **Measured rather than counted.** 700 was arithmetic on a twenty-two-row
-- banner; the art is fourteen rows now, and a freshly opened window draws
-- its last row - the prompt - at y=578 against a console that ran to y=726.
-- A hundred and forty-eight pixels of black under the cursor is not a
-- margin, it is a window that was sized for a different picture.
--
-- 580 leaves the prompt with a row and a half beneath it, which is the same
-- slack the old number had. The banner is the only reason this is not the
-- 420 it was, so it moves whenever the banner does - it is a constant
-- precisely so that it can.
--
local W, H = 640, 580
local SCROLLBACK = 400          -- lines kept

local win, err = ui.window{ title = "Terminal", w = W, h = H, x = 90, y = 40 }

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

--------------------------------------------------------------------------
-- What is on screen, now that a write carries a colour.
--
-- **A line is a list of runs, not a string.** `con_request` has a colour on
-- it, so two writes to the same line can want two colours, and a string has
-- nowhere to keep the second one. A run is `{ text, colour }`, and a line
-- with one colour in it - which is almost every line - is one run, so the
-- ordinary case costs one table more than it did.
--
-- The colour is whatever the writer sent: a number, or nil for the console's
-- own. Nothing here maps names; `ns.write` did that before the message left
-- the program, because a name means nothing to the kernel's console and this
-- window has to behave like that one.
--
-- A trailing empty line, because `emit` appends to the last one. Without it
-- the first thing typed lands on the end of the banner.
--------------------------------------------------------------------------
local lines = {
  { { text = "Kosmos terminal. Type a program's name; `help` lists them." } },
  {},
}
local input = ""
local busy = nil                -- the child that currently owns this console

local function emit(text, colour)
  -- Whatever arrives, split on newlines and appended to the last line if it
  -- did not start with one. A `write` is a stream, not a line: `print` sends
  -- one ending in a newline and `write_text` splits long output into pieces
  -- that can end anywhere.
  for piece, newline in tostring(text):gmatch("([^\n]*)(\n?)") do
    if piece ~= "" then
      if #lines == 0 then lines[1] = {} end

      local line = lines[#lines]
      local last = line[#line]

      --
      -- Joined onto the run before it when the colour is the same, which is
      -- what keeps this from growing a run per message. `write_text` splits
      -- anything over 1400 bytes, and a program printing a screenful in one
      -- colour would otherwise arrive as a run for every piece of it.
      --
      if last and last.colour == colour then
        last.text = last.text .. piece
      else
        line[#line + 1] = { text = piece, colour = colour }
      end
    end

    if newline == "\n" then
      lines[#lines + 1] = {}
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

  --
  -- Laid out in *pixels* and not in columns, and that is the change UTF-8
  -- forces.
  --
  -- This drew `line:sub(1, columns)`, and `#text` in Lua is bytes: one block
  -- character is three of them, so a run holding any would be cut a third of
  -- the way along and every run after it would start in the wrong place.
  -- `gfx.measure` decodes UTF-8 and answers in pixels, which is the ruler
  -- that was always meant here.
  --
  -- Nothing is truncated any more either. A view is clipped to itself, so a
  -- line running past the right edge stops at the edge - which is what the
  -- `sub` was for and what the view was already doing underneath it.
  --
  for i, line in ipairs(shown) do
    local y = 3 + (i - 1) * MH
    local x = 4

    for _, run in ipairs(line) do
      if x >= self.w then break end

      -- `console_text`, not `text`: `text` is chosen to read against the
      -- window colour, and in a light theme that is black - which on a
      -- black console is nothing at all. A run with a colour of its own
      -- overrides it, and that is the only reason this is not a constant.
      g:text(x, y, run.text, run.colour or "console_text", "console", "mono")
      x = x + gfx.measure(run.text, "mono")
    end
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
    lines = { {} }
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
  -- The endpoint *and* what it speaks. This window is a console, and the
  -- child has to mount it as one: a bare capability would be mounted
  -- speaking Lua tables, and every `write` would arrive here as a
  -- serialised table where a `con_request` was expected.
  local ok, why = run(path, rest, true,
                      { ["/dev/console"] = { cap = ep, proto = "console" } },
                      cwd)

  if ok then
    busy = name
  else
    emit(name .. ": " .. tostring(why) .. "\n")
  end
end

--------------------------------------------------------------------------
-- The console protocol, as far as a program can tell.
--------------------------------------------------------------------------

--
-- This window is a console, and says so in the console's own words.
--
-- A terminal mounts itself as its child's `/dev/console`, so a program
-- running in one prints to an application and cannot tell - which is the
-- namespace working as intended, and which means this window implements a
-- system ABI. It used to answer with Lua tables while the real console
-- server answered with Lua tables, and the two agreed by both being written
-- in the same language rather than by agreeing about anything.
--
-- The server is C now and the protocol is `conproto.h`. Rather than copy a
-- format string in here, both sides go through the Console Kit, which
-- compiles that header once. `use("/kits/console")` is the same line
-- `use("/lib/ui.lua")` is; that the layout is defined in C is not something
-- this file has to know.
--
local con = use("/kits/console")

--------------------------------------------------------------------------
-- A burst of writes is one repaint, not one repaint each.
--
-- **This loop served exactly one message per pass, and that made a
-- screenful quadratic.** The drain was non-blocking, so after answering a
-- write it asked again immediately - before the child it had just woken
-- could possibly have been scheduled and sent its next one. Nothing was
-- there, the loop returned, the window repainted, and `poll` waited a tick.
-- One write, one full repaint, every time.
--
-- A repaint sends the *whole* window as drawing commands, in 1200-byte
-- batches at `96 + #text` bytes an op, so a window holding `n` runs costs
-- about `n / 11` round trips to draw. Answering `n` writes with `n`
-- repaints is therefore `n^2 / 11` of them: `neofetch` is 165 writes -
-- eight runs a line, because a line in several colours is several writes -
-- which measured **24.0 seconds** to put twenty-three lines on a screen,
-- with the processor idle throughout. None of that is work; it is waiting.
--
-- So: **wait briefly for the next message once one has arrived.** A child
-- mid-burst is a few microseconds from sending again, and `receive_raw`
-- takes a timeout. The banner is then absorbed in a pass or two and painted
-- once or twice, which is all a screen could have shown anyway. Measured
-- the same way afterwards: complete before the window is first visible.
--
-- The first receive of a pass stays non-blocking, so an idle terminal costs
-- exactly what it did. And the burst is bounded in *time* rather than in
-- messages: a window is also a window, and one that stopped answering the
-- desktop while a chatty program ran would have traded one complaint for a
-- worse one. One frame is the bound, because one frame is all it could have
-- displayed.
--------------------------------------------------------------------------
local BURST_WAIT = 1        -- scheduler ticks: how long to wait mid-burst

-- Counter units, and read rather than assumed: `sys.ticks()` is the counter
-- and the two clocks differ by a quarter of a million on this board. The
-- same read `tile` does, with the same fallback.
local BURST_SPAN = ((fs.read("/dev/cpu") or {}).counter_hz or 62500000) // 60

local function serve_console()
  local changed = false
  local until_ = nil

  while true do
    local bytes, who

    if until_ == nil then
      -- The first of a pass. Nothing waiting means nothing to do, and this
      -- must not be the thing that makes an idle window cost a tick.
      bytes, who = sys.receive_raw(ep, true)
    elseif sys.ticks() < until_ then
      bytes, who = sys.receive_raw(ep, false, BURST_WAIT)
    end

    if not bytes then return changed end

    -- Started on the first message rather than at the top, so the span
    -- measures the burst and not the pass.
    until_ = until_ or (sys.ticks() + BURST_SPAN)

    local req = con.decode_request(bytes)
    local reply

    if not req then
      reply = { error = con.ERR_BAD_OP }

    elseif req.op == con.WRITE then
      -- Zero is "no opinion", and it has to become nil rather than be
      -- passed on: `emit` compares colours to decide whether to join two
      -- runs, and a run drawn in colour 0 would be invisible.
      emit(req.text, (req.colour ~= 0) and req.colour or nil)
      changed = true
      reply = {}

    elseif req.op == con.READ then
      -- A program asking this window for a line. Not supported yet, and
      -- said rather than hung: a child blocked for ever on a reply nobody
      -- is going to send is the worst failure shape there is.
      --
      -- A number now, not a sentence. The words used to be invented here,
      -- by one of the two things that implement this protocol; whoever puts
      -- the failure in front of a person composes them.
      reply = { error = con.ERR_NO_READER }

    elseif req.op == con.POLL or req.op == con.KEYS then
      -- Nothing typed at this window reaches the program in it yet. `poll`
      -- answers "no Control-C" and `keys` answers "nothing", which are the
      -- same two answers as before and are now the protocol's own zeroes.
      reply = {}

    else
      reply = { error = con.ERR_BAD_OP }
    end

    pcall(sys.reply_raw, who, con.encode_reply(reply))
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
win.poll_wait_ticks = 1

win:add(view)

--------------------------------------------------------------------------
-- What was typed into this window before.
--
-- The same shape as the console server's, deliberately: 0 is the line being
-- typed, 1 is the most recent, anything that ends a line puts it back to 0.
-- Two implementations because there are two line editors - a Terminal does
-- its own, since the window is where the keys arrive - and the one thing
-- they must not do is disagree about what up-up-down means.
--
-- Consecutive duplicates are dropped: the arrow is for finding something,
-- and a history of `ls` eleven times is one you walk past rather than use.
--
-- Not shared with the console's, and could not be: that ring lives in
-- another process, and a window asking it for the lines somebody typed
-- somewhere else would be reaching for state it was never handed.
--------------------------------------------------------------------------
local HISTORY = 32

local past, recall = {}, 0

local function remember(text)
  if text == "" or past[#past] == text then return end

  past[#past + 1] = text

  if #past > HISTORY then table.remove(past, 1) end
end

local function walk(to)
  if to < 0 or to > #past then return end

  recall = to
  input = (to == 0) and "" or past[#past - to + 1]
end

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
    remember(text)
    recall = 0
    launch(text)
    return true
  end

  -- `ui.key_decoder` hands arrows back as negative codes, so an up-arrow is
  -- a key here rather than three bytes to reassemble. The console server
  -- does that reassembly because it is handed the bytes.
  if c == -1 then walk(recall + 1) return true end
  if c == -2 then walk(recall - 1) return true end

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

--------------------------------------------------------------------------
-- What machine this is, before the first prompt.
--
-- The same program the boot shell runs, started the same way anything
-- typed into this window is started - so a window opened here and the
-- console the machine came up on say the same thing, and neither of them
-- has a copy of how to say it.
--
-- It goes through `launch` rather than being echoed as a typed line: this
-- window did not type it, and printing `> neofetch` above the output would
-- be the window claiming somebody did. `busy` shows "running neofetch" in
-- the corner for the pass it takes, which is true.
--
-- Nothing waits for it. `launch` detaches, the output arrives as `write`
-- messages that `on_frame` is already serving, and a Terminal whose banner
-- failed is a Terminal with a prompt in it.
--------------------------------------------------------------------------
launch("neofetch")

win:run()
