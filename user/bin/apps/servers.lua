-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Poorman
-- kosmos: section system
-- kosmos: needs processes
-- Every network server this machine can run, in one window.
--
--   wm servers
--   wm servers telnet        open on that server's page
--
-- **As `docs/servers.html` draws it**, and Diego's word on the drawing:
-- "The mockup looks great". His ask, 29 September: "a servers app that hold
-- all servers like web, telnet, vnc, etc like the preferences app but with
-- servers configuration so we can config and activate/deactivate network
-- servers" (`roadmap.md`, remote). Preferences' shape - a list on the left,
-- a page of cards on the right - with a server where it has a part.
--
-- **A manager rather than the servers**, as the Web Server window was, and
-- this takes its place. A server is a program of its own - `httpd`,
-- `telnetd` - which writes its state and its last lines under
-- `/Temporary/<name>` and does not know a window is watching; this starts
-- one through the desktop, ends one through it, and reads what it wrote.
-- Running is a process of that name existing, never what a file last said:
-- a server that was ended rather than asked writes nothing on the way out.
--
-- **Its settings are `/Home/Preferences/servers`**, a table a server; the
-- shell starts what is marked to start with the machine, at boot. The
-- screen is `vncd` (`roadmap.md` remote 7a), which reads its password from
-- there at each connection; a viewer only looks until keys and the pointer
-- are built (7b), so that row says so rather than offering a choice.

local ui = use("/Kosmos/Libraries/ui.lua")
local theme = ui.theme
local L = ui.layout

local W, H = 760, 600
local SIDE = 216
local SETTINGS = "/Home/Preferences/servers"

local win, err = ui.window{ title = "Servers", w = W, h = H, x = 140, y = 90,
                            header = true }

