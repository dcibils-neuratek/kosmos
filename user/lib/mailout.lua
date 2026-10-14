-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **A message into the Outbox**, built and handed to `maild` to send
-- (`docs/mail.md` M6; `docs/automations.md` A2).
--
--   local mailout = use("/Kosmos/Libraries/mailout.lua")
--   local id, why = mailout.send{ to = { "ana@example.com" }, subject = "Hello",
--                                 text = "...", files = { "/Home/Documents/a.pdf" },
--                                 account = "me@example.com" }   -- optional
--   mailout.accounts()                          -> { { address =, name = }, ... }
--   mailout.write(path, spec, files)            -> true, or nil and why
--   mailout.post(id, envelope)                  -> maild has it, or false and why
--   mailout.new_id()                            -> twenty hex digits
--
-- **One door to the Outbox.** The composer wrote it, and an automation that
-- sends a mail has to write the same two files the same way - so the
-- writing came out of `mailcompose.lua` into this, which both use, rather
-- than being copied into `automate.lua` (`CLAUDE.md`: a second copy is a
-- defect). What a message is - its words, its files, its envelope - is the
-- caller's; how it becomes `<id>.eml` and `<id>.send` and reaches `maild`
-- is this.
--
-- **Sent by `maild`**: written here and `maild` asked, so it goes when the
-- caller has gone. With `maild` not running it stays in the Outbox, and
-- `maild` sends what it finds there when it starts.

local mailout = {}

local files = use("/Kosmos/Libraries/files.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local filetypes = use("/Kosmos/Libraries/filetypes.lua")
local mailkit = use("/Kosmos/Kits/mail")

mailout.HOME = "/Home/Mail"
mailout.OUTBOX = mailout.HOME .. "/Outbox"

-- A message's own name: twenty hex digits from the Crypto Kit's generator,
-- or the counter where there is none.
function mailout.new_id()
  local ok, crypto = pcall(use, "/Kosmos/Kits/crypto")
  local bytes = ok and crypto and crypto.random(10)

  if not bytes then
    bytes = tostring(sys.ticks()) .. tostring(math.random(1, 1 << 30))
  end

  return (bytes:gsub(".", function(c) return ("%02x"):format(c:byte()) end)):sub(1, 20)
end

-- The accounts a message may be sent from: each folder in `/Home/Mail`
-- with an `account` that says how to send.
function mailout.accounts()
  local out = {}

  for _, name in ipairs(fs.list(mailout.HOME) or {}) do
    local a = fs.read(mailout.HOME .. "/" .. name .. "/account")

    if type(a) == "table" and a.smtp then
      out[#out + 1] = { address = a.address or name, name = a.name }
    end
  end

  table.sort(out, function(x, y) return x.address < y.address end)
  return out
end

--
-- The message at `path`, as `mailkit.build` writes it: `spec` its head and
-- text - `from`, `to`, `cc` (each `{ address =, name = }`), `subject`,
-- `text`, `id`, `in_reply_to`, `references` - and `list` the files it
-- carries, each `{ path =, name = }`, read into regions and never into a
-- Lua string. The date and zone are now's.
--
function mailout.write(path, spec, list)
  local now = clock.now()
  local held, carried = {}, 0

  spec.date = now and now.epoch or 0
  spec.zone = clock.offset()
  spec.attachments = {}

  -- Each file into a region for the kit to read; base64 is a third more.
  for _, f in ipairs(list or {}) do
    local fr, size = regions.read_whole(f.path)

    if not fr then
      for _, h in ipairs(held) do regions.free(h) end
      return nil, tostring(f.name) .. " could not be read"
    end

    held[#held + 1] = fr
    spec.attachments[#spec.attachments + 1] = { name = f.name, type = filetypes.mime_type(f.name),
                                                at = fr.at, bytes = size }
    carried = carried + size
  end

  local room = #tostring(spec.text or "") * 3 + carried * 4 // 3 + carried // 38 + 16384
               + #(list or {}) * 1024
  local r = regions.make(room)
  local n, need, ok, why

  if r then
    n, need = mailkit.build(spec, r.at, r.size)

    if not n then
      regions.free(r)
      r = regions.make(need)
      n = r and mailkit.build(spec, r.at, r.size)
    end
  end

  if r and n then
    ok, why = regions.write_file(path, r, n)
  else
    ok, why = nil, "no memory to write it in"
  end

  if r then regions.free(r) end
  for _, h in ipairs(held) do regions.free(h) end

  return ok, why
end

--
-- Its envelope beside it - `account`, `rcpt` (everyone it goes to, the Bcc
-- among them), `subject`, and a composer's `draft` and `replaces` - and
-- `maild` asked to send `<id>`. True when `maild` has it; false and why when
-- it is not running, and the message waits in the Outbox for it.
--
function mailout.post(id, envelope)
  fs.write(mailout.OUTBOX .. "/" .. id .. ".send", envelope)

  local ok, reply = pcall(fs.send, "/Running/maild", { type = "send", name = id })

  if ok and type(reply) == "table" and reply.ok then return true end

  return false, ok and type(reply) == "table" and reply.why or "maild is not running"
end

local function addresses_of(list)
  local out = {}

  if type(list) == "string" then list = { list } end

  for _, a in ipairs(list or {}) do
    if type(a) == "table" then
      out[#out + 1] = { address = tostring(a.address), name = a.name }
    else
      out[#out + 1] = { address = tostring(a) }
    end
  end

  return out
end

--
-- A message from words, as a program or an automation says it: `to`, `cc`
-- and `bcc` addresses (a string or a list), `subject`, `text`, `files` (paths)
-- and `account` (an address; the first that can send when not given).
-- Returns its id and whether `maild` has it yet, or nil and why.
--
function mailout.send(m)
  m = m or {}

  local to, cc, bcc = addresses_of(m.to), addresses_of(m.cc), addresses_of(m.bcc)

  if #to + #cc + #bcc == 0 then return nil, "who is it to?" end

  for _, list in ipairs({ to, cc, bcc }) do
    for _, a in ipairs(list) do
      if not a.address:match("^[^@%s]+@[^@%s]+$") then
        return nil, ("\u{201c}%s\u{201d} is not an address"):format(a.address)
      end
    end
  end

  local account

  for _, a in ipairs(mailout.accounts()) do
    if m.account == nil or a.address == m.account then account = a break end
  end

  if not account then
    return nil, m.account and ("no account " .. tostring(m.account) .. " that sends")
                or "no mail account that sends - add one in Mail"
  end

  local list = {}

  for _, p in ipairs(type(m.files) == "string" and { m.files } or (m.files or {})) do
    list[#list + 1] = { path = p, name = tostring(p):match("([^/]+)$") or p }
  end

  local id = mailout.new_id()
  local domain = account.address:match("@(.+)$") or "kosmos"

  files.make_folder(mailout.OUTBOX)

  local ok, why = mailout.write(mailout.OUTBOX .. "/" .. id .. ".eml",
                                { from = { name = account.name or "", address = account.address },
                                  to = to, cc = cc, subject = tostring(m.subject or ""),
                                  text = tostring(m.text or ""), id = id .. "@" .. domain }, list)

  if not ok then return nil, "not written: " .. tostring(why) end

  local rcpt = {}

  for _, l in ipairs({ to, cc, bcc }) do
    for _, a in ipairs(l) do rcpt[#rcpt + 1] = a.address end
  end

  local asked = mailout.post(id, { account = account.address, rcpt = rcpt,
                                   subject = tostring(m.subject or "") })

  return id, asked
end

return mailout
