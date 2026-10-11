-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Poorman
-- kosmos: name Servers
-- kosmos: section system
-- kosmos: needs processes sharing
-- Every network server this machine can run, in one window.
--
--   wm servers
--   wm servers:telnet        open on that server's page
--   wm servers:sharing       File sharing: folders of /Home over SMB
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
-- screen is `vncd` (`roadmap.md` remote 7), which reads its password and
-- whether a viewer may use the keyboard and the pointer from there at each
-- connection - and which the window manager starts, not the shell, since
-- it is what lends it the desktop.

local ui = use("/Kosmos/Libraries/ui.lua")
local theme = ui.theme
local L = ui.layout

local W, H = 760, 600
local SIDE = 216
local prefs = use("/Kosmos/Libraries/prefs.lua")
local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
local files = use("/Kosmos/Libraries/files.lua")

local win, err = ui.window{ title = "Servers", w = W, h = H, x = 140, y = 90,
                            header = true }

if not win then
  print("servers: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The servers, and what each needs to be started.
--------------------------------------------------------------------------

--
-- **File sharing** (`docs/sharing.md` N12c, `docs/sharing.html`'s fifth
-- state): folders of `/Home` offered to the Mac's Finder and Windows over
-- SMB. Not a program this window starts: `smbd` is a server init starts at
-- every boot, idle, and this tells it what to share through its door
-- (`smbdproto.h`), which the namespace lends this window because the image
-- serves it and it says `kosmos: needs sharing`. So it is `door`, not
-- `program`, and running is what `smbd` says rather than a process existing.
--

local SERVERS = {
  { id = "web", name = "Web", icon = "network", program = "httpd",
    what = "HTTP", status = "/Temporary/httpd/status", log = "/Temporary/httpd/log",
    defaults = { port = 80, folder = "/Home/www", at_start = false },
    args = function(c) return ("%d %s"):format(c.port, files.quote(c.folder)) end,
    reach = function(a, c) return ("http://%s%s"):format(a, c.port == 80 and "" or (":" .. c.port)) end },
  { id = "telnet", name = "Command line", icon = "run", program = "telnetd",
    what = "Telnet", status = "/Temporary/telnetd/status", log = "/Temporary/telnetd/log",
    defaults = { port = 23, at_start = false },
    args = function(c) return tostring(c.port) end,
    reach = function(a, c) return ("telnet %s%s"):format(a, c.port == 23 and "" or (" " .. c.port)) end },
  { id = "vnc", name = "Screen", icon = "display", program = "vncd",
    what = "VNC", status = "/Temporary/vncd/status", log = "/Temporary/vncd/log",
    defaults = { port = 5900, at_start = false, password = "", control = false },
    args = function(c) return tostring(c.port) end,
    reach = function(a, c) return ("vnc://%s%s"):format(a, c.port == 5900 and "" or (":" .. c.port)) end },
  { id = "sharing", name = "File sharing", icon = "folder", door = true,
    what = "SMB",
    defaults = { port = 445, at_start = false, account = "", hostname = "KOSMOS",
                 shares = {}, local_only = true },
    reach = function(a) return ("smb://%s"):format(a) end },
}

local BY_ID = {}

for _, s in ipairs(SERVERS) do BY_ID[s.id] = s end

-- What is kept: each server's table, with its defaults under what is missing.
local saved = prefs.read("servers")

local function config(s)
  local c = type(saved[s.id]) == "table" and saved[s.id] or {}

  for k, v in pairs(s.defaults) do
    if c[k] == nil then c[k] = v end
  end

  saved[s.id] = c
  return c
end

local function keep()
  prefs.write("servers", saved)
end

--------------------------------------------------------------------------
-- Is it running, and where is this machine?
--------------------------------------------------------------------------

local netprogram = use("/Kosmos/Libraries/netprogram.lua")

-- What `smbd` last said, asked once a pass: its door answers at once.
local smbd_state = nil

local function sharing_state(fresh)
  if fresh or not smbd_state then
    smbd_state = fs.has_sharing() and fs.sharing_state() or nil
  end

  return smbd_state
end

local function running(s)
  if s.door then
    local st = sharing_state()
    return (st and st.state ~= "stopped") and true or nil
  end

  return s.program and netprogram.running(s.program) or nil
end

local function address()
  local net = fs.net_info("/Network")
  local a = net and net.address

  if ipv4.given(a) then return ipv4.text(a), net end

  return nil, net
end

local said = ""                     -- the last thing a verb said, for a moment

-- File sharing, started: the folders and then the account, as `smbd`
-- wants them - each refusal said as it comes.
local function start_sharing(c)
  if not fs.has_sharing() then
    said = "this window was not handed smbd's door"
    return
  end

  if #c.shares == 0 then
    said = "share a folder of /Home first"
    return
  end

  if c.account == "" then
    said = "name the account a visitor signs in with first"
    return
  end

  local ok, why = fs.sharing_kept(c.account)

  if not ok then
    said = "give " .. c.account .. " a password first"
    return
  end

  ok, why = fs.sharing_shares(c.shares)

  if ok then
    ok, why = fs.sharing_start{ account = c.account, hostname = c.hostname, port = c.port,
                                local_only = c.local_only }
  end

  said = ok and "" or ("could not share: " .. tostring(why))
  sharing_state(true)
end

local function start(s)
  local c = config(s)

  if s.door then
    start_sharing(c)
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
  if s.door then
    local ok, why = fs.sharing_stop()

    said = ok and "" or ("could not stop " .. s.name .. ": " .. tostring(why))
    sharing_state(true)
    return
  end

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

  if s.door then
    local st = sharing_state()

    if not st then return "not handed smbd's door", status end
    if st.state == "stopped" then return "stopped", status end
    if st.state == "waiting" then
      return "waiting for the network to let it listen", status
    end

    return ("sharing %d folder%s · %d connected"):format(st.shares, st.shares == 1 and "" or "s",
                                                       st.connections), status
  end

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

    -- A server not built has its switch drawn disabled, saying so: a
    -- control that moves and does nothing is the thing this window must not
    -- have, and a disabled one does not move.
    rows[#rows + 1] = { label = s.name,
                        control = (s.program or s.door) and run_switch(s)
                                  or ui.switch{ disabled = true },
                        note = s.what .. " · " .. state
                               .. ((a and (s.program or s.door)) and (" · " .. s.reach(a, config(s))) or "") }
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
        { label = "A viewer may use it",
          note = "The keyboard and the pointer; off, a viewer only looks",
          control = ui.switch{ on = config(s).control == true,
                               on_change = function(_, on)
                                 config(s).control = on
                                 keep()
                               end } },
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

--
-- **File sharing, as drawn** (`docs/sharing.html`): on or off, the address,
-- which folders of `/Home`, who may sign in, and how it is set up. Read and
-- write is drawn and kept off until smbd writes (N14); encryption until
-- libsmb2's server can ask a client to seal; This network only and who is
-- connected come with N12c-3.
--
local panel = use("/Kosmos/Libraries/panel.lua")
local sharing_switch, share_button, password_button, account_field = nil, nil, nil, nil
local disconnects = {}
local sheet = nil

-- Whether the account has a password, asked of smbd once a name and kept:
-- a question that is not STATE waits for smbd's serving thread, up to a
-- tenth of a second, which is no thing to ask at every repaint.
local kept_for = nil

local function password_kept(name)
  if name == "" or not fs.has_sharing() then return false end

  if not kept_for or kept_for.name ~= name then
    kept_for = { name = name, kept = fs.sharing_kept(name) and true or false }
  end

  return kept_for.kept
end

-- The folders, sent again when sharing is on: smbd takes them one at a time
-- and a change while serving takes effect from the next connection.
local function shares_changed(s)
  keep()

  if running(s) then
    local ok, why = fs.sharing_shares(config(s).shares)

    said = ok and "" or ("the folders: " .. tostring(why))
  end
end

-- A folder chosen in the Open window's folder mode: its last name, made
-- unique, is the share's name - "Music", then "Music 2".
local function share_folder(s, path)
  local c = config(s)

  if path ~= "/Home" and path:sub(1, 6) ~= "/Home/" then
    said = path .. " is not in /Home"
    return
  end

  for _, sh in ipairs(c.shares) do
    if sh.path == path then
      said = path .. " is shared already, as " .. sh.name
      return
    end
  end

  local base = path:match("([^/]+)$") or "Home"
  local name, n = base, 1
  local taken = function(x)
    for _, sh in ipairs(c.shares) do
      if sh.name:lower() == x:lower() then return true end
    end
    return false
  end

  while taken(name) do
    n = n + 1
    name = base .. " " .. n
  end

  c.shares[#c.shares + 1] = { name = name, path = path }
  shares_changed(s)
  print(("servers: sharing %s as %s"):format(path, name))
end

-- The password, typed once into a sheet and sent: smbd keeps its NT hash,
-- and nothing here keeps anything.
local function change_password(s)
  local c = config(s)

  if c.account == "" then
    said = "name the account first"
    rebuild()
    return
  end

  sheet = ui.sheet{ title = "A password for " .. c.account, w = 380, h = 190,
                    on_close = function() sheet = nil end }

  local field
  local function set()
    local ok, why = fs.sharing_account(c.account, field.text or "")

    field.text = ""

    if not ok then
      said = tostring(why)
    else
      said = ""
      print("servers: " .. c.account .. "'s password kept")
    end

    kept_for = nil
    sheet:close()
    rebuild()
  end

  field = ui.field{ x = 18, y = 64, w = 344, h = 26, text = "", secret = true,
                    hint = "password", on_enter = set }

  sheet:put(ui.label{ x = 18, y = 100, w = 344,
                      text = "Kept only as NTLMv2 needs it: its hash, never the words" })
  sheet:put(field)
  sheet:put(ui.button{ x = 380 - 18 - 96, y = 140, w = 96, h = 26, text = "Set",
                       on_click = set })
  sheet:put(ui.button{ x = 380 - 18 - 96 - 104, y = 140, w = 96, h = 26, text = "Cancel",
                       on_click = function() field.text = ""; sheet:close() end })
  win:add(sheet)
  win:focus_on(field)
  print("servers: the password sheet open")
end

-- The account's name, kept as it is typed: a name without spaces or the
-- characters a path uses, shorter than smbd's 64.
local function account_field_of(c)
  account_field = ui.field{ w = 160, text = c.account,
                            on_change = function(_, text)
                              local v = text:match("^%s*(.-)%s*$")

                              if #v < 64 and not v:find("[%s\\/:]") then
                                c.account = v
                                keep()
                              end
                            end }
  return account_field
end

local function page_sharing(s)
  local c = config(s)
  local a = address()
  local state = state_of(s)
  local kept = password_kept(c.account)

  sharing_switch = run_switch(s)

  local folders = {}

  for i, sh in ipairs(c.shares) do
    local size = fs.getattr(sh.path)

    folders[#folders + 1] = {
      label = sh.name,
      note = sh.path .. (size and "" or " · not there now") .. " · Read only",
      control = ui.button{ text = "Remove", on_click = function()
        table.remove(c.shares, i)
        shares_changed(s)
        rebuild()
      end } }
  end

  -- The folder window runs as the Open window always does, until it is
  -- closed - this window waits behind it.
  share_button = ui.button{ text = "Share a folder of /Home...", on_click = function()
    local chooser = panel.folder{ start = "/Home", title = "Share a folder of /Home",
                                  button = "Share",
                                  on_choose = function(path)
                                    share_folder(s, path)
                                    rebuild()
                                  end }

    if chooser then chooser:run() end
  end }

  folders[#folders + 1] = { label = #c.shares == 0 and "None yet" or "",
                            note = "Read and write comes in a later step",
                            control = share_button }

  password_button = ui.button{ text = "Change...", on_click = function() change_password(s) end }

  -- Who is connected, as smbd says it at once; Disconnect ends one.
  local who = {}

  disconnects = {}

  for _, v in ipairs((running(s) and fs.sharing_connections()) or {}) do
    local mins = v.for_ms // 60000

    who[#who + 1] = {
      label = v.address,
      note = ("As %s%s · for %s"):format(v.account ~= "" and v.account or "nobody yet",
                                         v.share ~= "" and (" · in " .. v.share) or "",
                                         mins > 0 and (mins .. " min") or ((v.for_ms // 1000) .. " s")),
      control = ui.button{ text = "Disconnect", on_click = function()
        local ok, why = fs.sharing_disconnect(v.id)

        said = ok and "" or ("could not disconnect: " .. tostring(why))
        sharing_state(true)
        rebuild()
      end } }
    disconnects[#disconnects + 1] = { who = v, button = who[#who].control }
  end

  if #who == 0 then
    who[1] = { label = "Nobody", note = "A visitor is listed here, with a Disconnect" }
  end

  return {
    { name = "", rows = {
        { label = "File sharing", control = sharing_switch,
          note = state .. ": folders of /Home, to the Mac's Finder, Windows and anything "
                 .. "else that speaks SMB"
                 .. (a and (" - " .. s.reach(a) .. " · seen as " .. c.hostname) or "") } } },
    { name = "Shared folders", rows = folders },
    { name = "Connected now", rows = who },
    { name = "Who may connect", rows = {
        { label = "Name", note = "The one account a visitor signs in with",
          control = account_field_of(c) },
        { label = "Password", value = kept and "Set" or "Not set",
          note = "Kept only as NTLMv2 needs it", control = password_button },
        { label = "Guests", value = "Off",
          note = "Nobody reaches a folder without the name and password" },
        { label = "This network only",
          note = c.local_only and "Others are turned away, from the next start"
                 or "Anywhere this machine can be reached, from the next start",
          control = ui.switch{ on = c.local_only == true,
                               on_change = function(_, on)
                                 c.local_only = on
                                 keep()
                               end } } } },
    { name = "Set up", rows = {
        { label = "Name on the network", note = "What Finder lists under Locations",
          control = ui.field{ w = 160, text = c.hostname,
                              on_change = function(_, text)
                                local v = text:match("^%s*(.-)%s*$"):upper()

                                if #v > 0 and #v <= 15 and v:match("^[%w%-]+$") then
                                  c.hostname = v
                                  keep()
                                end
                              end } },
        { label = "Encrypt everything",
          note = "SMB 3's AES on the wire - comes in a later step",
          control = ui.switch{ disabled = true } },
        { label = "Start with the machine", control = start_switch(s) } } },
  }
end

local PAGES = { all = page_all, web = page_web, telnet = page_telnet, vnc = page_vnc,
                sharing = page_sharing }

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

  -- File sharing's page, for the display harness: what it says and where
  -- its switch and its two buttons are, in the window.
  if showing == "sharing" and sharing_switch then
    local at = function(v)
      return ("%d,%d"):format(cards.x + v.x + v.w // 2, cards.y + v.y + v.h // 2)
    end

    print(("servers: file sharing - %s; its switch at %s, %s; share at %s; password at %s; "
           .. "name at %s"):format(
          state_of(s), at(sharing_switch), sharing_switch.disabled and "disabled" or "enabled",
          at(share_button), at(password_button), at(account_field)))

    for _, d in ipairs(disconnects) do
      print(("servers: connected %s as %s in %s; disconnect at %s"):format(
            d.who.address, d.who.account, d.who.share ~= "" and d.who.share or "nothing",
            at(d.button)))
    end
  end

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

  sharing_state(true)

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
