-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Tracker
-- kosmos: name Connect to Server
-- kosmos: section none
-- Connect to Server: a share on another machine, signed into from a window
-- (`docs/sharing.md` step N6, `docs/sharing.html`'s second state).
--
--   connect                              opened from Tracker's Network group
--   connect smb://10.0.2.2:4450/Projects with the address in it
--
-- **As the mockup draws it**: the server's address, the recent ones, what
-- the server answered, a name and a password, "Remember in this machine's
-- keyring" - ticked, as Diego decided (`docs/keyring.md`, K5) - and Cancel
-- and Connect. **A server remembered** says so once it answers, fills the
-- name in, and signs in with nothing typed: smbfs takes the password from
-- the keyring.
--
-- **The server answers before the password is asked for.** Typing an
-- address and pausing asks smbfs to PROBE it, so a mistyped address is said
-- at once rather than after a sign-in. An address with no share signs in
-- alone, and the shares the server offers are then buttons to choose from
-- (`SHARE_OP_SHARES`, "or choose one once it answers").
--
-- **Nothing here waits on a server, and nothing is asked in a press or a
-- paint** (`CLAUDE.md`, *nothing on the desktop waits on a server*). A
-- press says at once what it has begun and leaves the asking to this
-- window's clock (`on_frame`): smbfs answers PROBE and CONNECT at once and
-- `fs.share_status` - asked a few times a second while something is under
-- way - says how it went. The window moves, types and closes the whole time.
--
-- **The password field draws a bullet a character** (`ui.field`'s
-- `secret`); what was typed crosses to smbfs once, in CONNECT, and the field
-- is emptied as it goes. smbfs keeps the NT hash while connected, and the
-- password only until the server takes it, when Remember is ticked - then
-- the keyring keeps it, sealed. Nothing here keeps it: the recent servers
-- are addresses, shares and accounts (`/Home/Preferences/sharing`,
-- `netshares.lua`).
--
-- The Tracker that opened it goes to the share once it is connected; this
-- window says so and closes.

local ui = use("/Kosmos/Libraries/ui.lua")
local prefs = use("/Kosmos/Libraries/prefs.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local netshares = use("/Kosmos/Libraries/netshares.lua")
local files = use("/Kosmos/Libraries/files.lua")
local theme = ui.theme

local W, H = 520, 560
local PAD = 20
local GAP = 10

local win, err = ui.window{ title = "Connect to Server", w = W, h = H,
                            x = 250, y = 110 }

if not win then
  print("connect: " .. tostring(err))
  return
end

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

--------------------------------------------------------------------------
-- What was asked, and what was heard.
--------------------------------------------------------------------------

local recent = (prefs.read(netshares.PREFS) or {}).recent or {}

local want = nil            -- an action a press began, for the clock to send
local typed_at = nil        -- counter ticks: the address last changed
local probed = nil          -- the address last probed
local asked = nil           -- { address, share, account } after Connect
local offered = nil         -- what a server signed into offers
local done_at = nil         -- counter ticks: connected, and closing soon
local looked_at = 0
local said = { kind = "hint", text = "" }

-- Said once a change, for the display harness and the log.
local place

local function say(kind, text)
  if said.kind == kind and said.text == text then return end

  said = { kind = kind, text = text }
  print("connect: " .. kind .. ": " .. text)
  win.dirty = true

  if place then place() end
end

--------------------------------------------------------------------------
-- The controls.
--------------------------------------------------------------------------

local address = ui.field{ x = PAD, y = 0, w = W - 2 * PAD,
                          text = files.words(args)[1] or "",
                          hint = "smb://192.168.1.38/Projects" }

local half = (W - 2 * PAD - GAP) // 2
local account = ui.field{ x = PAD, y = 0, w = half, text = "", hint = "name" }
local password = ui.field{ x = PAD + half + GAP, y = 0, w = half, text = "",
                           secret = true, hint = "password" }

-- Ticked unless it is unticked: once the server takes the password, the
-- keyring keeps it and the share connects when Kosmos starts (K5).
local remember = ui.checkbox{ x = PAD, y = 0,
                              text = "Remember in this machine\u{2019}s keyring",
                              checked = true }

-- What the keyring keeps for the address that answered: its account, or
-- false when nothing is; asked once an address, on the clock.
local remembered = {}

local cancel = ui.button{ text = "Cancel" }
local go = ui.button{ text = "Connect", go = true }

-- The shares a server offers, as buttons, once it has been signed into.
local choices = {}

-- The latest account typed for an address, from what was remembered.
local function account_for(server)
  for _, r in ipairs(recent) do
    if r.address == server and r.account then return r.account end
  end

  return nil
end

--------------------------------------------------------------------------
-- The page: what is drawn where, worked out again when what is shown
-- changes (a server's shares arriving adds a row of buttons).
--------------------------------------------------------------------------

local body = ui.view{ x = 0, y = 0, w = W, h = H }
local layout = {}

local NOTE = "An address, or a name: 192.168.1.38, diego-mac. A share after "
             .. "the slash, or choose one once it answers."
local KEYRING = "Kept sealed, and shown in Passwords. The share connects "
                .. "when Kosmos starts."

function place()
  local y = 16
  local lh = gfx.height()

  layout.server = y
  y = y + gfx.height("text") + 6
  address.y = y
  y = y + address.h + 6
  layout.note = y
  layout.note_lines = ui.wrapped(NOTE, W - 2 * PAD)
  y = y + #layout.note_lines * lh + 12

  local shown = math.min(#recent, 3)

  if shown > 0 then
    layout.recent = y
    y = y + gfx.height("text") + 6
    layout.recent_rows = y
    y = y + shown * 30 + 12
  else
    layout.recent = nil
  end

  layout.said = y
  layout.said_lines = ui.wrapped(said.text ~= "" and said.text or " ",
                                 W - 2 * PAD - 40)
  y = y + math.max(40, #layout.said_lines * lh + 18) + 10

  -- The shares to choose from, under what the server said.
  local x = PAD

  for _, b in ipairs(choices) do
    if x + b.w > W - PAD then
      x = PAD
      y = y + b.h + 6
    end

    b.x, b.y = x, y
    x = x + b.w + 8
  end

  if #choices > 0 then y = y + choices[1].h + 12 end

  layout.names = y
  y = y + gfx.height("text") + 6
  account.y, password.y = y, y
  y = y + account.h + 12
  remember.y = y
  y = y + remember.h + 4
  -- The keyring's sentence wrapped to the room beside the box, which it
  -- ran past on one line (Diego's screenshot, 7 October).
  layout.keyring = y
  layout.keyring_lines = ui.wrapped(KEYRING, W - 2 * PAD - 26)
  y = y + #layout.keyring_lines * lh + 12

  go.x, go.y = W - PAD - go.w, H - PAD - go.h
  cancel.x, cancel.y = go.x - 10 - cancel.w, go.y

  --
  -- Where its fields are, in the window, for the display harness - which
  -- types into them by the keyboard and reads the password's from the
  -- screen - said again whenever what the server said moves them.
  --
  local top = win.head_h or 0
  local where = ("connect: address at %d,%d, name at %d,%d, password at %d,%d %dx%d, "
                 .. "connect at %d,%d"):format(
                address.x + 10, top + address.y + address.h // 2,
                account.x + 10, top + account.y + account.h // 2,
                password.x, top + password.y, password.w, password.h,
                go.x + go.w // 2, top + go.y + go.h // 2)

  if where ~= layout.told then
    layout.told = where
    print(where)
  end
end

function body:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  local function dim(x, y, words, role)
    g:text(x, y, words, theme.text_dim, nil, role)
  end

  dim(PAD, layout.server, "Server", "text")

  for i, line in ipairs(layout.note_lines) do
    dim(PAD, layout.note + (i - 1) * gfx.height(), line)
  end

  -- The recent ones: a card of rows, each the address and when.
  if layout.recent then
    dim(PAD, layout.recent, "Recent", "text")

    local n = math.min(#recent, 3)
    local top = layout.recent_rows

    g:fill_round(PAD, top, W - 2 * PAD, n * 30, theme.sunken, ui.layout.card_r)
    g:frame_round(PAD, top, W - 2 * PAD, n * 30, theme.line_soft, ui.layout.card_r)

    local now = (fs.read("/Devices/clock") or {}).epoch

    for i = 1, n do
      local r = recent[i]
      local ry = top + (i - 1) * 30

      if i > 1 then g:fill(PAD + 1, ry, W - 2 * PAD - 2, 1, theme.line_soft) end

      g:line_icon(PAD + 12, ry + 7, "recent", theme.text_dim)
      g:text(PAD + 36, ry + (30 - gfx.height()) // 2,
             ui.fitted(netshares.url(r.address, r.share), W - 2 * PAD - 130),
             theme.text, theme.sunken)

      if now and r.at then
        local when = clock.day_word(r.at, now)

        g:text(W - PAD - 12 - gfx.measure(when), ry + (30 - gfx.height()) // 2,
               when, theme.text_dim, theme.sunken)
      end
    end
  end

  -- What the server said: green once it has answered, amber when it would
  -- not, plain while asking - words in every case, never a spinner.
  if said.text ~= "" then
    local warn, warn_ground = ui.warning_colours()
    local ground, ink, icon = theme.raised, theme.text, nil

    if said.kind == "answered" or said.kind == "done" or said.kind == "shares" then
      ground, icon, ink = theme.mix(theme.sunken, theme.good, 120), "check", theme.good
    elseif said.kind == "refused" then
      ground, icon, ink = warn_ground, "warning", warn
    end

    local lines = ui.wrapped(said.text, W - 2 * PAD - 40)
    local h = math.max(40, #lines * gfx.height() + 18)

    g:fill_round(PAD, layout.said, W - 2 * PAD, h, ground, ui.layout.card_r)

    if icon then
      g:line_icon(PAD + 12, layout.said + (h - 15) // 2, icon, ink)
    end

    for i, line in ipairs(lines) do
      g:text(PAD + 36, layout.said + 9 + (i - 1) * gfx.height(), line,
             theme.text, ground)
    end
  end

  dim(PAD, layout.names, "Name", "text")
  dim(PAD + half + GAP, layout.names, "Password", "text")
  local server = netshares.split(address.text)
  local who = server and remembered[server]

  local lines = who and ui.wrapped("Remembered: signs in as " .. who
                                  .. " with nothing to type.", W - 2 * PAD - 26)
                or layout.keyring_lines or { KEYRING }

  for i, line in ipairs(lines) do
    dim(PAD + 26, layout.keyring + (i - 1) * gfx.height(), line)
  end
end

--------------------------------------------------------------------------
-- Presses: each says at once what it has begun, and the clock asks.
--------------------------------------------------------------------------

local function close_soon(words)
  say("done", words)
  done_at = sys.ticks()
end

local function begin_connect(share)
  local server, typed_share = netshares.split(address.text)

  if not server then
    say("refused", "Type an address first: smb://192.168.1.38/Projects")
    return
  end

  if account.text == "" then
    say("refused", "A name is needed: guests are not let in.")
    win:focus_on(account)
    return
  end

  -- What the password's field draws, as the press is made: a bullet a
  -- character, and none of the password (the display harness holds it to
  -- that, and to the screen's pixels).
  print(("connect: the password field draws %d characters as %s"):format(
        utf8.len(password.text) or #password.text, password:shown()))

  want = { op = "connect", address = server, share = share or typed_share,
           account = account.text }
  say("asking", ("Signing in to %s as %s\u{2026}"):format(server, account.text))
  win.poll_wait_ticks = 1
end

go.on_click = function() begin_connect() end
cancel.on_click = function() win:close() end

function address:on_change()
  typed_at = sys.ticks()
  offered = nil

  -- An address of a server remembered brings its account.
  local server = netshares.split(self.text)
  local known = server and account_for(server)

  if known and account.text == "" then account.text = known end
end

function address:on_enter() win:focus_on(account) end
function account:on_enter() win:focus_on(password) end
function password:on_enter() begin_connect() end

-- The recent ones are pressed to fill the address in.
function body:mouse(action, x, y)
  if action ~= "press" or not layout.recent then return false end

  local i = (y - layout.recent_rows) // 30 + 1

  if y >= layout.recent_rows and i >= 1 and i <= math.min(#recent, 3)
     and x >= PAD and x < W - PAD then
    local r = recent[i]

    address.text = netshares.url(r.address, r.share)
    address.caret = #address.text + 1
    account.text = r.account or account.text
    typed_at = sys.ticks()
    win:focus_on(password)
    win.dirty = true
    return true
  end

  return false
end

local function show_choices(list)
  for _, b in ipairs(choices) do win:remove(b) end

  choices = {}

  for _, one in ipairs(list or {}) do
    if one.state ~= "connected" then
      local b = ui.button{ text = one.name, icon = "folder" }

      b.on_click = function()
        want = { op = "share", address = asked.address, share = one.name,
                 account = asked.account }
        say("asking", ("Opening %s\u{2026}"):format(one.name))
        win.poll_wait_ticks = 1
      end

      choices[#choices + 1] = b
      win:add(b)
    end
  end

  place()

  -- Where each is, for the display harness, which chooses one.
  for _, b in ipairs(choices) do
    print(("connect: share %s at %d,%d"):format(b.text, b.x + b.w // 2,
                                                (win.head_h or 0) + b.y + b.h // 2))
  end
end

--------------------------------------------------------------------------
-- The clock: what a press began is sent, and what smbfs says is read.
--------------------------------------------------------------------------

local function record(server)
  for _, s in ipairs(fs.share_status() or {}) do
    if s.address == server and not s.probe then return s end
  end

  for _, s in ipairs(fs.share_status() or {}) do
    if s.address == server then return s end
  end

  return nil
end

local function named(s)
  return (s.name ~= "" and s.name) or s.address
end

local function how(s)
  return "SMB " .. tostring(s.dialect or "?")
         .. (s.sealing and ", sealed" or s.signing and ", signed" or "")
end

-- Remembered, for the next time: an address, a share, an account - never a
-- password.
local function remember_it(server, share, who)
  local now = (fs.read("/Devices/clock") or {}).epoch
  local kept = prefs.read(netshares.PREFS) or {}

  kept.recent = netshares.remember(kept.recent or {},
                                   { address = server, share = share or "",
                                     account = who, at = now })
  prefs.write(netshares.PREFS, kept)
end

local function send(action)
  local s = record(action.address)
  local signed = s and s.state == "connected" and s.account == action.account
  local secret = signed and "" or password.text

  -- No password typed for an account remembered there: none is sent, and
  -- smbfs signs in with the keyring's. Remember only what was typed.
  local keep = remember.checked and secret ~= ""

  local ok, why = fs.share_connect(action.address, action.share or "",
                                   action.account, secret, keep)

  -- The password has crossed, once; the field is emptied of it.
  password.text, password.caret = "", 1
  secret = nil

  if not ok then
    say("refused", tostring(why))
    return
  end

  asked = { address = action.address, share = action.share or "",
            account = action.account, signed = signed }
end

-- Where a connect has got to, from STATUS and SHARES.
local function follow()
  local s = record(asked.address)

  if not s then
    say("refused", asked.address .. " was let go of")
    asked = nil
    return
  end

  if s.state == "asking" then
    say("asking", ("Asking %s\u{2026}"):format(asked.address))
    return
  end

  if s.state ~= "connected" then
    say("refused", s.why ~= "" and s.why or (asked.address .. " did not answer"))
    asked = nil
    return
  end

  local list = fs.share_shares(asked.address)

  if asked.share == "" then
    if list and list.known then
      if #choices == 0 then show_choices(list) end
      say("shares", ("Signed in to %s as %s - %s. Choose a share."):format(
          named(s), s.account, how(s)))
    else
      say("asking", ("Signed in to %s - asking it for its shares\u{2026}"):format(named(s)))
    end
    return
  end

  for _, one in ipairs(list or {}) do
    if one.name:lower() == asked.share:lower() then
      if one.state == "connected" then
        remember_it(asked.address, one.name, asked.account)
        close_soon(("Connected - %s on %s, %s, as %s. It opens in Tracker."):format(
                   one.name, named(s), how(s), s.account))
        asked = nil
        return
      elseif one.state == "refused" then
        say("refused", s.why ~= "" and s.why or (one.name .. " was refused"))
        asked = nil
        return
      end
    end
  end

  say("asking", ("Signed in to %s - opening %s\u{2026}"):format(named(s), asked.share))
end

-- What the address answers, before anything is signed into.
local function probe_typed()
  local server = netshares.split(address.text)

  if not server then return end

  local s = record(server)

  if s and s.state == "connected" and not s.probe then
    say("answered", ("Signed in to %s as %s - %s. Connect opens %s on the "
                     .. "same connection."):format(named(s), s.account, how(s),
                                                   select(2, netshares.split(address.text)) ~= ""
                                                   and select(2, netshares.split(address.text))
                                                   or "a share"))
    probed = server
    return
  end

  if probed ~= server then
    probed = server

    local ok, why = fs.share_probe(server)

    if not ok then
      say("refused", tostring(why))
      return
    end
  end

  s = record(server)

  if not s or s.state == "asking" then
    say("asking", ("Asking %s\u{2026}"):format(server))
  elseif s.state == "answered" then
    local _, share = netshares.split(address.text)

    -- Remembered? Asked once, here on the clock, never in a paint.
    if remembered[server] == nil then
      remembered[server] = fs.share_remembered(server) or false

      if remembered[server] and account.text == "" then
        account.text = remembered[server]
      end
    end

    say("answered", ("%s answered - SMB %s%s. %s %s."):format(
        server, tostring(s.dialect or "?"),
        s.signing and ", signing required" or "",
        remembered[server] and "Connect to open" or "Sign in to open",
        share ~= "" and share or "it and choose a share"))
  else
    say("refused", s.why ~= "" and s.why or (server .. " did not answer"))
  end
end

function win:on_frame()
  local now = sys.ticks()

  -- Connected: said, and closed a moment later.
  if done_at then
    self.poll_wait_ticks = 25

    if now - done_at > counter_hz then self:close() end

    return false
  end

  if want then
    local action = want

    want = nil
    send(action)
    looked_at = 0
  end

  -- Asked a few times a second while something is under way, and on the
  -- default quarter second otherwise.
  self.poll_wait_ticks = (asked or typed_at) and 25 or nil

  if now - looked_at < counter_hz // 4 then return false end

  looked_at = now

  if asked then
    follow()
  elseif typed_at and now - typed_at > counter_hz * 6 // 10 then
    typed_at = nil
    probe_typed()
  elseif probed and said.kind == "asking" then
    probe_typed()
  end

  place()
  return true
end

--------------------------------------------------------------------------

win:add(body)
win:add(address)
win:add(account)
win:add(password)
win:add(remember)
win:add(cancel)
win:add(go)

place()
win:focus_on(address)

if address.text ~= "" then
  typed_at = sys.ticks()

  local server = netshares.split(address.text)
  account.text = (server and account_for(server)) or ""
end

win:run()
