-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- POP3 (RFC 1939), a client: a maildrop's messages listed by their unique
-- ids, one fetched into a file, one deleted - with CAPA (RFC 2449) and STLS
-- (RFC 2595).
--
--   local pop3 = use("/Kosmos/Libraries/pop3.lua")
--   local s, why = pop3.open{ host =, port = 995, user =, password =,
--                             tls = true, name =, anchors = }
--   s.ready                   a request: signed in, and what the server can do
--   s:list()                  -> { { n =, id =, size = } }, oldest first: a
--                                message's number in this session, its unique
--                                id (UIDL), which is the same in every
--                                session, and its size
--   s:fetch(n, path[, size])  -> { bytes = }: message n, into the file at
--                                `path`; `size` is what `list` said, a hint
--   s:delete(n)               marked; the server deletes it at `quit`
--   s:quit()
--
--   s:step()                  what has arrived, read and answered
--   s:wait(request[, seconds])    -> its result, or nil and why
--
-- **The same shape as `imap.lua`'s session**, so `maild` runs both the same
-- way: every call returns a request, `{ done, ok, result, why }`, answered
-- as the conversation goes, and nothing here waits on the network.
--
-- **POP3 has no folders and no flags**: one maildrop, and a message is
-- there or not. What it was read, flagged or filed is kept here, beside the
-- copy; the server is told only that a message is to go (`docs/mail.md`,
-- *POP3*).
--
-- TLS from the first byte on 995; on 110, STLS before the password, or no
-- password at all - one is never sent in the clear. USER and PASS sign in,
-- which every server takes.
--
-- **A message is never a Lua string here**, as in `imap.lua`: RETR's lines
-- go into a region as they arrive, the dot that stuffs a line undone, and
-- the region is written to the file whole. POP3 says no size before the
-- bytes, so the region is the size `list` gave and grows if a server's
-- count was short (one that counts a line's end as one byte, not two).

local pop3 = {}

local netstream = use("/Kosmos/Libraries/netstream.lua")
local regions = use("/Kosmos/Libraries/regions.lua")

-- How long a request may go with nothing heard before the session is given
-- up on, in seconds.
pop3.QUIET_SECONDS = 60

local S = {}
S.__index = S

--------------------------------------------------------------------------
-- Requests, one in flight at a time: POP3 answers in order and says
-- nothing of its own accord, so the answer is always the one in flight's.
--------------------------------------------------------------------------

local function settle(r, ok, result, why)
  if r.done then return end

  r.done, r.ok, r.result, r.why = true, ok, result, why
  if r.region then regions.free(r.region) r.region = nil end
end

-- `front`: before everything waiting - the sign-in's next step, which goes
-- ahead of whatever was asked while it was under way.
local function request(s, line, finish, opts, front)
  local r = { done = false, line = line, finish = finish }

  for k, v in pairs(opts or {}) do r[k] = v end

  if front then
    table.insert(s.queue, 1, r)
  else
    s.queue[#s.queue + 1] = r
  end

  s:send_next()

  return r
end

function S:fail(why)
  self.broken = self.broken or why

  if self.active then settle(self.active, false, nil, why) end

  for _, r in ipairs(self.queue) do settle(r, false, nil, why) end

  settle(self.ready, false, nil, why)
  self.active, self.queue = nil, {}
  self.opts.password = nil
  self.stream:close()
  self.stream.conn:close()
end

function S:write(text)
  self.out[#self.out + 1] = text
end

-- The next command, once the greeting has come; before the sign-in is done,
-- only the sign-in's own.
function S:send_next()
  if self.active or not self.greeted or self.broken then return end

  local r = self.queue[1]

  if not r or not (self.signed or r.signing) then return end

  table.remove(self.queue, 1)
  self.active = r
  self.heard = sys.ticks()
  self:write(r.line .. "\r\n")

  -- A password is not kept past the line it went in.
  r.line = nil
end

--------------------------------------------------------------------------
-- Signing in: CAPA, STLS where the connection is plain, USER and PASS.
--------------------------------------------------------------------------

local sign_in

local function capabilities(s, lines)
  s.can = {}

  for _, line in ipairs(lines or {}) do
    local word, rest = line:match("^(%S+)%s*(.*)$")

    if word then s.can[word:upper()] = rest end
  end
end

function sign_in(s)
  local o = s.opts

  if not s.stream.tls then
    if not s.can.STLS then
      s:fail("the server does not offer STLS, and will not be sent a password in the clear")
      return
    end

    request(s, "STLS", function(self)
      local secure, why = netstream.start_tls(self.stream.conn, o.name or o.host, o)

      if not secure then
        self:fail(why)
        return true
      end

      -- What was read through the plain stream is all there was: a server
      -- that spoke past its +OK has put words in TLS's place.
      if self.inbuf:sub(self.at) ~= "" then
        self:fail("the server spoke before TLS began")
        return true
      end

      self.stream = secure

      -- What it can do is asked again, as RFC 2595 says: it may say more
      -- once nobody can listen.
      request(self, "CAPA", function(again, r)
        capabilities(again, r.lines)
        sign_in(again)
        return true
      end, { multi = true, signing = true, soft = true }, true)

      return true
    end, { signing = true }, true)

    return
  end

  -- Held only until the PASS line is made, which is the moment USER is
  -- answered; this side lets go of it now.
  local password = o.password or ""

  o.password = nil

  request(s, "USER " .. (o.user or ""), function(self)
    request(self, "PASS " .. password, function(again)
      again.signed = true
      settle(again.ready, true, again.can)
      return true
    end, { signing = true }, true)

    password = nil
    return true
  end, { signing = true }, true)
end

--------------------------------------------------------------------------
-- What arrives.
--------------------------------------------------------------------------

-- A command's answer, `+OK` or `-ERR`: a one-line command is done with it,
-- and a multi-line one goes on to its lines.
function S:status(line)
  local r = self.active
  local word, rest = line:match("^([%+%-]%u+)%s?(.*)$")

  if not self.greeted then
    if word == "+OK" then
      self.greeted = true

      request(self, "CAPA", function(s, req)
        capabilities(s, req.lines)
        sign_in(s)
        return true
      end, { multi = true, signing = true, soft = true }, true)

      return
    end

    return self:fail("the server said: " .. line)
  end

  if not r then return end

  if word == "+OK" then
    r.first = rest

    if r.multi then
      self.multi = true
      return
    end

    return self:answered(r, true)
  end

  if r.soft then
    -- A server without CAPA is one that can do nothing extra.
    r.lines = {}
    return self:answered(r, true)
  end

  local why = ("the server said: %s"):format(rest ~= "" and rest or line)

  self.active = nil
  settle(r, false, nil, why)

  -- A refused sign-in is the end of the session: nothing else can go.
  if r.signing then return self:fail(why) end

  self:send_next()
end

function S:answered(r, ok)
  self.active = nil
  self.multi = false

  local good, result, why = ok, nil, nil

  if r.finish then good, result, why = r.finish(self, r) end

  if good == false then
    settle(r, false, nil, why)
  elseif r.signing then
    -- Settled by the step that finishes the sign-in, or by `fail`.
    r.done, r.ok = true, true
  else
    settle(r, true, result)
  end

  self:send_next()
end

-- A message's lines, as far as they have come, into its region: whole lines
-- only, so each piece starts at a line's start and a stuffed dot is always
-- at one. Done at the line that is a dot alone.
function S:body(r)
  local buf, at = self.inbuf, self.at
  local stop

  if buf:sub(at, at + 2) == ".\r\n" then
    stop = at
  else
    local e = buf:find("\r\n.\r\n", at, true)

    if e then stop = e + 2 end
  end

  local upto = stop and stop - 1 or select(2, buf:find(".*\r\n", at))

  if upto and upto >= at then
    local piece = buf:sub(at, upto):gsub("^%.", ""):gsub("\r\n%.", "\r\n")

    if not self:land(r, piece) then return false end
  end

  if not stop then
    if upto then self.at = upto + 1 end
    return false
  end

  self.at = stop + 3

  local wrote, why = regions.write_file(r.into, r.region, r.got)

  if not wrote then
    r.finish = function() return false, nil, why end
  else
    r.finish = function(_, req) return true, { bytes = req.got } end
  end

  self:answered(r, true)
  return true
end

-- A piece of a message, where the last one ended; the region twice the size
-- when it does not fit.
function S:land(r, piece)
  if not r.region then
    local region, why = regions.unmapped(math.max(r.size or 0, #piece, 1))

    if not region then
      self:fail(why)
      return false
    end

    r.region, r.got = region, 0
  end

  if r.got + #piece > r.region.size then
    local bigger, why = regions.unmapped(math.max(r.region.size * 2, r.got + #piece))

    if not bigger then
      self:fail(why)
      return false
    end

    if r.got > 0 then sys.region_copy(bigger.cap, 0, r.region.cap, 0, r.got) end

    regions.free(r.region)
    r.region = bigger
  end

  sys.region_write(r.region.cap, r.got, piece)
  r.got = r.got + #piece

  return true
end

function S:digest()
  while not self.broken do
    local r = self.active

    if self.multi and r and r.into then
      if not self:body(r) then break end
    else
      local e = self.inbuf:find("\r\n", self.at, true)

      if not e then break end

      local line = self.inbuf:sub(self.at, e - 1)

      self.at = e + 2

      if self.multi and r then
        if line == "." then
          self:answered(r, true)
        else
          r.lines = r.lines or {}
          r.lines[#r.lines + 1] = line:sub(1, 1) == "." and line:sub(2) or line
        end
      else
        self:status(line)
      end
    end
  end

  if self.at > 1 then
    self.inbuf = self.inbuf:sub(self.at)
    self.at = 1
  end
end

function S:step()
  if self.broken then return end

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
    self:fail(self.said_goodbye and "signed out" or why or "the server closed the connection")
    return
  end

  if (self.active or #self.queue > 0)
     and sys.ticks() - self.heard >= pop3.QUIET_SECONDS * netstream.counter() then
    self:fail(("nothing from the server for %d seconds"):format(pop3.QUIET_SECONDS))
  end
end

function S:wait(r, seconds)
  local hz = (sys.info() or {}).tick_hz or 250
  local tick = math.max(1, hz // 20)
  local deadline = sys.ticks() + (seconds or 30) * netstream.counter()

  while not r.done do
    self:step()

    if r.done or self.broken then break end

    if sys.ticks() > deadline then return nil, "no answer in time" end

    self.stream.conn:wait(tick)
  end

  if r.ok then return r.result end

  return nil, r.why or self.broken
end

--------------------------------------------------------------------------
-- The commands.
--------------------------------------------------------------------------

function pop3.open(opts)
  local o = {}

  for k, v in pairs(opts) do o[k] = v end

  local tls = o.tls ~= false and (o.port or 995) ~= 110
  local stream, why = netstream.open{
    host = o.host, port = o.port or 995, tls = tls,
    name = o.name, anchors = o.anchors, insecure = o.insecure,
  }

  if not stream then return nil, why end

  local s = setmetatable({
    opts = o, stream = stream, out = {}, inbuf = "", at = 1, queue = {},
    greeted = false, signed = false, multi = false, can = {},
    heard = sys.ticks(),
  }, S)

  s.ready = { done = false }

  return s
end

-- Every message's unique id and size, by its number in this session.
function S:list()
  local ids = request(self, "UIDL", nil, { multi = true })

  return request(self, "LIST", function(_, r)
    if not ids.ok then return false, nil, ids.why end

    local by_n, out = {}, {}

    for _, line in ipairs(ids.lines or {}) do
      local n, id = line:match("^(%d+)%s+(%S+)")

      if n then by_n[tonumber(n)] = id end
    end

    for _, line in ipairs(r.lines or {}) do
      local n, size = line:match("^(%d+)%s+(%d+)")

      n = tonumber(n)

      if n and by_n[n] then out[#out + 1] = { n = n, id = by_n[n], size = tonumber(size) } end
    end

    table.sort(out, function(x, y) return x.n < y.n end)
    return true, out
  end, { multi = true })
end

function S:fetch(n, path, size)
  return request(self, ("RETR %d"):format(n), nil,
                 { multi = true, into = path, size = tonumber(size) })
end

function S:delete(n)
  return request(self, ("DELE %d"):format(n))
end

function S:quit()
  return request(self, "QUIT", function(s)
    s.said_goodbye = true
    return true
  end)
end

return pop3
