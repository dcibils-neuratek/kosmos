-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- Starts and stops the web server, and shows what it has been asked for.
--
--   wm webserver
--
-- **A manager rather than the server.** `httpd` is a process of its own and
-- this starts one, ends one, and reads what it wrote down - which is the
-- arrangement every service manager has with every service, and it is forced
-- rather than chosen: `accept` blocks, and a window that blocked would stop
-- drawing. Two processes is what lets one of them wait and the other stay
-- alive.
--
-- So the two of them talk through `/data`. `httpd` writes its state and its
-- last forty lines there; this reads them on a tick. That is a file rather
-- than a message because the server has no idea anybody is watching, and
-- should not have to.

local ui = use("/lib/ui.lua")
local theme = ui.theme

local W, H = 560, 460
local BAR_H = gfx.font.h + 8

local STATUS = "/data/httpd/status"
local LOG    = "/data/httpd/log"

local win, err = ui.window{ title = "Web Server", w = W, h = H, x = 120, y = 80 }

if not win then
  print("webserver: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- Is it running, and which process is it?
--
-- **Asked of the process table rather than of the status file.** A file says
-- what the server last wrote, which is what it believed when it was alive -
-- a server that was ended writes nothing on the way out if it was ended
-- rather than asked. So "running" is a process called `httpd` existing, and
-- the file is for everything else.
--------------------------------------------------------------------------

local function running()
  for _, p in ipairs(sys.processes() or {}) do
    if p.name == "httpd" then return p.id end
  end

  return nil
end

local said = ui.label{ x = 12, y = H - 30, w = W - 24, text = "",
                       follow = { "left", "right", "bottom" } }

local port_field = ui.field{ x = 90, y = 14 + BAR_H, w = 70, text = "80" }
local root_field = ui.field{ x = 250, y = 14 + BAR_H, w = 200,
                             text = "/home/www" }

win:add(ui.label{ x = 12, y = 18 + BAR_H, w = 80, text = "port",
                  color = "text_dim" })
win:add(port_field)
win:add(ui.label{ x = 172, y = 18 + BAR_H, w = 80, text = "directory",
                  color = "text_dim" })
win:add(root_field)

--------------------------------------------------------------------------
-- What it is doing.
--------------------------------------------------------------------------

-- Seventy-six, which is three lines of text and the padding round them.
-- Fifty-eight was two lines' worth and the third - the process id - was
-- drawn underneath the label of the list below it.
local state = ui.view{ x = 12, y = 48 + BAR_H, w = W - 24, h = 76,
                       follow = { "left", "right", "top" } }

function state:draw(g)
  local now = fs.read(STATUS)
  local id = running()

  g:sunken(0, 0, self.w, self.h, "sunken")

  --
  -- A dot, because the one thing somebody wants from this window is
  -- answerable at a glance. Green for serving, grey for stopped - and the
  -- text says the same thing, because a colour on its own is a statement
  -- nobody who cannot see it can read.
  --
  g:fill(8, 8, 10, 10, id and theme.good or theme.line)

  if id then
    g:text(26, 6, ("serving on port %s"):format(
             (type(now) == "table" and now.port) or "?"),
           theme.text, theme.sunken)

    if type(now) == "table" then
      g:text(26, 6 + gfx.font.h + 4,
             ("%s   at %s   %d served, %d refused")
             :format(now.root or "?", now.address or "?",
                     now.served or 0, now.refused or 0),
             theme.text_dim, theme.sunken)
    end

    g:text(26, 6 + (gfx.font.h + 4) * 2, ("process %d"):format(id),
           theme.line, theme.sunken)
  else
    g:text(26, 6, "stopped", theme.text, theme.sunken)

    if type(now) == "table" and (now.served or 0) > 0 then
      -- What the last run did, which is worth keeping on screen: a server
      -- that answered nothing and one that answered a thousand requests
      -- before it stopped are different situations.
      g:text(26, 6 + gfx.font.h + 4,
             ("last run served %d, refused %d")
             :format(now.served or 0, now.refused or 0),
             theme.text_dim, theme.sunken)
    end
  end
end

win:add(state)

--------------------------------------------------------------------------
-- The log.
--------------------------------------------------------------------------

local lines = ui.list{ x = 12, y = 134 + BAR_H, w = W - 24,
                       h = H - 208 - BAR_H, items = { "nothing yet" },
                       follow = { "left", "right", "top", "bottom" } }

-- Nothing selected, because a log is read and not chosen from. A list that
-- highlights its first row is offering an action there is none of.
lines.selected = 0

win:add(ui.label{ x = 12, y = 120 + BAR_H, w = 200, text = "Requests",
                  color = "text_dim" })
win:add(lines)

--------------------------------------------------------------------------
-- Starting and stopping.
--------------------------------------------------------------------------

local function start()
  if running() then
    said.text = "it is already running"
    return
  end

  local port = tonumber(port_field.text)
  local root = root_field.text:match("^%s*(.-)%s*$")

  if not port or port < 1 or port > 65535 then
    said.text = "that is not a port"
    return
  end

  if root == "" then
    said.text = "which directory?"
    return
  end

  --
  -- Checked here rather than left to the server.
  --
  -- `httpd` would answer 404 for everything and say nothing about why, and
  -- a person would reasonably conclude the network was broken. The directory
  -- being absent is the likeliest mistake and the cheapest to catch.
  --
  local attrs = fs.getattr(root)

  if not attrs then
    said.text = root .. " is not there"
    return
  end

  if attrs.kind ~= "directory" then
    said.text = root .. " is not a directory"
    return
  end

  --
  -- Through the desktop, which is what launches an application here - and
  -- which is also what can end one, since it is then the parent. Doing it
  -- with `run` would make this process the parent and this window the thing
  -- the server dies with.
  --
  local ok, why = fs.send("/app/wm", { type = "launch", program = "httpd",
                                       args = port .. " " .. root })

  said.text = ok and ("started on port " .. port)
              or ("could not start it: " .. tostring(why))
end

local function stop()
  local id = running()

  if not id then
    said.text = "it is not running"
    return
  end

  -- The desktop started it, so the desktop is its parent and the only thing
  -- that may end it. `procs` does exactly this and for the same reason.
  local ok, why = fs.send("/app/wm", { type = "end_process", pid = id })

  said.text = ok and "stopped" or ("could not stop it: " .. tostring(why))
end

local row = H - 60

win:add(ui.button{ x = 12, y = row, w = 70, h = 24, text = "Start",
                   follow = { "left", "bottom" }, on_click = start })
win:add(ui.button{ x = 90, y = row, w = 70, h = 24, text = "Stop",
                   follow = { "left", "bottom" }, on_click = stop })

win:add(ui.button{
  x = 168, y = row, w = 90, h = 24, text = "Clear log",
  follow = { "left", "bottom" },
  on_click = function()
    fs.write(LOG, {})
    said.text = "log cleared"
  end,
})

win:add(said)

--
-- Re-read on a tick rather than on a change.
--
-- `/data` can be *watched* - `fs.watch` blocks until a query's answer
-- changes, which is what M7 built - and this does not use it, because this
-- window is already blocked in the desktop's poll and there is no way to
-- wait on two things at once. **The fourth time that missing `select` has
-- come up**, after live queries, the stack's own loop, and telnet.
--
-- Twice a second, which is faster than anybody reads a log and slow enough
-- to cost nothing.
--
local last = 0

function win:on_frame()
  local now = sys.ticks()
  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

  self.poll_wait = 125

  if now - last < hz // 2 then return false end

  last = now

  local text = fs.read(LOG)

  if type(text) == "table" and #text > 0 then
    lines.items = text

    -- Following the end, because a log is read from the bottom and a window
    -- that stayed at the top would show the first forty requests for ever.
    if lines.top < #text - 1 then lines.top = math.max(1, #text - 10) end

    lines.selected = 0
  end

  return true
end

win:run()
