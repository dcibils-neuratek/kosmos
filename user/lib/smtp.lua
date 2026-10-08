-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- SMTP (RFC 5321), a client: one message, sent.
--
--   local smtp = use("/Kosmos/Libraries/smtp.lua")
--   local job, why = smtp.send{ host =, port = 587, user =, password =,
--                               security = "starttls" | "tls" | "none",
--                               name =, anchors =,
--                               from = "lena@example.com",
--                               to = { "bob@example.net", ... },
--                               path = "/Home/Mail/.../draft.eml" }
--   job:step()                       what has arrived, read and answered
--   job:wait([seconds])              -> true, or nil and why
--   job.done, job.ok, job.why, job.said   the server's last words
--
-- `EHLO`; `STARTTLS` on 587, and TLS from the first byte on 465; `AUTH
-- PLAIN`; `MAIL FROM`; a `RCPT TO` for each recipient - the Bcc's among
-- them, who are in no header; `DATA`, the message's lines with a dot
-- doubled where one begins a line; `QUIT`. **Nothing here waits**: the
-- job is a conversation stepped when its connection has news, as
-- `imap.lua`'s session is (`docs/mail.md` M2).
--
-- The message is read from its file whole: it is going over TLS, whose
-- engine takes a string, so its bytes are a string on this side whatever
-- this did. What comes back from a server - a message - never is.

local smtp = {}

local netstream = use("/Kosmos/Libraries/netstream.lua")

smtp.QUIET_SECONDS = 60

local J = {}
J.__index = J

local function finish(job, ok, why)
  if job.done then return end

  job.done, job.ok, job.why = true, ok, why

  if not ok then
    job.stream:close()
    job.stream.conn:close()
  end
end

