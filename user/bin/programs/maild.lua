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
--
-- **A POP3 account** has `pop3 = { host, port = 995 }` where `imap` would
-- be, and its password under `pop3://<host>:<port>`. POP3 has one maildrop
-- and no flags, so such an account is two folders kept here - its Inbox,
-- fetched, and a Sent of what was sent from it - and a look every two
-- minutes, signed out between: a POP3 server locks the maildrop while
-- somebody is in it. Mail stays on the server, as Gmail's POP keeps it,
-- until it is deleted here.

local imap = use("/Kosmos/Libraries/imap.lua")
local pop3 = use("/Kosmos/Libraries/pop3.lua")
local smtp = use("/Kosmos/Libraries/smtp.lua")
local files = use("/Kosmos/Libraries/files.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
local mailkit = use("/Kosmos/Kits/mail")

local HOME = "/Home/Mail"

-- **What is written is sent from here** (M6): the window puts a message in
-- the Outbox - `<id>.eml` and `<id>.send`, its envelope - and asks; `maild`
-- sends it, so a message is still sent when the window that wrote it has
-- closed, and one the network refused is still there to try again. A draft
-- is kept in `Drafts here` by the window, and its copy on the server by
-- this. Neither folder is an account: neither has an `account` file.
local OUTBOX = HOME .. "/Outbox"
local DRAFTS = HOME .. "/Drafts here"
local ONCE = tostring(args or ""):match("%-%-once") ~= nil

-- What the window may ask done to a message.
local FLAGGABLE = { seen = true, flagged = true, answered = true }
local MOVES = { move = true, archive = true, delete = true }

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
local status, outbox_status           -- below, and each needs the other's name
local account_named
local keep_here, run_pop_account      -- POP3's, below the IMAP account's

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

-- The messages being sent, by their id: { id, account, subject, state =
-- "sending" | "failed", why, job }.
local outbox = {}

function outbox_status()
  local out = {}

  for _, o in pairs(outbox) do
    out[#out + 1] = { id = o.id, account = o.account, subject = o.subject, state = o.state, why = o.why }
  end

  table.sort(out, function(x, y) return x.id < y.id end)
  return out
end

function status()
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

  return { accounts = out, version = version, outbox = outbox_status() }
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
-- style sheet, and the messages kept then kept it until this said 2; 3 is
-- `to`, whom a message went to, for the composer's addresses (M6); 4 is
-- `words`, the start of its text, so a search finds a message by what it
-- says without reading it (M8).
--
local FACTS = 4

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

    -- Whom it went to, To and Cc, a line each - "name<TAB>address" - so
    -- what was sent completes an address as what arrived does.
    local to = {}

    for _, field in ipairs({ "to", "cc" }) do
      for _, who in ipairs(m:addresses(field)) do
        if #to < 50 then to[#to + 1] = (who.name or "") .. "\t" .. who.address end
      end
    end

    -- Every attribute shares one block of the disk: a message to a crowd
    -- keeps its first thousand bytes of them, and its text its first
    -- thousand characters, lower case, for a search to look in.
    out.to = table.concat(to, "\n"):sub(1, 1000)
    out.words = m:preview(1000):lower()
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

-- Kept by an older reading: its facts worked out again, its flags kept.
local function redo_facts(a, mailbox, folder)
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
end

-- What the window's row for a mailbox says: its messages, and those unseen.
local function counted(a, mailbox, folder)
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

  redo_facts(a, mailbox, folder)

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

  counted(a, mailbox, folder)

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

local forget_files

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

function forget_files(a, b, uids)
  for _, uid in ipairs(uids) do
    files.remove(("%s/%d.eml"):format(folder_of(a, b), uid))
  end
end

-- Which server copy each draft has: { [draft id] = uid } in the Drafts
-- mailbox, kept beside the account so a draft's old copy is found after a
-- restart. Not `drafts`: names on the disk are one whatever their case,
-- and that is the Drafts mailbox's folder.
local function drafts_kept(a)
  local t = fs.read(a.dir .. "/drafts.kept")

  return type(t) == "table" and t or {}
end

-- A message in a mailbox, gone for good: marked deleted and expunged.
local function erase(s, b, uids)
  local _, why = await(s:select(b.name))

  if why then return nil, why end

  await(s:flag(uids, "deleted", true))
  return await(s:expunge(uids))
end

--
-- **What the composer leaves for the server** (M6): a copy of what was
-- sent put in Sent, where the server does not keep one itself; a draft's
-- copy put in Drafts in place of the one before it; a draft's copy taken
-- away once it is sent or thrown away.
--
local function do_written(a, s, op)
  local drafts = box_for(a, "drafts")

  if op.type == "append" then
    local b = box_for(a, op.use)

    if b then
      local r = s:append(b.name, op.path, op.flags)

      await(r)
      note(a, ("kept %s in %s: %s"):format(op.id or "a message", b.title, r.ok and "done" or tostring(r.why)))
    else
      note(a, ("no %s mailbox to keep %s in"):format(op.use, op.id or "a message"))
    end

    for _, path in ipairs(op.remove or {}) do files.remove(path) end
  elseif op.type == "draft" and drafts then
    local kept = drafts_kept(a)
    local r = s:append(drafts.name, op.path, { "draft", "seen" })
    local result = await(r)

    note(a, ("draft %s kept in %s: %s"):format(op.id, drafts.title, r.ok and "done" or tostring(r.why)))

    if r.ok then
      local old = kept[op.id]

      kept[op.id] = result and result.uid or nil
      fs.write(a.dir .. "/drafts.kept", kept)
      if old then erase(s, drafts, { old }) end
    end

    files.remove(op.path)
  elseif op.type == "undraft" and drafts then
    local kept = drafts_kept(a)
    local uids = {}

    -- Its copy from this machine, and the one from another it took the
    -- place of: both go.
    if op.id and kept[op.id] then uids[#uids + 1] = kept[op.id] end
    if op.uid and op.mailbox == drafts.name and op.uid ~= uids[1] then uids[#uids + 1] = op.uid end

    if #uids > 0 then
      erase(s, drafts, uids)
      forget_files(a, drafts, uids)
      note(a, ("draft %s taken from %s"):format(op.id or imap.set(uids), drafts.title))
    end

    if op.id then kept[op.id] = nil end
    fs.write(a.dir .. "/drafts.kept", kept)
  end
end

local function do_ops(a, s)
  while #a.ops > 0 do
    local op = table.remove(a.ops, 1)
    local b = op.mailbox and box_named(a, op.mailbox)

    if op.type == "append" or op.type == "draft" or op.type == "undraft" then
      do_written(a, s, op)
    elseif b then
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

-- The account's password, from the keyring - under its incoming server,
-- IMAP or POP3, which is the one Add Account keeps; the same signs in to
-- send (an app password for Gmail is one password for both).
local function password_of(a)
  local acc = a.account
  local service = acc.pop3 and ("pop3://%s:%d"):format(acc.pop3.host, acc.pop3.port or 995)
                  or ("imap://%s:%d"):format(acc.imap.host, acc.imap.port or 993)
  local password, why = fs.mail_password(service, a.address)

  if not password then return nil, "no password kept for " .. service .. ": " .. tostring(why) end

  return password
end

-- An authority of the account's own, a test's.
local function anchors_of(a)
  if not a.account.certificate then return nil end

  local der = fs.read(a.account.certificate)

  return type(der) == "string" and { der } or nil
end

local function session_of(a)
  local acc = a.account
  local password, why = password_of(a)

  if not password then return nil, why end

  local anchors = anchors_of(a)

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
-- A POP3 account (`docs/mail.md`, *POP3*): signed in, what is new fetched,
-- what was deleted here deleted there, signed out, and again in two minutes.
--------------------------------------------------------------------------

-- Its two folders, both kept here: what arrives, and what was sent.
local POP_BOXES = { { name = "INBOX", title = "INBOX", use = "inbox", selectable = true },
                    { name = "Sent", title = "Sent", use = "sent", selectable = true } }

-- What a folder of a POP3 account keeps of itself: `ids`, each message the
-- server has by its unique id - the number it is kept under here, or false
-- for one that is not (older than the first look, or deleted here) - and
-- `next`, the number the next message kept is given.
local function pop_state(folder)
  local t = fs.read(folder .. "/state")

  if type(t) ~= "table" then return nil end

  t.ids = type(t.ids) == "table" and t.ids or {}
  t.next = math.tointeger(t.next) or 1
  return t
end

-- A message put into one of the account's own folders, under the next
-- number, its facts worked out: what was sent, kept in Sent. The file is
-- moved, not copied; its path here, or nil.
function keep_here(a, box, from, flags)
  if not box then return nil end

  local folder = folder_of(a, box)

  files.make_folder(folder)

  local state = pop_state(folder) or { ids = {}, next = 1 }
  local uid = state.next
  local path = ("%s/%d.eml"):format(folder, uid)

  if not files.move(from, path) then
    note(a, "could not keep " .. from .. " in " .. box.title)
    return nil
  end

  state.next = uid + 1
  fs.write(folder .. "/state", state)

  local f = facts(path, uid, flags or {})

  if f then fs.setattr(path, f) end

  counted(a, box, folder)
  publish()
  return path
end

-- The Inbox brought up to date: what the window deleted, deleted on the
-- server; what is new, fetched. Nothing on the server is a reason to take a
-- message away from here: the copy here is the account's mail.
local function pop_sync(a, s, list, quiet)
  local box = POP_BOXES[1]
  local folder = folder_of(a, box)
  local state = pop_state(folder)
  local first = state == nil

  files.make_folder(folder)
  state = state or { ids = {}, next = 1 }

  -- Deleted here: the server's copy marked, to go at QUIT.
  local by_uid, deleted = {}, 0

  for id, uid in pairs(state.ids) do
    if uid then by_uid[uid] = id end
  end

  while #a.ops > 0 do
    local op = table.remove(a.ops, 1)

    if op.type == "delete" and op.mailbox == box.name then
      for _, uid in ipairs(op.uids) do
        local id = by_uid[uid]

        for _, m in ipairs(list) do
          if id and m.id == id then
            local _, why = await(s:delete(m.n))

            if why then return nil, why end

            deleted = deleted + 1
          end
        end

        if id then state.ids[id] = false end
        files.remove(("%s/%d.eml"):format(folder, uid))
      end
    elseif op.type == "delete" and op.mailbox == "Sent" then
      forget_files(a, POP_BOXES[2], op.uids)
      counted(a, POP_BOXES[2], folder_of(a, POP_BOXES[2]))
    elseif MOVES[op.type] then
      note(a, ("%s in %s: a POP3 account has nowhere to put them"):format(op.type, op.mailbox or "?"))
    end
    -- A flag is the window's, on the file here; the server has none.
  end

  -- What the server has that is not known here, oldest first; on a first
  -- look, only the newest hundred, the rest known and left there.
  local new = {}

  for _, m in ipairs(list) do
    if state.ids[m.id] == nil then new[#new + 1] = m end
  end

  if first and #new > FIRST_KEEP then
    for i = 1, #new - FIRST_KEEP do state.ids[new[i].id] = false end
    table.move(new, #new - FIRST_KEEP + 1, #new, 1)
    for i = #new, FIRST_KEEP + 1, -1 do new[i] = nil end
  end

  local why, fetched = nil, 0

  for i, m in ipairs(new) do
    local uid = state.next
    local path = ("%s/%d.eml"):format(folder, uid)
    local got, oops = await(s:fetch(m.n, path, m.size))

    if not got then
      why = oops
      note(a, ("message %s not fetched: %s"):format(m.id, tostring(oops)))
      break
    end

    state.next = uid + 1
    state.ids[m.id] = uid
    fetched = fetched + 1

    local f = facts(path, uid, {})

    if f then fs.setattr(path, f) end
    if not first and not quiet then a.arrived[#a.arrived + 1] = { path = path, facts = f or {} } end

    -- A long first look is seen arriving, ten at a time, and what it has
    -- kept is remembered as it goes.
    if i % 10 == 0 then
      fs.write(folder .. "/state", state)
      counted(a, box, folder)
      publish()
    end
  end

  -- What the server no longer has is no longer asked about.
  local present = {}

  for _, m in ipairs(list) do present[m.id] = true end

  for id in pairs(state.ids) do
    if not present[id] then state.ids[id] = nil end
  end

  fs.write(folder .. "/state", state)

  for _, b in ipairs(POP_BOXES) do
    redo_facts(a, b, folder_of(a, b))
    counted(a, b, folder_of(a, b))
  end

  if not first and fetched + deleted > 0 then
    note(a, ("INBOX: %d new, %d deleted on the server"):format(fetched, deleted))
  end

  if why then return nil, why end

  return true
end

function run_pop_account(a)
  local acc = a.account

  a.list = POP_BOXES
  a.inbox_folder = folder_of(a, POP_BOXES[1])
  fs.write(a.dir .. "/mailboxes", POP_BOXES)

  for _, b in ipairs(POP_BOXES) do
    files.make_folder(folder_of(a, b))
    counted(a, b, folder_of(a, b))
  end

  publish()

  local first = true

  while true do
    a.wake = false
    set_state(a, first and "signing in" or "syncing")

    local password, why = password_of(a)
    local s, can

    if password then
      s, why = pop3.open{ host = acc.pop3.host, port = acc.pop3.port or 995,
                          tls = acc.pop3.tls ~= false, name = acc.tls_name,
                          anchors = anchors_of(a), user = acc.user or a.address,
                          password = password }
      password = nil
    end

    if s then
      a.session = s
      can, why = await(s.ready)
    end

    local ok = can ~= nil

    if ok then
      if first then note(a, "signed in") end
      set_state(a, "syncing")

      local list

      list, why = await(s:list())
      ok = list ~= nil

      if ok then ok, why = pop_sync(a, s, list, false) end
      if ok then await(s:quit()) end
    else
      note(a, "could not sign in: " .. tostring(why))
    end

    if s and not s.broken then s:fail(ok and "signed out" or "given up") end
    a.session = nil

    if ok then
      if first then
        note(a, ("%d messages kept, %d unseen"):format(a.messages, a.unseen))
        first = false
      end

      say_arrived(a)
      set_state(a, "idle")
    else
      set_state(a, "error", tostring(why))
    end

    if ONCE then return end

    -- Until the next look, or until the window asks for one or deletes.
    local until_ = seconds_from_now(ok and POLL_SECONDS or RETRY_SECONDS)

    while sys.ticks() < until_ and not a.wake and #a.ops == 0 do coroutine.yield() end
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
  local incoming = type(account) == "table" and (account.pop3 or account.imap)

  if type(incoming) ~= "table" or not incoming.host then return nil end

  local a = { address = account.address or name, account = account, dir = dir,
              state = "waiting", arrived = {}, messages = 0, unseen = 0,
              counts = {}, wanted = {}, ops = {}, pop = account.pop3 ~= nil }
  local run = a.pop and run_pop_account or run_account

  a.co = coroutine.create(function() return run(a) end)

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

function account_named(address)
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

--------------------------------------------------------------------------
-- Sending (M6): a message from the Outbox, by SMTP, beside the accounts'
-- IMAP and stepped by the same loop.
--------------------------------------------------------------------------

-- An id as the window makes them: letters and digits, nothing that walks
-- out of a folder.
local function id_ok(id)
  return type(id) == "string" and #id > 0 and #id <= 64 and id:match("^[%w%-]+$") ~= nil
end

local function send_failed(o, why)
  o.state, o.why, o.job = "failed", why, nil
  print(("maild: not sent %s: %s"):format(o.id, tostring(why)))
  publish()
end

-- A message in the Outbox, sent: its envelope read, the account's server
-- spoken to. Nothing here waits; the loop steps the job.
local function start_send(id)
  if not id_ok(id) then return false, "no such message" end

  local o = outbox[id]

  if o and o.state == "sending" then return true end

  local env = fs.read(OUTBOX .. "/" .. id .. ".send")
  local path = OUTBOX .. "/" .. id .. ".eml"

  if type(env) ~= "table" or not fs.getattr(path) then return false, "no such message in the Outbox" end

  o = { id = id, account = tostring(env.account or ""), subject = tostring(env.subject or ""),
        state = "sending", env = env, path = path }
  outbox[id] = o

  local a = account_named(o.account)
  local rcpt = {}

  for i, who in ipairs(type(env.rcpt) == "table" and env.rcpt or {}) do
    if i <= 500 and type(who) == "string" and who:match("^[^%s<>]+@[^%s<>]+$") then rcpt[#rcpt + 1] = who end
  end

  if not a then send_failed(o, "no account " .. o.account) return true end
  if #rcpt == 0 then send_failed(o, "it is to nobody") return true end

  local acc = a.account
  local password, why = password_of(a)

  if not password then send_failed(o, why) return true end

  if not (acc.smtp and acc.smtp.host) then send_failed(o, "the account has no server to send by") return true end

  local job, oops = smtp.send{ host = acc.smtp.host, port = acc.smtp.port or 587,
                               security = acc.smtp.security, name = acc.smtp_tls_name or acc.tls_name,
                               anchors = anchors_of(a), user = acc.user or a.address,
                               password = password, from = a.address, to = rcpt, path = path }

  password = nil

  if not job then send_failed(o, oops) return true end

  o.job, o.a = job, a
  print(("maild: sending %s to %d"):format(id, #rcpt))
  publish()
  return true
end

-- Each message being sent, stepped; one done is kept in Sent where the
-- server does not keep it, and its draft's copy taken away.
local function step_sending(conns)
  for id, o in pairs(outbox) do
    local job = o.job

    if job and not job.done then
      job:step()
      if not job.done and job.stream and job.stream.conn then conns[#conns + 1] = job.stream.conn end
    end

    if job and job.done then
      if job.ok then
        local a, env = o.a, o.env
        local remove = { o.path, OUTBOX .. "/" .. id .. ".send" }

        print(("maild: sent %s"):format(id))
        outbox[id] = nil

        -- Gmail keeps what it sent by itself; any other server is given
        -- it, and a POP3 account, which has nowhere there, keeps it here.
        if a.pop then
          keep_here(a, box_for(a, "sent"), o.path, { seen = true })
          for _, p in ipairs(remove) do files.remove(p) end
        elseif a.account.kind ~= "google" then
          a.ops[#a.ops + 1] = { type = "append", use = "sent", path = o.path, flags = { "seen" },
                                id = id, remove = remove }
        else
          for _, p in ipairs(remove) do files.remove(p) end
        end

        if id_ok(env.draft) or env.replaces then
          local r = type(env.replaces) == "table" and env.replaces or {}

          a.ops[#a.ops + 1] = { type = "undraft", id = id_ok(env.draft) and env.draft or nil,
                                mailbox = type(r.mailbox) == "string" and r.mailbox or nil,
                                uid = math.tointeger(r.uid) }
        end

        a.wake = true
        publish()
      else
        send_failed(o, job.why or "the server would not take it")
      end
    end
  end
end

-- What was left in the Outbox when `maild` last stopped, sent now.
local function send_left()
  for _, name in ipairs(fs.list(OUTBOX) or {}) do
    local id = tostring(name):match("^(.+)%.send$")

    if id and not outbox[id] then start_send(id) end
  end
end

send_left()

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
      send_left()
      reply = { ok = true }
    elseif t == "send" then
      local ok, why = start_send(req.name)

      reply = { ok = ok, why = why }
    elseif t == "draft" and a and id_ok(req.name) then
      local path = DRAFTS .. "/" .. req.name .. ".eml"

      if fs.getattr(path) then
        a.ops[#a.ops + 1] = { type = "draft", id = req.name, path = path }
        reply = { ok = true }
      else
        reply = { ok = false, why = "no such draft" }
      end
    elseif t == "undraft" and a then
      local r = type(req.replaces) == "table" and req.replaces or {}

      a.ops[#a.ops + 1] = { type = "undraft", id = id_ok(req.name) and req.name or nil,
                            mailbox = type(r.mailbox) == "string" and r.mailbox or nil,
                            uid = math.tointeger(r.uid) }
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
      -- A POP3 account's folders are both here; nothing to look at there.
      if not a.pop then
        a.wanted[req.mailbox] = true
        a.wake = true
      end

      reply = { ok = true }
    elseif t == "flag" and a and a.pop and FLAGGABLE[req.flag] then
      -- Kept on the file by the window; POP3 has no flags to tell.
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
          return (a.pop and run_pop_account or run_account)(a)
        end)
      end
    end

    if coroutine.status(a.co) ~= "dead" then alive = alive + 1 end
  end

  step_sending(conns)

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
