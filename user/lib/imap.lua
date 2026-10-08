-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- IMAP4rev1 (RFC 3501), a client: a mail server's mailboxes, what changed
-- in one, a message fetched into a file, flags, moves and appends, and IDLE.
--
--   local imap = use("/Kosmos/Libraries/imap.lua")
--   local s, why = imap.open{ host =, port = 993, user =, password =,
--                             tls = true, name =, anchors = }
--   s.ready                          a request: signed in, and what it can do
--   s:mailboxes()                    -> { { name =, title =, use =, attrs = } }
--   s:status(name)                   -> { messages =, unseen =, uidnext = }
--   s:select(name)                   -> { exists =, uidvalidity =, uidnext =,
--                                         highestmodseq =, writable = }
--   s:changes(since)                 -> { new = { { uid =, flags =, size = } },
--                                         changed = {...}, present = { uids },
--                                         reset =, uidvalidity =, uid_high =,
--                                         modseq = } - `since` is the last
--                                         answer's three numbers, or nil
--   s:fetch(uid, path)               -> { uid =, flags =, bytes = }: the whole
--                                         message, into the file at `path`
--   s:flag(uids, flag, on)           flag "seen", "flagged", "answered",
--                                    "deleted", "draft", or a keyword
--   s:move(uids, to)  s:append(name, path, flags)  s:logout()
--   s:idle()  s:done()               held open for news; `s.news` is set
--                                    when the mailbox changes, and `done`
--                                    ends it
--
--   s:step()                         what has arrived, read and answered
--   s:wait(request[, seconds])       -> its result, or nil and why
--
-- Every call above but `step` and `wait` returns a **request**, `{ done,
-- ok, result, why }`, answered as the conversation goes; nothing here
-- waits on the network. `maild` steps every account's session when
-- `fs.poll` says its connection has news (`s.stream.conn`); a program at
-- the prompt, or a test, `wait`s on one.
--
-- **Commands go one at a time**, each sent when the one before it is
-- answered. IMAP allows more in flight, and the price of not taking it is a
-- round trip a command; what it buys is that every response belongs to the
-- one command waiting, which is what lets a message's bytes go straight to
-- its file: **a message is never a Lua string here**. Its size comes first
-- (`{18731}`), so a region of that size is claimed, each read is copied in
-- as it arrives, and the region is written to the file whole
-- (`docs/mail.md`, "an IMAP literal is copied from the connection to a file
-- as it arrives").
--
-- Mailbox names are kept as the server spells them, in IMAP's modified
-- UTF-7, so they go back exactly; `title` is the same name in UTF-8, for a
-- person.

local imap = {}

local netstream = use("/Kosmos/Libraries/netstream.lua")
local regions = use("/Kosmos/Libraries/regions.lua")

-- How long a request may go with nothing heard before the session is given
-- up on, in seconds; `wait`'s own deadline is the caller's.
imap.QUIET_SECONDS = 60

-- What a mailbox is for, from its SPECIAL-USE attribute (RFC 6154) - and
-- INBOX, which is the one every server has by name.
local USES = {
  ["\\sent"] = "sent", ["\\drafts"] = "drafts", ["\\trash"] = "trash",
  ["\\junk"] = "junk", ["\\archive"] = "archive", ["\\all"] = "all",
  ["\\flagged"] = "flagged", ["\\important"] = "important",
}

local FLAGS = {
  seen = "\\Seen", flagged = "\\Flagged", answered = "\\Answered",
  deleted = "\\Deleted", draft = "\\Draft",
}

--------------------------------------------------------------------------
-- Words on the wire.
--------------------------------------------------------------------------

-- A string as IMAP quotes it; one that cannot be quoted - a line's end, a
-- byte past ASCII - goes as a literal (`{ literal = s }`), which the sender
-- sends after the server says to go on.
local function astring(s)
  s = tostring(s)

  if s:find("[\r\n%z\128-\255]") then return { literal = s } end

  return '"' .. s:gsub('[\\"]', "\\%0") .. '"'
end

-- UIDs as a set: { 3, 4, 5, 9 } -> "3:5,9".
function imap.set(uids)
  if type(uids) ~= "table" then return tostring(math.tointeger(uids) or uids) end

  local sorted = {}

  for _, u in ipairs(uids) do sorted[#sorted + 1] = math.tointeger(u) end

  table.sort(sorted)

  local out, i = {}, 1

  while i <= #sorted do
    local j = i

    while j < #sorted and sorted[j + 1] <= sorted[j] + 1 do j = j + 1 end

    out[#out + 1] = (j > i) and (sorted[i] .. ":" .. sorted[j]) or tostring(sorted[i])
    i = j + 1
  end

  return table.concat(out, ",")
end

--
-- A mailbox's name in modified UTF-7 (RFC 3501 5.1.3) made UTF-8: `&` and
-- base64 of UTF-16, with `,` for `/`, to the next `-`; `&-` is `&` itself.
--
function imap.title(name)
  local compress = use("/Kosmos/Kits/compress")

  return (tostring(name):gsub("&([^-]*)-", function(b64)
    if b64 == "" then return "&" end

    local text = b64:gsub(",", "/")
    local ok, bytes = pcall(compress.unbase64, text .. ("="):rep((4 - #text % 4) % 4))

    if not ok then return "&" .. b64 .. "-" end

    local out, i = {}, 1

    while i + 1 <= #bytes do
      local u = bytes:byte(i) * 256 + bytes:byte(i + 1)

      i = i + 2

      if u >= 0xD800 and u <= 0xDBFF and i + 1 <= #bytes then
        local lo = bytes:byte(i) * 256 + bytes:byte(i + 1)

        i = i + 2
        u = 0x10000 + (u - 0xD800) * 0x400 + (lo - 0xDC00)
      end

      out[#out + 1] = utf8.char(u)
    end

    return table.concat(out)
  end))
end

--
-- A response's words: atoms, quoted strings, lists in parentheses, and the
-- literals that came out of line, marked in the text as `\1` and a number.
-- An atom keeps what is in its brackets whole, spaces and all -
-- `BODY[HEADER.FIELDS (FROM)]` is one word. NIL is `false`.
--
local function words(text, literals)
  local i, n = 1, #text

  local function space()
    while text:byte(i) == 32 do i = i + 1 end
  end

  local function value()
    space()

    local c = text:sub(i, i)

    if c == "(" then
      local list = {}

      i = i + 1

      while true do
        space()

        if i > n then return list end

        if text:sub(i, i) == ")" then
          i = i + 1
          return list
        end

        list[#list + 1] = value()
      end
    elseif c == '"' then
      local out = {}

      i = i + 1

      while i <= n do
        local d = text:sub(i, i)

        if d == "\\" then
          out[#out + 1] = text:sub(i + 1, i + 1)
          i = i + 2
        elseif d == '"' then
          i = i + 1
          break
        else
          out[#out + 1] = d
          i = i + 1
        end
      end

      return table.concat(out)
    elseif c == "\1" then
      local j = text:find("\1", i + 1, true) or n
      local k = tonumber(text:sub(i + 1, j - 1))

      i = j + 1
      return literals[k]
    end

    local start, depth = i, 0

    while i <= n do
      local d = text:sub(i, i)

      if d == "[" then
        depth = depth + 1
      elseif d == "]" then
        depth = depth - 1
      elseif depth <= 0 and (d == " " or d == "(" or d == ")") then
        break
      end

      i = i + 1
    end

    if i == start then
      i = i + 1                     -- a `)` with no list open: passed over
      return ""
    end

    local atom = text:sub(start, i - 1)

    if atom == "NIL" then return false end

    return atom
  end

  local out = {}

  while true do
    space()

    if i > n then return out end

    out[#out + 1] = value()
  end
end

-- A FETCH's list of names and values as a table by upper-case name.
local function fetched(list)
  local out = {}

  for k = 1, #list - 1, 2 do
    if type(list[k]) == "string" then out[list[k]:upper()] = list[k + 1] end
  end

  return out
end

-- Flags as written, `(\Seen $Label)`, as a set by lower-case name without
-- the backslash: `{ seen = true, ["$label"] = true }`.
local function flagset(list)
  local out = {}

  for _, f in ipairs(type(list) == "table" and list or {}) do
    if type(f) == "string" then out[f:gsub("^\\", ""):lower()] = true end
  end

  return out
end

--------------------------------------------------------------------------
-- The session.
--------------------------------------------------------------------------

local S = {}
S.__index = S

local function request(s, parts, finish, opts)
  local r = { done = false, parts = parts, finish = finish }

  for k, v in pairs(opts or {}) do r[k] = v end

  s.queue[#s.queue + 1] = r
  s:send_next()

  return r
end

local function settle(r, ok, result, why)
  if r.done then return end

  r.done, r.ok, r.result, r.why = true, ok, result, why
  if r.region then regions.free(r.region) r.region = nil end
end

--
-- Everything still waiting, failed with one reason: the connection went,
-- or nothing was heard for too long.
--
function S:fail(why)
  self.broken = self.broken or why

  if self.active then settle(self.active, false, nil, why) end

  for _, r in ipairs(self.queue) do settle(r, false, nil, why) end

  self.active, self.queue = nil, {}
  self.stream:close()
  self.stream.conn:close()
end

function S:write(text)
  self.out[#self.out + 1] = text
end

--
-- The next command, when none is in flight and the greeting has come: its
-- words up to its first literal, and the rest when the server says go on.
--
function S:send_next()
  if self.active or not self.greeted or self.broken then return end

  local r = table.remove(self.queue, 1)

  if not r then return end

  if r.needs and not r.needs.ok then
    settle(r, false, nil, r.needs.why or "an earlier step failed")
    return self:send_next()
  end

  self.tags = self.tags + 1
  r.tag = ("k%d"):format(self.tags)
  r.at = 1
  self.active = r
  self.heard = sys.ticks()
  self:send_part(r, r.tag .. " ")
end

-- `r.parts` from `r.at`: strings as they are, to a literal, whose size is
-- said and whose bytes wait for the server's `+`.
function S:send_part(r, prefix)
  local line = { prefix }

  while r.at <= #r.parts do
    local p = r.parts[r.at]

    r.at = r.at + 1

    if type(p) == "table" then
      line[#line + 1] = ("{%d}\r\n"):format(#p.literal)
      r.literal = p.literal
      self:write(table.concat(line))
      return
    end

    line[#line + 1] = p
  end

  line[#line + 1] = "\r\n"
  self:write(table.concat(line))
end

-- The literal promised, and the rest of the command after it.
function S:go_on(text)
  local r = self.active

  if not r then return end

  if r.literal then
    local l = r.literal

    r.literal = nil
    self:write(l)
    self:send_part(r, "")
  elseif r.continue then
    r.continue(self, text)
  end
end

--
-- A response's codes, `[UIDVALIDITY 3857529045]` in an OK's words, into
-- what is known of the mailbox and the server.
--
function S:codes(text)
  local code = text:match("^%[(.-)%]")

  if not code then return end

  local name, rest = code:match("^(%S+)%s*(.*)$")

  name = (name or ""):upper()

  local box = self.mailbox

  if name == "CAPABILITY" then
    self:capabilities(rest)
  elseif name == "UIDVALIDITY" then
    box.uidvalidity = tonumber(rest)
  elseif name == "UIDNEXT" then
    box.uidnext = tonumber(rest)
  elseif name == "HIGHESTMODSEQ" then
    box.highestmodseq = tonumber(rest)
  elseif name == "NOMODSEQ" then
    box.highestmodseq = nil
  elseif name == "READ-WRITE" then
    box.writable = true
  elseif name == "READ-ONLY" then
    box.writable = false
  elseif name == "APPENDUID" or name == "COPYUID" then
    self.last_code = { name = name, words = rest }
  end
end

function S:capabilities(text)
  self.can = {}

  for word in tostring(text):gmatch("%S+") do self.can[word:upper()] = true end
end

--
-- One whole response: a continuation, something the server says of its
-- own accord (`*`), or the answer that ends the command in flight.
--
function S:response(text, literals)
  if text:sub(1, 1) == "+" then
    return self:go_on(text:sub(3))
  end

  local r = self.active

  if text:sub(1, 2) == "* " then
    local rest = text:sub(3)
    local first, after = rest:match("^(%S+)%s*(.*)$")
    local word = (first or ""):upper()

    if not self.greeted then
      if word == "OK" or word == "PREAUTH" then
        self.greeted = true
        self:codes(after)
        return self:send_next()
      end

      return self:fail("the server said: " .. rest)
    end

    if word == "OK" or word == "NO" or word == "BAD" then
      return self:codes(after)
    elseif word == "BYE" then
      self.bye = after
      return
    elseif word == "CAPABILITY" then
      return self:capabilities(after)
    elseif word == "FLAGS" then
      self.mailbox.flags = flagset(words(after, literals)[1])
      return
    end

    local w = words(rest, literals)
    local number = tonumber(w[1])
    local what = type(w[2]) == "string" and w[2]:upper() or ""

    if word == "LIST" or word == "LSUB" then
      if r and r.list then
        local attrs, use = {}, nil

        for _, a in ipairs(type(w[2]) == "table" and w[2] or {}) do
          attrs[#attrs + 1] = a
          use = use or USES[tostring(a):lower()]
        end

        local name = tostring(w[4] or "")

        if name:upper() == "INBOX" then use = "inbox" end

        r.list[#r.list + 1] = { name = name, title = imap.title(name), use = use,
                                attrs = attrs, delimiter = w[3] or nil,
                                selectable = not flagset(w[2])["noselect"] }
      end
    elseif word == "STATUS" then
      if r and r.status then
        local items = w[3] or {}

        for k = 1, #items - 1, 2 do
          r.status[tostring(items[k]):lower()] = tonumber(items[k + 1])
        end
      end
    elseif word == "SEARCH" then
      if r and r.search then
        for k = 2, #w do r.search[#r.search + 1] = tonumber(w[k]) end
      end
    elseif number and what == "EXISTS" then
      self.mailbox.exists = number
      self.news = true
    elseif number and what == "EXPUNGE" then
      self.mailbox.exists = math.max(0, (self.mailbox.exists or 1) - 1)
      self.news = true
    elseif number and what == "FETCH" then
      local f = fetched(type(w[3]) == "table" and w[3] or {})

      if r and r.fetched then
        r.fetched[#r.fetched + 1] = f
      else
        self.news = true
      end
    end

    return
  end

  -- Tagged: the command in flight, answered.
  local tag, status, rest = text:match("^(%S+)%s+(%S+)%s*(.*)$")

  if not r or tag ~= r.tag then return end

  status = (status or ""):upper()
  self:codes(rest)
  self.active = nil

  if status == "OK" then
    local ok, result, why = true, nil, nil

    if r.finish then ok, result, why = r.finish(self, r, rest) end

    if ok == false then
      settle(r, false, nil, why)
    else
      settle(r, true, result)
    end
  else
    settle(r, false, nil, ("the server said: %s"):format(rest ~= "" and rest or status))
  end

  self:send_next()
end

--
-- A literal is a message when it is the BODY[] of a fetch with a file to
-- go to: then it goes into a region of its size, never a string.
--
function S:literal_sink(before, size)
  local r = self.active

  if r and r.into and before:upper():match("BODY%[%]%s*$") then
    local region, why = regions.make(math.max(size, 1))

    if not region then
      self:fail(why)
      return nil
    end

    r.region = region
    return { region = region, bytes = size, at = 0 }
  end

  return { parts = {}, bytes = size, at = 0 }
end

--
-- What has arrived, taken apart: lines, and the literals inside them, which
-- a response may have several of. A response is whole at the line's end
-- that is not followed by a literal.
--
function S:digest()
  while not self.broken do
    if self.sink then
      local k = self.sink
      local take = math.min(k.bytes - k.at, #self.inbuf - self.at + 1)

      if take > 0 then
        local piece = self.inbuf:sub(self.at, self.at + take - 1)

        if k.region then
          sys.region_write(k.region.cap, k.at, piece)
        else
          k.parts[#k.parts + 1] = piece
        end

        k.at = k.at + take
        self.at = self.at + take
      end

      if k.at < k.bytes then break end

      local value = k.region and { region = k.region, bytes = k.bytes }
                    or table.concat(k.parts)

      self.literals[#self.literals + 1] = value
      self.pending[#self.pending + 1] = "\1" .. #self.literals .. "\1"
      self.sink = nil
    else
      local e = self.inbuf:find("\r\n", self.at, true)

      if not e then break end

      local line = self.inbuf:sub(self.at, e - 1)

      self.at = e + 2

      local before, size = line:match("^(.-){(%d+)%+?}$")

      if size then
        self.pending[#self.pending + 1] = before
        self.sink = self:literal_sink(table.concat(self.pending), tonumber(size))
      else
        self.pending[#self.pending + 1] = line

        local text, literals = table.concat(self.pending), self.literals

        self.pending, self.literals = {}, {}
        self:response(text, literals)
      end
    end
  end

  -- What was taken is let go, so the buffer holds only what is unread.
  if self.at > 1 then
    self.inbuf = self.inbuf:sub(self.at)
    self.at = 1
  end
end

function S:step()
  if self.broken then return end

  -- What is waiting to go, as much as the connection takes.
  if #self.out > 0 then
    local text = table.concat(self.out)

    self.out = {}

    local n = self.stream:write(text)

    if n < #text then self.out[1] = text:sub(n + 1) end
  end

  self.stream:flush()

  for _ = 1, 256 do
    local got = self.stream:read()

    if not got or got == "" then break end

    self.heard = sys.ticks()
    self.inbuf = self.inbuf .. got
    self:digest()
  end

  local over, why = self.stream:done()

  if over and not self.broken then
    self:digest()
    self:fail(why or (self.bye and ("the server said goodbye: " .. self.bye))
              or "the server closed the connection")
    return
  end

  if (self.active or #self.queue > 0)
     and not (self.active and self.active.idling)
     and sys.ticks() - self.heard >= imap.QUIET_SECONDS * netstream.counter() then
    self:fail(("nothing from the server for %d seconds"):format(imap.QUIET_SECONDS))
  end
end

--
-- A request's result, stepping and waiting on the connection until it is
-- answered or `seconds` (30) have passed.
--
function S:wait(r, seconds)
  local hz = (sys.info() or {}).tick_hz or 250
  local tick = math.max(1, hz // 20)
  local deadline = sys.ticks() + (seconds or 30) * netstream.counter()

  while not r.done do
    self:step()

    if r.done or self.broken then break end

    if sys.ticks() > deadline then
      return nil, "no answer in time"
    end

    self.stream.conn:wait(tick)
  end

  if r.ok then return r.result end

  return nil, r.why or self.broken
end

--------------------------------------------------------------------------
-- The commands.
--------------------------------------------------------------------------

function imap.open(opts)
  local stream, why = netstream.open{
    host = opts.host, port = opts.port or 993, tls = opts.tls ~= false,
    name = opts.name, anchors = opts.anchors, insecure = opts.insecure,
  }

  if not stream then return nil, why end

  local s = setmetatable({
    stream = stream, out = {}, inbuf = "", at = 1, pending = {}, literals = {},
    queue = {}, tags = 0, greeted = false, can = {}, mailbox = {},
    heard = sys.ticks(), news = false,
  }, S)

  s.login = request(s, { "LOGIN ", astring(opts.user or ""), " ",
                         astring(opts.password or "") })
  s.ready = request(s, { "CAPABILITY" }, function(self) return true, self.can end,
                    { needs = s.login })

  return s
end

function S:mailboxes()
  return request(self, { 'LIST "" "*"' }, function(_, r) return true, r.list end,
                 { list = {} })
end

function S:status(name)
  return request(self, { "STATUS ", astring(name), " (MESSAGES UNSEEN UIDNEXT)" },
                 function(_, r) return true, r.status end, { status = {} })
end

function S:select(name)
  self.mailbox = { name = name }

  local condstore = self.can.CONDSTORE and " (CONDSTORE)" or ""

  return request(self, { "SELECT ", astring(name), condstore }, function(s)
    local b = s.mailbox

    return true, { name = b.name, exists = b.exists or 0, uidvalidity = b.uidvalidity,
                   uidnext = b.uidnext, highestmodseq = b.highestmodseq,
                   writable = b.writable ~= false, flags = b.flags }
  end)
end

local function message_of(f)
  return { uid = tonumber(f.UID), flags = flagset(f.FLAGS),
           size = tonumber(f["RFC822.SIZE"]),
           modseq = type(f.MODSEQ) == "table" and tonumber(f.MODSEQ[1]) or nil }
end

--
-- What is new since the last look, what changed, and what is still there -
-- the last from which the caller learns what has gone. `since` is the last
-- answer's `{ uidvalidity, uid_high, modseq }`: a mailbox whose UIDVALIDITY
-- moved is a different mailbox, and everything in it is new (`reset`).
-- With CONDSTORE only what changed since `modseq` is asked for; without it,
-- every message's flags.
--
function S:changes(since)
  local box = self.mailbox
  local reset = not since or since.uidvalidity ~= box.uidvalidity
  local high = reset and 0 or (since.uid_high or 0)
  local result = { new = {}, changed = {}, present = {}, reset = reset }

  local new = request(self, { ("UID FETCH %d:* (UID FLAGS RFC822.SIZE)"):format(high + 1) },
                      function(_, r)
                        for _, f in ipairs(r.fetched) do
                          local m = message_of(f)

                          if m.uid and m.uid > high then result.new[#result.new + 1] = m end
                        end

                        return true
                      end, { fetched = {} })

  if high > 0 then
    local changed = { ("UID FETCH 1:%d (UID FLAGS)"):format(high) }

    if since.modseq and self.can.CONDSTORE then
      changed[1] = changed[1] .. (" (CHANGEDSINCE %d)"):format(since.modseq)
    end

    request(self, changed, function(_, r)
      for _, f in ipairs(r.fetched) do result.changed[#result.changed + 1] = message_of(f) end
      return true
    end, { fetched = {}, needs = new })

    request(self, { ("UID SEARCH UID 1:%d"):format(high) }, function(_, r)
      for _, u in ipairs(r.search) do
        if u <= high then result.present[#result.present + 1] = u end
      end
      return true
    end, { search = {}, needs = new })
  end

  -- The last of them carries the answer, and the mailbox's numbers as they
  -- are now: what the next `changes` is given.
  return request(self, { "NOOP" }, function(s)
    local top = high

    for _, m in ipairs(result.new) do top = math.max(top, m.uid) end

    result.uidvalidity = s.mailbox.uidvalidity
    result.uid_high = top
    result.modseq = s.mailbox.highestmodseq

    return true, result
  end, { needs = new })
end

--
-- A whole message into a file, its bytes straight from the connection to a
-- region and from the region to the file. `BODY.PEEK`, so reading it does
-- not mark it seen; that is the reader's to say.
--
function S:fetch(uid, path)
  return request(self, { ("UID FETCH %s (UID FLAGS BODY.PEEK[])"):format(imap.set(uid)) },
                 function(_, r)
                   for _, f in ipairs(r.fetched) do
                     local body = f["BODY[]"]

                     if tonumber(f.UID) == tonumber(uid) and type(body) == "table" then
                       local wrote, why = regions.write_file(path, body.region, body.bytes)

                       if not wrote then return false, nil, why end

                       local m = message_of(f)

                       m.bytes = body.bytes
                       return true, m
                     end
                   end

                   return false, nil, ("the server sent no message %s"):format(tostring(uid))
                 end, { fetched = {}, into = path })
end

function S:flag(uids, flag, on)
  local word = FLAGS[flag] or flag

  return request(self, { ("UID STORE %s %sFLAGS.SILENT (%s)")
                         :format(imap.set(uids), on == false and "-" or "+", word) },
                 nil, { fetched = {} })
end

--
-- Messages to another mailbox: MOVE where the server has it (RFC 6851),
-- and otherwise a copy, the originals marked deleted, and those expunged -
-- by UID where UIDPLUS lets that touch only these.
--
function S:move(uids, to)
  local set = imap.set(uids)

  if self.can.MOVE then
    return request(self, { ("UID MOVE %s "):format(set), astring(to) }, nil, { fetched = {} })
  end

  local copy = request(self, { ("UID COPY %s "):format(set), astring(to) })
  local mark = request(self, { ("UID STORE %s +FLAGS.SILENT (\\Deleted)"):format(set) },
                       nil, { fetched = {}, needs = copy })

  return request(self, { self.can.UIDPLUS and ("UID EXPUNGE " .. set) or "EXPUNGE" },
                 nil, { needs = mark })
end

--
-- A message from a file into a mailbox - what was sent, kept in Sent; a
-- draft. Its bytes are a literal, sent when the server says to go on.
--
function S:append(name, path, flags)
  local bytes = fs.read(path)

  if type(bytes) ~= "string" then
    local r = { done = true, ok = false, why = ("cannot read %s"):format(path) }

    return r
  end

  local words = {}

  for _, f in ipairs(flags or {}) do words[#words + 1] = FLAGS[f] or f end

  return request(self, { "APPEND ", astring(name), (" (%s) "):format(table.concat(words, " ")),
                         { literal = bytes } },
                 function(s)
                   local code = s.last_code

                   s.last_code = nil

                   if code and code.name == "APPENDUID" then
                     local validity, uid = code.words:match("^(%d+)%s+(%d+)")

                     return true, { uidvalidity = tonumber(validity), uid = tonumber(uid) }
                   end

                   return true, {}
                 end)
end

--
-- IDLE (RFC 2177): the mailbox held open, and the server says when it
-- changes - `s.news` is set and stays set until the caller clears it.
-- The request is answered when `done` ends it.
--
function S:idle()
  self.news = false

  return request(self, { "IDLE" }, nil, {
    idling = true,
    continue = function(s) s.idle_open = true end,
  })
end

function S:done()
  if self.active and self.active.idling then
    self:write("DONE\r\n")
    self.idle_open = false
  end
end

function S:logout()
  return request(self, { "LOGOUT" })
end

return imap