function J:write(text)
  self.out[#self.out + 1] = text
end

-- The message's lines, each ending CRLF, a leading dot doubled, and the
-- dot alone that ends it.
local function data_of(bytes)
  local text = bytes:gsub("\r?\n", "\r\n")

  if text:sub(-2) ~= "\r\n" then text = text .. "\r\n" end

  text = text:gsub("^%.", "..")
  text = text:gsub("\r\n%.", "\r\n..")

  return text .. ".\r\n"
end

--
-- The conversation, one reply at a time: what each step expects, and what
-- it says next.
--
function J:reply(code, lines)
  local said = table.concat(lines, " ")

  self.said = said

  local function want(ok)
    if code ~= ok then
      finish(self, false, ("the server said: %d %s"):format(code, said))
      return false
    end

    return true
  end

  local o = self.opts
  local step = self.state

  if step == "greeting" then
    if want(220) then
      self:write("EHLO kosmos\r\n")
      self.state = "ehlo"
    end
  elseif step == "ehlo" then
    if not want(250) then return end

    self.can = {}

    for _, line in ipairs(lines) do
      local word, rest = line:match("^(%S+)%s*(.*)$")

      if word then self.can[word:upper()] = rest:upper() end
    end

    if self.security == "starttls" and not self.stream.tls then
      if not self.can.STARTTLS then
        return finish(self, false, "the server does not offer STARTTLS, and will not be sent a password in the clear")
      end

      self:write("STARTTLS\r\n")
      self.state = "starttls"
    elseif o.user then
      local compress = use("/Kosmos/Kits/compress")

      self:write("AUTH PLAIN " .. compress.base64("\0" .. o.user .. "\0" .. (o.password or "")) .. "\r\n")
      self.state = "auth"
    else
      self:mail_from()
    end
  elseif step == "starttls" then
    if not want(220) then return end

    local secure, why = netstream.start_tls(self.stream.conn, o.name or o.host, o)

    if not secure then return finish(self, false, why) end

    self.stream = secure
    self.switched = true
    self:write("EHLO kosmos\r\n")
    self.state = "ehlo"
  elseif step == "auth" then
    if want(235) then self:mail_from() end
  elseif step == "from" then
    if want(250) then
      self.next_to = 1
      self:rcpt_to()
    end
  elseif step == "to" then
    if code ~= 250 and code ~= 251 then
      return finish(self, false, ("%s was refused: %d %s")
                                 :format(self.to[self.next_to - 1], code, said))
    end

    self:rcpt_to()
  elseif step == "data" then
    if code ~= 354 then return want(354) end

    self:write(self.body)
    self.body = nil
    self.state = "sent"
  elseif step == "sent" then
    if want(250) then
      self.accepted = said
      self:write("QUIT\r\n")
      self.state = "quit"
    end
  elseif step == "quit" then
    -- Sent at "250", whatever the goodbye says.
    finish(self, true)
  end
end

function J:mail_from()
  self:write(("MAIL FROM:<%s>\r\n"):format(self.opts.from))
  self.state = "from"
end

function J:rcpt_to()
  local who = self.to[self.next_to]

  if who then
    self.next_to = self.next_to + 1
    self:write(("RCPT TO:<%s>\r\n"):format(who))
    self.state = "to"
  else
    self:write("DATA\r\n")
    self.state = "data"
  end
end

function J:step()
  if self.done then return end

  if #self.out > 0 then
    local text = table.concat(self.out)

    self.out = {}

    local n = self.stream:write(text)

    if n < #text then self.out[1] = text:sub(n + 1) end
  end

  self.stream:flush()

  for _ = 1, 64 do
    local got = self.stream:read()

    if not got or got == "" then break end

    self.heard = sys.ticks()
    self.inbuf = self.inbuf .. got

    -- Replies: `250-first`, `250-more`, `250 last`.
    while not self.done do
      local e = self.inbuf:find("\r\n", 1, true)

      if not e then break end

      local line = self.inbuf:sub(1, e - 1)

      self.inbuf = self.inbuf:sub(e + 2)

      local code, sep, rest = line:match("^(%d%d%d)([ -]?)(.*)$")

      if not code then
        return finish(self, false, "the server's reply was not SMTP: " .. line)
      end

      self.lines[#self.lines + 1] = rest

      if sep ~= "-" then
        local lines = self.lines

        self.lines = {}
        self:reply(tonumber(code), lines)

        -- A switch to TLS reads nothing more through the plain stream.
        if self.switched then
          self.switched = nil

          if self.inbuf ~= "" then
            return finish(self, false, "the server spoke before TLS began")
          end
        end
      end
    end
  end

  if self.done then return end

  local over, why = self.stream:done()

  if over then
    return finish(self, false, why or "the server closed the connection")
  end

  if sys.ticks() - self.heard >= smtp.QUIET_SECONDS * netstream.counter() then
    finish(self, false, ("nothing from the server for %d seconds"):format(smtp.QUIET_SECONDS))
  end
end

function J:wait(seconds)
  local hz = (sys.info() or {}).tick_hz or 250
  local tick = math.max(1, hz // 20)
  local deadline = sys.ticks() + (seconds or 60) * netstream.counter()

  while not self.done do
    self:step()

    if self.done then break end

    if sys.ticks() > deadline then return nil, "no answer in time" end

    self.stream.conn:wait(tick)
  end

  if self.ok then return true end

  return nil, self.why
end

function smtp.send(opts)
  local bytes = opts.message or (opts.path and fs.read(opts.path))

  if type(bytes) ~= "string" then
    return nil, ("cannot read %s"):format(tostring(opts.path))
  end

  local to = {}

  for _, who in ipairs(opts.to or {}) do to[#to + 1] = who end

  if #to == 0 then return nil, "a message needs somebody to go to" end

  local security = opts.security or (opts.port == 465 and "tls") or "starttls"
  local stream, why = netstream.open{
    host = opts.host, port = opts.port or 587, tls = security == "tls",
    name = opts.name, anchors = opts.anchors, insecure = opts.insecure,
  }

  if not stream then return nil, why end

  return setmetatable({
    opts = opts, stream = stream, security = security, to = to,
    body = data_of(bytes), out = {}, inbuf = "", lines = {},
    state = "greeting", heard = sys.ticks(), done = false,
  }, J)
end

return smtp