if not win then
  print("servers: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The servers, and what each needs to be started.
--------------------------------------------------------------------------

local SERVERS = {
  { id = "web", name = "Web", icon = "network", program = "httpd",
    what = "HTTP", status = "/Temporary/httpd/status", log = "/Temporary/httpd/log",
    defaults = { port = 80, folder = "/Home/www", at_start = false },
    args = function(c) return ("%d %s"):format(c.port, c.folder) end,
    reach = function(a, c) return ("http://%s%s"):format(a, c.port == 80 and "" or (":" .. c.port)) end },
  { id = "telnet", name = "Command line", icon = "run", program = "telnetd",
    what = "Telnet", status = "/Temporary/telnetd/status", log = "/Temporary/telnetd/log",
    defaults = { port = 23, at_start = false },
    args = function(c) return tostring(c.port) end,
    reach = function(a, c) return ("telnet %s%s"):format(a, c.port == 23 and "" or (" " .. c.port)) end },
  { id = "vnc", name = "Screen", icon = "display", program = "vncd",
    what = "VNC", status = "/Temporary/vncd/status", log = "/Temporary/vncd/log",
    defaults = { port = 5900, at_start = false, password = "" },
    args = function(c) return tostring(c.port) end,
    reach = function(a, c) return ("vnc://%s%s"):format(a, c.port == 5900 and "" or (":" .. c.port)) end },
}

local BY_ID = {}

for _, s in ipairs(SERVERS) do BY_ID[s.id] = s end

-- What is kept: each server's table, with its defaults under what is missing.
local saved = fs.read(SETTINGS)

saved = type(saved) == "table" and saved or {}

local function config(s)
  local c = type(saved[s.id]) == "table" and saved[s.id] or {}

  for k, v in pairs(s.defaults) do
    if c[k] == nil then c[k] = v end
  end

  saved[s.id] = c
  return c
end

local function keep()
  fs.send("/Home/Preferences", { type = "mkdir" })
  fs.write(SETTINGS, saved)
end

--------------------------------------------------------------------------
-- Is it running, and where is this machine?
--------------------------------------------------------------------------

local function running(s)
  if not s.program then return nil end

  for _, p in ipairs(sys.processes() or {}) do
    if p.name == s.program and not p.exited then return p.id end
  end

  return nil
end

local function address()
  local net = fs.net_info("/Network")
  local a = net and net.address

  if type(a) == "string" and #a == 4 and a ~= "\0\0\0\0" then
    return ("%d.%d.%d.%d"):format(a:byte(1, 4)), net
  end

  return nil, net
end

local said = ""                     -- the last thing a verb said, for a moment

local function start(s)
  local c = config(s)

  if not s.program then
    said = s.name .. " is drawn and not built yet"
    return
  end

  if running(s) then return end

  if s.id == "web" then
    local attrs = fs.getattr(c.folder)

    if not attrs or attrs.kind ~= "directory" then
      said = c.folder .. " is not a folder here"
      return
    end
  end

  -- Through the desktop, as the Deskbar starts anything: then it is the
  -- desktop's child, and this window closing does not end it.
  local ok, why = fs.send("/Running/wm", { type = "launch", program = s.program,
                                           args = s.args(c) })

  said = ok and "" or ("could not start " .. s.name .. ": " .. tostring(why))
end

local function stop(s)
  local id = running(s)

  if not id then return end

  local ok, why = fs.send("/Running/wm", { type = "end_process", pid = id })

  said = ok and "" or ("could not stop " .. s.name .. ": " .. tostring(why))
end

--------------------------------------------------------------------------
-- The window: the list on the left, a page of cards on the right.
--------------------------------------------------------------------------

local function wanted()
  local a = tostring(args or ""):match("^%s*(%S*)")

  return BY_ID[a] and a or "all"
end

local showing = wanted()

local header = ui.header{ x = SIDE, y = 0, w = W - SIDE, title = "",
                          title_bar = true }

local cards = ui.cards{ x = SIDE, y = L.head, w = W - SIDE, h = H - L.head,
                        width = 500 }

local log = ui.list{ x = SIDE + L.page_side, y = H - 150, w = W - SIDE - 2 * L.page_side,
                     h = 150 - L.page_foot, items = {}, hidden = true,
                     follow = { "left", "right", "bottom" } }

log.selected = 0

local items = { { id = "all", name = "All servers", icon = "system" }, { gap = true } }

for _, s in ipairs(SERVERS) do
  items[#items + 1] = { id = s.id, name = s.name, icon = s.icon }
end

local rebuild                       -- forward: the list calls it

local side = ui.sidebar{ x = 0, y = L.head + 2, w = SIDE - 1, h = H - L.head - 2,
                         items = items, selected = showing,
                         on_select = function(_, id)
                           showing = id
                           rebuild()
                         end }

local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = H }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
  g:text(L.head_in, (L.head - 1 - gfx.height("title")) // 2, "Servers",
         theme.text, nil, "title")
end

win:add(side_ground)
win:add(side)
win:add(cards)
win:add(log)
win:add(header)

--------------------------------------------------------------------------
-- The pages.
--------------------------------------------------------------------------

-- The switch for a server, which starts and ends it.
local switches = {}

local function run_switch(s)
  local sw = ui.switch{ on = running(s) ~= nil,
                        on_change = function(_, on)
                          if on then start(s) else stop(s) end
                        end }

  switches[s.id] = sw
  return sw
end

local function start_switch(s)
  local c = config(s)

  return ui.switch{ on = c.at_start == true,
                    on_change = function(_, on)
                      c.at_start = on
                      keep()
                    end }
end

-- A field that keeps a number or a folder when it is changed.
local function field(s, key, w, number)
  local c = config(s)

  return ui.field{ w = w, text = tostring(c[key]),
                   on_change = function(self, text)
                     local v = number and tonumber(text) or text:match("^%s*(.-)%s*$")

                     if v and v ~= "" and (not number or (v >= 1 and v <= 65535)) then
                       c[key] = v
                       keep()
                     end
                   end }
end

local function state_of(s)
  local id = running(s)
  local status = s.status and fs.read(s.status)

  status = type(status) == "table" and status or {}

  if not s.program then return "not built yet", status end
  if not id then return "stopped", status end

  if s.id == "web" then
    return ("running · %d served, %d refused"):format(status.served or 0,
                                                     status.refused or 0), status
  end

  if s.id == "telnet" then
    local n = #(status.sessions or {})

    return ("running · %d session%s"):format(n, n == 1 and "" or "s"), status
  end

  if s.id == "vnc" then
    local n = #(status.viewers or {})

    return ("running · %d viewer%s"):format(n, n == 1 and "" or "s"), status
  end

  return "running", status
end

local function page_all()
  local a, net = address()
  local rows = {}

  for _, s in ipairs(SERVERS) do
    local state = state_of(s)

    -- A server not built has no switch: a control that moves and does
    -- nothing is the thing this window must not have.
    rows[#rows + 1] = { label = s.name,
                        control = s.program and run_switch(s) or nil,
                        value = (not s.program) and "Not built yet" or nil,
                        note = s.what .. " · " .. state
                               .. ((a and s.program) and (" · " .. s.reach(a, config(s))) or "") }
  end

  return {
    { name = "Servers", rows = rows },
    { name = "This machine", rows = {
        { label = "Address", value = a or "none yet",
          note = (net and net.addressed_by == "dhcp") and "From the router, by DHCP"
                 or (net and net.addressed_by == "asking") and "Asking the router"
                 or "Set by hand" },
        { label = "Who may connect", value = "This network only",
          note = "Others are turned away" } } },
  }
end

local function page_web(s)
  return {
    { name = "", rows = {
        { label = "Running", note = state_of(s), control = run_switch(s) } } },
    { name = "Set up", rows = {
        { label = "Port", control = field(s, "port", 80, true) },
        { label = "Folder", note = "What is served; nothing outside it can be asked for",
          control = field(s, "folder", 200, false) },
        { label = "Start with the machine", control = start_switch(s) } } },
  }
end

local function page_telnet(s)
  local _, status = state_of(s)
  local groups = {
    { name = "", rows = {
        { label = "Running", note = state_of(s), control = run_switch(s) } } },
    { name = "Set up", rows = {
        { label = "Port", control = field(s, "port", 80, true) },
        { label = "No password",
          note = "Anyone on this network can run any program; nothing on the wire is secret" },
        { label = "Start with the machine",
          note = "A development stick starts it whatever this says",
          control = start_switch(s) } } },
  }

  local sessions = {}

  for _, who in ipairs(status.sessions or {}) do
    local from = tostring(who.from)

    sessions[#sessions + 1] = {
      label = from,
      note = ("in %s%s"):format(tostring(who.cwd or "/Home"),
                                who.running and (" · running " .. who.running) or ""),
      control = ui.button{ text = "Disconnect", on_click = function()
        fs.send("/Running/telnetd", { type = "disconnect", from = from })
      end } }
  end

  if #sessions == 0 then
    sessions[1] = { label = "Nobody", note = "A session is listed here, with a Disconnect" }
  end

  groups[#groups + 1] = { name = "Sessions", rows = sessions }
  return groups
end

-- The screen's password: kept as typed, emptied as well - an empty one is
-- the choice to have none - and eight characters at most, which is all
-- VNC's own authentication reads.
local function password_field(s)
  local c = config(s)

  return ui.field{ w = 120, text = tostring(c.password or ""), secret = true,
                   hint = "none",
                   on_change = function(_, text)
                     c.password = text:sub(1, 8)
                     keep()
                   end }
end

local function page_vnc(s)
  local _, status = state_of(s)
  local groups = {
    { name = "", rows = {
        { label = "Running", note = state_of(s), control = run_switch(s) } } },
    { name = "Set up", rows = {
        { label = "Port", control = field(s, "port", 80, true) },
        { label = "A viewer may", value = "Only look",
          note = "Using the keyboard and the pointer comes next" },
        { label = "Password",
          note = "Eight characters at most; none lets this network look",
          control = password_field(s) },
        { label = "Start with the machine",
          note = "Off: a screen shared by itself is easy to forget",
          control = start_switch(s) } } },
  }

  local viewers = {}

  for _, who in ipairs(status.viewers or {}) do
    local from = tostring(who.from)

    viewers[#viewers + 1] = {
      label = from,
      note = ("%d bits a pixel, as it asked"):format(tonumber(who.bpp) or 32),
      control = ui.button{ text = "Disconnect", on_click = function()
        fs.send("/Running/vncd", { type = "disconnect", from = from })
      end } }
  end

  if #viewers == 0 then
    viewers[1] = { label = "Nobody", note = "A viewer is listed here, with a Disconnect" }
  end

  groups[#groups + 1] = { name = "Viewers", rows = viewers }
  return groups
end

local PAGES = { all = page_all, web = page_web, telnet = page_telnet, vnc = page_vnc }

local shown_cards, shown_log = nil, nil

-- The log of the page's server, when it keeps one; true when it changed.
local function refresh_log(s)
  if not (s and s.log) then return false end

  local lines = fs.read(s.log)

  lines = (type(lines) == "table" and #lines > 0) and lines or { "nothing yet" }

  local key = #lines .. "|" .. tostring(lines[#lines])

  if key == shown_log then return false end

  shown_log = key
  log.items = lines
  log.top = math.max(1, #lines - 8)
  log.selected = 0
  return true
end

rebuild = function()
  switches = {}

  local s = BY_ID[showing]

  header.title = s and s.name or "All servers"
  header.sub = said
  cards:set(PAGES[showing](s))

  -- The log, under the cards, for a server that keeps one.
  log.hidden = not (s and s.log)
  log.y = L.head + (cards.content_h or 0) + L.between
  log.h = math.max(40, H - log.y - L.page_foot)

  shown_log = nil
  refresh_log(s)
  shown_cards = nil
end

--------------------------------------------------------------------------
-- Twice a second: whether each runs, and what it wrote down. **The cards
-- are made again only when a server's state changed**, so a field being
-- typed in keeps its place when nothing but the log moved.
--------------------------------------------------------------------------

local last = 0

local function states()
  local out = {}

  for _, sv in ipairs(SERVERS) do
    out[#out + 1] = sv.id .. "=" .. state_of(sv)
  end

  return table.concat(out, "|")
end

function win:on_frame()
  local now = sys.ticks()
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

  self.poll_wait_ticks = 125

  if now - last < hz // 2 then return false end

  last = now

  local state = states() .. said
  local changed = false

  if state ~= shown_cards then
    rebuild()
    shown_cards = state
    print("servers: " .. states())
    changed = true
  end

  return refresh_log(BY_ID[showing]) or changed
end

rebuild()
win:run()
