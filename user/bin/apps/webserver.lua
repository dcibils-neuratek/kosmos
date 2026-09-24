-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Poorman
-- kosmos: section system
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
-- So the two of them talk through `/ramfs`. `httpd` writes its state and its
-- last forty lines there; this reads them on a tick. That is a file rather
-- than a message because the server has no idea anybody is watching, and
-- should not have to.

local ui = use("/lib/ui.lua")
local theme = ui.theme

local W, H = 560, 460
local L = nil                  -- the kit's layout, once it is loaded

local STATUS = "/ramfs/httpd/status"
local LOG    = "/ramfs/httpd/log"

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

L = ui.layout

--------------------------------------------------------------------------
-- The window, as `docs/apps.html` draws it (`roadmap.md` 5zp).
--
-- **The state is the header's.** It was a box of its own under the fields,
-- three lines in a sunken well, and the one thing somebody opens this
-- window to learn - is it serving - was the third thing down. Now it is
-- the words beside the title, and the verb that changes it - Start, or Stop
-- while it runs - is the button at the header's end. Clearing the log is
-- behind the dots, because nobody needs it twice in a row.
--
-- **The fields are a card and the log is under it**, both at the drawings'
-- margins: the kit's `ui.cards` and `ui.layout`, rather than the labels at
-- x = 12 and 172 this placed by hand - which is the "no margin or spacing"
-- Diego saw.
--------------------------------------------------------------------------

local port_field = ui.field{ w = 90, text = "80" }
local root_field = ui.field{ w = 220, text = "/home/www" }

local start, stop                -- the verbs, below
local said = ""                  -- the last thing a verb said, for a moment

local start_button = ui.button{ text = "Start", go = true,
                                on_click = function() start() end }
local stop_button = ui.button{ text = "Stop", hidden = true,
                               on_click = function() stop() end }

local more = ui.iconbutton{ icon = "more" }

local header = ui.header{ x = 0, y = 0, w = W, title = "Web server",
                          sub = "stopped",
                          right = { start_button, stop_button, more } }

local cards = ui.cards{
  x = 0, y = L.head, w = W, h = 1,
  follow = { "left", "right", "top" },
  groups = {
    { name = "Serving", rows = {
        { label = "Port", control = port_field },
        { label = "Directory", control = root_field } } },
  },
}

cards.h = cards.content_h

--
-- What it is doing, in the header: serving and how much, or stopped and
-- what the last run did - a server that answered nothing and one that
-- answered a thousand before it stopped are different situations.
--
local function state_text(id, now)
  now = type(now) == "table" and now or {}

  if id then
    return ("serving on port %s · %d served, %d refused · process %d")
           :format(now.port or "?", now.served or 0, now.refused or 0, id)
  end

  if (now.served or 0) > 0 then
    return ("stopped · the last run served %d, refused %d")
           :format(now.served or 0, now.refused or 0)
  end

  return "stopped"
end

--------------------------------------------------------------------------
-- The log, under the card at the drawings' spacing: a group's name, then
-- the list 26 below it, to the page's bottom margin.
--------------------------------------------------------------------------

local log_y = L.head + cards.content_h + L.between

local requests = ui.label{ x = L.page_side + 3,
                           y = log_y + (L.group - gfx.height("heading")) // 2,
                           w = 200, text = "Requests", role = "heading" }

local lines = ui.list{ x = L.page_side, y = log_y + L.to_card,
                       w = W - 2 * L.page_side,
                       h = H - (log_y + L.to_card) - L.page_foot,
                       items = { "nothing yet" },
                       follow = { "left", "right", "top", "bottom" } }

-- Nothing selected, because a log is read and not chosen from. A list that
-- highlights its first row is offering an action there is none of.
lines.selected = 0

win:add(header)
win:add(cards)
win:add(requests)
win:add(lines)

function start()
  if running() then
    said = "it is already running"
    return
  end

  local port = tonumber(port_field.text)
  local root = root_field.text:match("^%s*(.-)%s*$")

  if not port or port < 1 or port > 65535 then
    said = "that is not a port"
    return
  end

  if root == "" then
    said = "which directory?"
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
    said = root .. " is not there"
    return
  end

  if attrs.kind ~= "directory" then
    said = root .. " is not a directory"
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

  said = ok and ("started on port " .. port)
              or ("could not start it: " .. tostring(why))
end

function stop()
  local id = running()

  if not id then
    said = "it is not running"
    return
  end

  -- The desktop started it, so the desktop is its parent and the only thing
  -- that may end it. `procs` does exactly this and for the same reason.
  local ok, why = fs.send("/app/wm", { type = "end_process", pid = id })

  said = ok and "" or ("could not stop it: " .. tostring(why))
end

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "Clear log", on_choose = function()
        fs.write(LOG, {})
        lines.items = { "nothing yet" }
      end },
  })
end

--
-- Re-read on a tick rather than on a change.
--
-- `/ramfs` can be *watched* - `fs.watch` blocks until a query's answer
-- changes, which is what M7 built - and this does not use it, because this
-- window is already blocked in the desktop's poll and there is no way to
-- wait on two things at once. **The fourth time that missing `select` has
-- come up**, after live queries, the stack's own loop, and telnet.
--
-- Twice a second, which is faster than anybody reads a log and slow enough
-- to cost nothing.
--
local last = 0
local shown_state = nil

function win:on_frame()
  local now = sys.ticks()
  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

  self.poll_wait_ticks = 125

  if now - last < hz // 2 then return false end

  last = now

  --
  -- The header says what the server is doing, or for a moment what a verb
  -- just said - until the state itself changes, which is the answer to it.
  --
  local id = running()
  local state = state_text(id, fs.read(STATUS))

  if state ~= shown_state then
    if shown_state then said = "" end
    shown_state = state
  end

  header.sub = (said ~= "") and said or state
  start_button.hidden = id ~= nil
  stop_button.hidden = id == nil

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
