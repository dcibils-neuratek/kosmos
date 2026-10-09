-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs keyring-mail
-- maild: Mail's half that runs without the window (`docs/mail.md` M3).
--
--   maild                 every account in /Home/Mail, kept, until stopped
--   maild &               the same, in the background - as the desktop runs it
--   maild --once          every account brought up to date once, and ended
--
-- **What it does**, for each account `/Home/Mail/<address>/account` names:
-- signs in to its IMAP server with the password the keyring keeps for it
-- (never written anywhere else, and held only for the sign-in), lists its
-- mailboxes into `mailboxes`, brings the Inbox up to date - each message a
-- file, `INBOX/<uid>.eml`, its facts as attributes the list shows without
-- opening it - and then holds the Inbox open with IDLE, fetching what
-- arrives and saying each message in a notification. When the connection
-- goes, it signs in again and catches up from where it was, by CONDSTORE,
-- rather than fetching everything.
--
-- **Nothing waits on one account**: each is a coroutine stepped when its
-- connection has news, and one loop waits on every connection at once
-- (`fs.poll`), answering `/Running/maild` between - `{ type = "status" }`
-- for what each account is doing, `{ type = "sync" }` to look now. It is
-- Mail's own name, in tables, as an application's is (`docs/mail.md`).
--
-- An account is a table:
--
--   { address = "lena@example.com", name = "Lena Moreau",
--     imap = { host = "imap.example.com", port = 993 },
--     smtp = { host = "smtp.example.com", port = 587 },
--     certificate = "/Home/ca.der",     an authority of its own, a test's
--     tls_name = "imap.example.com" }   the name its certificate is for
--
-- and its password is the keyring's, under `imap://<host>:<port>` and the
-- address: `mailpass keep imap://imap.example.com:993 lena@example.com ...`.

