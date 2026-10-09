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

-- Where a mailbox's messages are kept: its name as a person reads it, so
-- Gmail's `[Gmail]/Sent Mail` is a folder inside a folder.
local function folder_of(a, mailbox)
  return a.dir .. "/" .. (mailbox.title or mailbox.name)
end

--
-- **The window learns what changed from here, not from a message**: Kosmos
-- has no message that does not wait for its answer, and `maild` must never
-- wait on a window. So each change is written whole, with `version` one
-- more, and the window reads it on its own clock - a read of `/Temporary`,
-- which nothing here holds up.
--
local version = 0

local function status()
  local out = {}

  for _, a in ipairs(accounts) do
    local boxes = {}

    for _, b in ipairs(a.list or {}) do
      local c = a.counts[b.name]

      boxes[#boxes + 1] = { name = b.name, title = b.title, use = b.use,
                            selectable = b.selectable,
                            folder = folder_of(a, b), kept = c ~= nil,
                            messages = c and c.messages, unseen = c and c.unseen }
    end

    out[#out + 1] = { address = a.address, name = a.account.name, state = a.state,
                      why = a.why, messages = a.messages, unseen = a.unseen,
                      boxes = boxes }
  end

  return { accounts = out, version = version }
end

local function publish()
  version = version + 1
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

-- What the list shows of a message, read from its file by the Mail Kit.
--
-- **Which reading worked a message's facts out.** One more each time what
-- the Mail Kit or this file makes of a message changes, so what was kept
-- before is worked out again: on 8 October a newsletter's preview was its
-- style sheet, and the messages kept then kept it until this said 2.
--
local FACTS = 2