local imap = use("/Kosmos/Libraries/imap.lua")
local files = use("/Kosmos/Libraries/files.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
local mailkit = use("/Kosmos/Kits/mail")

local HOME = "/Home/Mail"
local ONCE = tostring(args or ""):match("%-%-once") ~= nil

-- A first look at a mailbox keeps its newest hundred; older mail stays on
-- the server until it is asked for. Ten thousand messages fetched on the
-- first evening is a night's download nobody asked for.
local FIRST_KEEP = 100

-- IDLE is ended and begun again before servers end it (RFC 2177: 29
-- minutes); a server without IDLE is asked every two.
local IDLE_SECONDS = 25 * 60
local POLL_SECONDS = 120

-- After a failure, the next try: a broken network is waited out, not
-- hammered.
local RETRY_SECONDS = 30

-- More new messages than this at once are said in one notification.
local SAY_EACH = 5

local hz = (sys.info() or {}).tick_hz or 250
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

local function seconds_from_now(s) return sys.ticks() + s * counter_hz end

local accounts = {}

local function note(a, text)
  print(("maild: %s: %s"):format(a and a.address or "-", text))
end

--------------------------------------------------------------------------
-- What is published: each account's state, for Mail's window and for
-- `/Running/maild`'s status.
--------------------------------------------------------------------------

local function status()
  local out = {}

  for _, a in ipairs(accounts) do
    out[#out + 1] = { address = a.address, state = a.state, why = a.why,
                      messages = a.messages, unseen = a.unseen }
  end

  return { accounts = out }
end

local function publish()
  files.make_folder("/Temporary/maild")
  fs.write("/Temporary/maild/status", status())
end

local function set_state(a, state, why)
  if a.state == state and a.why == why then return end

  a.state, a.why = state, why
  publish()
end

--------------------------------------------------------------------------
-- Waiting, inside an account's coroutine: for a request to be answered,
-- for a time, or for news.
--------------------------------------------------------------------------

local function await(r)
  while not r.done do coroutine.yield() end

  if r.ok then return r.result end

  return nil, r.why
end

local function pause(a, seconds)
  local until_ = seconds_from_now(seconds)

  while sys.ticks() < until_ and not a.wake do coroutine.yield() end
end

--------------------------------------------------------------------------
-- A message kept: its file, and its facts as attributes.
--------------------------------------------------------------------------

local function folder_of(a, mailbox)
  return a.dir .. "/" .. (mailbox.title or mailbox.name)
end

-- What the list shows of a message, read from its file by the Mail Kit.
local function facts(path, uid, flags)
  local r, size = regions.read_whole(path)

  if not r then return nil end

  local m = mailkit.parse(r.at, size)
  local out = { type = "mail", uid = uid, seen = flags.seen == true,
                flagged = flags.flagged == true }

  if m then
    local from = m:addresses("from")[1] or {}
    local attachments = 0

    for _, p in ipairs(m:parts()) do
      if p.disposition == "attachment" or (p.name ~= "" and not p.multipart) then
        attachments = attachments + 1
      end
    end

    out.from = from.name ~= "" and from.name or from.address
    out.from_address = from.address
    out.subject = m:header("subject") or ""
    out.date = m:date()
    out.preview = m:preview(160)
    out.attachments = attachments
    out.id = m:header("message-id")
  end

  regions.free(r)

  return out
end

local function keep(a, s, folder, m, fresh)
  local path = ("%s/%d.eml"):format(folder, m.uid)
  local got, why = await(s:fetch(m.uid, path))

  if not got then
    note(a, ("message %d not fetched: %s"):format(m.uid, tostring(why)))
    return nil
  end

  local f = facts(path, m.uid, got.flags or m.flags or {})

  if f then fs.setattr(path, f) end

  if fresh then a.arrived[#a.arrived + 1] = { path = path, facts = f or {} } end

  return path
end

--
-- What came, said: a notification for each message, or one for all of
-- them when many came at once. A press opens the message.
--
local function say_arrived(a)
  local list = a.arrived

  a.arrived = {}

  if #list == 0 then return end

  if #list > SAY_EACH then
    notify.post{ title = ("%d new messages"):format(#list),
                 body = a.address, open = a.inbox_folder }
    return
  end

  for _, m in ipairs(list) do
    notify.post{ title = m.facts.from or a.address,
                 body = m.facts.subject or "", open = m.path }
  end
end

--------------------------------------------------------------------------
-- A mailbox brought up to date: what is new fetched, what changed marked,
-- what is gone taken away.
--------------------------------------------------------------------------

local function sync(a, s, mailbox, quiet)
  local folder = folder_of(a, mailbox)
  local state_path = folder .. "/state"
  local state = fs.read(state_path)

  if type(state) ~= "table" then state = nil end

  files.make_folder(folder)

  local box, why = await(s:select(mailbox.name))

  if not box then return nil, why end

  local since = state and { uidvalidity = state.uidvalidity, uid_high = state.uid_high,
                            modseq = state.modseq }
  local ch, cwhy = await(s:changes(since))

  if not ch then return nil, cwhy end

  -- A mailbox whose UIDVALIDITY moved is another mailbox: what was kept
  -- of it names messages that are no longer those.
  if ch.reset and state then
    note(a, mailbox.name .. " is not the mailbox it was; kept again from the start")

    for _, name in ipairs(fs.list(folder) or {}) do
      if name:match("%.eml$") then files.remove(folder .. "/" .. name) end
    end
  end

  -- New: on a first look the newest hundred, and said only after it.
  local new = ch.new

  table.sort(new, function(x, y) return x.uid > y.uid end)

  if not state and #new > FIRST_KEEP then
    for i = #new, FIRST_KEEP + 1, -1 do new[i] = nil end
  end

  for i = #new, 1, -1 do
    keep(a, s, folder, new[i], state ~= nil and not quiet)
  end

  local changed, gone = 0, 0

  -- Changed: the flags the list shows.
  for _, m in ipairs(ch.changed) do
    local path = ("%s/%d.eml"):format(folder, m.uid)

    if fs.getattr(path) then
      fs.setattr(path, { seen = m.flags.seen == true, flagged = m.flags.flagged == true })
      changed = changed + 1
    end
  end

  -- Gone: what the server no longer has, when it was asked.
  if since and not ch.reset then
    local present = {}

    for _, u in ipairs(ch.present) do present[u] = true end

    for _, name in ipairs(fs.list(folder) or {}) do
      local uid = tonumber(name:match("^(%d+)%.eml$"))

      if uid and uid <= (since.uid_high or 0) and not present[uid] then
        files.remove(folder .. "/" .. name)
        gone = gone + 1
      end
    end
  end

  fs.write(state_path, { uidvalidity = ch.uidvalidity, uid_high = ch.uid_high,
                         modseq = ch.modseq })

  -- What the window's Inbox row says.
  local messages, unseen = 0, 0

  for _, name in ipairs(fs.list(folder) or {}) do
    if name:match("%.eml$") then
      messages = messages + 1

      local at = fs.getattr(folder .. "/" .. name) or {}

      if not at.seen then unseen = unseen + 1 end
    end
  end

  a.messages, a.unseen = messages, unseen

  -- Said when something moved, so a log reads as what happened.
  if state and #new + changed + gone > 0 then
    note(a, ("%s: %d new, %d changed, %d gone"):format(mailbox.title or mailbox.name,
                                                       #new, changed, gone))
  end

  return true
end

--------------------------------------------------------------------------
-- An account, from sign-in to IDLE and round again.
--------------------------------------------------------------------------

local function session_of(a)
  local acc = a.account
  local service = ("imap://%s:%d"):format(acc.imap.host, acc.imap.port or 993)
  local password, why = fs.mail_password(service, a.address)

  if not password then return nil, "no password kept for " .. service .. ": " .. tostring(why) end

  local anchors

  if acc.certificate then
    local der = fs.read(acc.certificate)

    if type(der) == "string" then anchors = { der } end
  end

  local s, oops = imap.open{ host = acc.imap.host, port = acc.imap.port or 993,
                             tls = acc.imap.tls ~= false, name = acc.tls_name,
                             anchors = anchors, user = acc.user or a.address,
                             password = password }

  -- The password is not kept past the sign-in it was read for.
  password = nil

  return s, oops
end

local function run_account(a)
  while true do
    a.wake = false
    set_state(a, "signing in")

    local s, why = session_of(a)
    local can

    if s then
      a.session = s
      can, why = await(s.ready)
    end

    if not can then
      note(a, "could not sign in: " .. tostring(why))
      set_state(a, "error", tostring(why))
      if s then s:fail("given up") end
      a.session = nil

      if ONCE then return end

      pause(a, RETRY_SECONDS)
    else
      note(a, "signed in")
      set_state(a, "syncing")

      local boxes = await(s:mailboxes()) or {}
      local kept = {}

      for _, b in ipairs(boxes) do
        kept[#kept + 1] = { name = b.name, title = b.title, use = b.use,
                            selectable = b.selectable }
      end

      fs.write(a.dir .. "/mailboxes", kept)

      local inbox = { name = "INBOX", title = "INBOX" }

      for _, b in ipairs(boxes) do
        if b.use == "inbox" then inbox = b end
      end

      a.inbox_folder = folder_of(a, inbox)

      local first = true
      local ok, swhy = true, nil

      while ok and not s.broken do
        a.wake = false
        ok, swhy = sync(a, s, inbox, false)

        if not ok then break end

        if first then
          note(a, ("%d messages kept, %d unseen"):format(a.messages, a.unseen))
          first = false
        end

        say_arrived(a)
        set_state(a, "idle")

        if ONCE then
          await(s:logout())
          return
        end

        if can.IDLE then
          local idle = s:idle()
          local until_ = seconds_from_now(IDLE_SECONDS)

          while not s.news and not a.wake and not s.broken and sys.ticks() < until_ do
            coroutine.yield()
          end

          s:done()
          await(idle)
        else
          pause(a, POLL_SECONDS)
        end

        set_state(a, "syncing")
      end

      note(a, "connection lost: " .. tostring(swhy or s.broken))
      set_state(a, "error", tostring(swhy or s.broken))

      if not s.broken then s:fail("given up") end
      a.session = nil

      if ONCE then return end

      pause(a, RETRY_SECONDS)
    end
  end
end

--------------------------------------------------------------------------
-- The accounts, from /Home/Mail.
--------------------------------------------------------------------------

for _, name in ipairs(fs.list(HOME) or {}) do
  local dir = HOME .. "/" .. name
  local account = fs.read(dir .. "/account")

  if type(account) == "table" and type(account.imap) == "table" and account.imap.host then
    local a = { address = account.address or name, account = account, dir = dir,
                state = "waiting", arrived = {}, messages = 0, unseen = 0 }

    a.co = coroutine.create(function() return run_account(a) end)
    accounts[#accounts + 1] = a
  end
end

if #accounts == 0 then
  print("maild: no accounts in " .. HOME)
  return
end

publish()

-- Mail's own name, for the window to ask.
local control = not ONCE and sys.endpoint() or nil

if control then fs.send("/Running", { type = "register", name = "maild" }, control) end

local function answer()
  while control do
    local req, who = sys.receive(control, true)

    if not req then return end

    local reply = { ok = false }

    if type(req) == "table" and req.type == "status" then
      reply = status()
      reply.ok = true
    elseif type(req) == "table" and req.type == "sync" then
      for _, a in ipairs(accounts) do a.wake = true end
      reply = { ok = true }
    end

    pcall(sys.reply, who, reply)
  end
end

local tick = math.max(1, hz // 10)

while true do
  local conns, alive = {}, 0

  for _, a in ipairs(accounts) do
    if a.session and not a.session.broken then
      a.session:step()
      conns[#conns + 1] = a.session.stream.conn
    end

    if coroutine.status(a.co) ~= "dead" then
      local ok, err = coroutine.resume(a.co)

      if not ok then
        note(a, "stopped on an error: " .. tostring(err))
        set_state(a, "error", tostring(err))

        if a.session and not a.session.broken then a.session:fail("stopped") end
        a.session = nil

        -- And started again, after the pause any failure gets.
        a.co = coroutine.create(function()
          pause(a, RETRY_SECONDS)
          return run_account(a)
        end)
      end
    end

    if coroutine.status(a.co) ~= "dead" then alive = alive + 1 end
  end

  if alive == 0 then break end

  answer()

  if #conns > 0 then
    fs.poll("/Network", conns, {}, nil, tick)
  else
    sys.sleep(tick)
  end
end

print("maild: done")