local function facts(path, uid, flags)
  local r, size = regions.read_whole(path)

  if not r then return nil end

  local m = mailkit.parse(r.at, size)
  local out = { type = "mail", uid = uid, seen = flags.seen == true,
                flagged = flags.flagged == true, facts = FACTS }

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
  -- What is kept here, by UID: the server is asked about these alone.
  local kept = {}

  if state then
    for _, name in ipairs(fs.list(folder) or {}) do
      local uid = tonumber(name:match("^(%d+)%.eml$"))

      if uid then kept[#kept + 1] = uid end
    end
  end

  local ch, cwhy = await(s:changes(since, { newest = FIRST_KEEP, kept = kept }))

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

    -- A long first look is seen arriving, ten at a time.
    if (#new - i + 1) % 10 == 0 then publish() end
  end

  local changed, gone = 0, 0

  -- Kept by an older reading: its facts worked out again, its flags kept.
  local redone = 0

  for _, name in ipairs(fs.list(folder) or {}) do
    local uid = tonumber(name:match("^(%d+)%.eml$"))
    local path = folder .. "/" .. name
    local at = uid and fs.getattr(path)

    if at and (tonumber(at.facts) or 0) < FACTS then
      local f = facts(path, uid, { seen = at.seen, flagged = at.flagged })

      if f then
        fs.setattr(path, f)
        redone = redone + 1
      end
    end
  end

  if redone > 0 then note(a, ("%s: %d worked out again"):format(mailbox.title or mailbox.name, redone)) end

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

  a.counts[mailbox.name] = { messages = messages, unseen = unseen }

  if mailbox.use == "inbox" or mailbox.name:upper() == "INBOX" then
    a.messages, a.unseen = messages, unseen
  end

  -- Said when something moved, so a log reads as what happened.
  if state and #new + changed + gone > 0 then
    note(a, ("%s: %d new, %d changed, %d gone"):format(mailbox.title or mailbox.name,
                                                       #new, changed, gone))
  end

  return true
end

--------------------------------------------------------------------------
-- What the window asked: done on the server between one look and the next.
-- The window has already shown it - a message read, flagged, gone - so
-- this is the server catching up with what a person did.
--------------------------------------------------------------------------

local function box_named(a, name)
  for _, b in ipairs(a.list or {}) do
    if b.name == name then return b end
  end

  return nil
end

local function box_for(a, use)
  for _, b in ipairs(a.list or {}) do
    if b.use == use then return b end
  end

  return nil
end

local function forget_files(a, b, uids)
  for _, uid in ipairs(uids) do
    files.remove(("%s/%d.eml"):format(folder_of(a, b), uid))
  end
end

local function do_ops(a, s)
  while #a.ops > 0 do
    local op = table.remove(a.ops, 1)
    local b = box_named(a, op.mailbox)

    if b then
      local _, why = await(s:select(b.name))

      if why then return nil, why end

      local r, done

      if op.type == "flag" then
        r = s:flag(op.uids, op.flag, op.on)
      else
        -- Archive and Delete are a move to the mailbox for that use; Delete
        -- in the Trash, or with no Trash, is the server's delete.
        local to = op.type == "move" and box_named(a, op.to)
                   -- Gmail has no Archive: archiving there is All Mail
                   -- keeping it and the Inbox not.
                   or op.type == "archive" and (box_for(a, "archive") or box_for(a, "all"))
                   or op.type == "delete" and box_for(a, "trash")

        if to and to.name ~= b.name then
          r = s:move(op.uids, to.name)
        elseif op.type == "delete" then
          local mark = s:flag(op.uids, "deleted", true)

          await(mark)
          r = s:expunge(op.uids)
        end

        done = true
      end

      if r then
        await(r)
        note(a, ("%s %s in %s: %s"):format(op.type, imap.set(op.uids), b.title,
                                          r.ok and "done" or tostring(r.why)))

        if r.ok and done then forget_files(a, b, op.uids) end
      else
        note(a, ("%s in %s: nowhere to put them"):format(op.type, b.title))
      end
    end
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
      a.list = kept

      -- Said at once, so the window's sidebar has the mailboxes before the
      -- first look at the Inbox is done.
      publish()

      local inbox = { name = "INBOX", title = "INBOX" }

      for _, b in ipairs(boxes) do
        if b.use == "inbox" then inbox = b end
      end

      a.inbox_folder = folder_of(a, inbox)

      local first = true
      local ok, swhy = true, nil

      while ok and not s.broken do
        a.wake = false

        -- What the window asked, then every mailbox it has open, then the
        -- Inbox last - which is the one IDLE holds.
        ok, swhy = do_ops(a, s)

        for name in pairs(a.wanted) do
          local b = box_named(a, name)

          if ok and b and b.name ~= inbox.name then ok, swhy = sync(a, s, b, true) end
        end

        if not ok then break end

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

          while not s.news and not a.wake and #a.ops == 0 and not s.broken
                and sys.ticks() < until_ do
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

-- An account read from its folder, ready to run; nil when the folder
-- holds no account.
local function account_from(name)
  local dir = HOME .. "/" .. name
  local account = fs.read(dir .. "/account")

  if type(account) ~= "table" or type(account.imap) ~= "table" or not account.imap.host then
    return nil
  end

  local a = { address = account.address or name, account = account, dir = dir,
              state = "waiting", arrived = {}, messages = 0, unseen = 0,
              counts = {}, wanted = {}, ops = {} }

  a.co = coroutine.create(function() return run_account(a) end)

  return a
end

for _, name in ipairs(fs.list(HOME) or {}) do
  local a = account_from(name)

  if a then accounts[#accounts + 1] = a end
end

-- With no account, there is nothing to bring up to date; kept running, the
-- window's Add Account has somewhere to send the first one.
if #accounts == 0 then
  print("maild: no accounts in " .. HOME .. " yet")

  if ONCE then return end
end

publish()

-- Mail's own name, for the window to ask.
local control = not ONCE and sys.endpoint() or nil

if control then fs.send("/Running", { type = "register", name = "maild" }, control) end

local function account_named(address)
  for _, a in ipairs(accounts) do
    if a.address == address then return a end
  end

  return nil
end

-- UIDs as the window sent them: whole numbers, at most a thousand.
local function uids_of(list)
  local out = {}

  if type(list) ~= "table" then return out end

  for i = 1, math.min(#list, 1000) do
    local u = math.tointeger(list[i])

    if u and u > 0 then out[#out + 1] = u end
  end

  return out
end

local FLAGGABLE = { seen = true, flagged = true }
local MOVES = { move = true, archive = true, delete = true }

-- When the window last asked: for a while after, the loop looks more often,
-- so a press is answered in a fiftieth of a second rather than a tenth.
local asked = 0

local function answer()
  while control do
    local req, who = sys.receive(control, true)

    if not req then return end

    local reply = { ok = false }
    local t = type(req) == "table" and req.type
    local a = type(req) == "table" and account_named(tostring(req.account or ""))

    asked = sys.ticks()

    if t == "status" then
      reply = status()
      reply.ok = true
    elseif t == "sync" then
      for _, each in ipairs(accounts) do each.wake = true end
      reply = { ok = true }
    elseif t == "add" and type(req.account) == "string" and req.account:match("^[^/]+@[^/]+$") then
      --
      -- **An account the window has just written**, signed in now: its
      -- folder read again, and one already running started over with what
      -- the folder says - a password typed again after a refusal.
      --
      local fresh = account_from(req.account)

      if fresh then
        if a then
          if a.session and not a.session.broken then a.session:fail("signing in again") end

          for i, each in ipairs(accounts) do
            if each == a then accounts[i] = fresh end
          end
        else
          accounts[#accounts + 1] = fresh
        end

        note(fresh, "added")
        publish()
        reply = { ok = true }
      else
        reply = { ok = false, why = "no account in " .. HOME .. "/" .. req.account }
      end
    elseif t == "remove" and a then
      if a.session and not a.session.broken then a.session:fail("removed") end

      for i = #accounts, 1, -1 do
        if accounts[i] == a then table.remove(accounts, i) end
      end

      note(a, "removed")
      publish()
      reply = { ok = true }
    elseif t == "open" and a and type(req.mailbox) == "string" then
      a.wanted[req.mailbox] = true
      a.wake = true
      reply = { ok = true }
    elseif t == "flag" and a and type(req.mailbox) == "string" and FLAGGABLE[req.flag] then
      a.ops[#a.ops + 1] = { type = "flag", mailbox = req.mailbox, uids = uids_of(req.uids),
                            flag = req.flag, on = req.on ~= false }
      reply = { ok = true }
    elseif MOVES[t] and a and type(req.mailbox) == "string" then
      a.ops[#a.ops + 1] = { type = t, mailbox = req.mailbox, uids = uids_of(req.uids),
                            to = type(req.to) == "string" and req.to or nil }
      reply = { ok = true }
    end

    pcall(sys.reply, who, reply)
  end
end

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

  if alive == 0 and (ONCE or not control) then break end

  answer()

  local tick = math.max(1, (sys.ticks() - asked < 30 * counter_hz) and hz // 50 or hz // 10)

  if #conns > 0 then
    fs.poll("/Network", conns, {}, nil, tick)
  else
    sys.sleep(tick)
  end
end

print("maild: done")
