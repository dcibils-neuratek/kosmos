-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One HTTP request, over TCP or over TLS, and the whole reply.
--
--   local http = use("/Kosmos/Libraries/http.lua")
--   local reply, why, how = http.get("https://example.com/")
--
-- **One place for what `fetch` and the browser both do** (`roadmap.md` 6zz
-- c): an address taken apart, a name looked up, a connection made, TLS laid
-- over it when the address says `https`, a request written until it has all
-- gone and a reply read until the far end closes. Each had its own copy of
-- the plain half, and HTTPS would have made two copies of the harder one.
--
-- HTTP/1.0 on purpose, as `fetch` has always spoken it: 1.1 keeps the
-- connection open and would need this to understand `Content-Length` and
-- chunked encoding to know when to stop; 1.0's answer is that the server
-- closes, which is exactly what the connection reports.
--
-- **Whom it trusts**: the roots the image carries - Mozilla's, in the TLS
-- Kit - and every certificate in `/Home/Preferences/Authorities`, in DER, a
-- person's own: a network's authority, or a test's. Nothing else.
--
-- What comes back is the reply as it arrived, head and all, and a table
-- saying how it came:
--
--   how.scheme      "http" or "https"
--   how.secure      over TLS: true when the certificate checked out, false
--                   when it was taken anyway (`anyway`); nil over plain HTTP
--   how.reason      why it did not check out
--   how.refused     the request was refused for its certificate, and why -
--                   what an Open anyway would go past
--   how.short       the body ended before the length the server gave:
--                   { got = bytes, want = bytes } - never passed on as whole
--
-- Nothing here prints. `opts.say`, when given, is told each step as it
-- starts, which is what the browser's status line shows.

local http = {}

http.AUTHORITIES = "/Home/Preferences/Authorities"

local function trim(text)
  return (tostring(text or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

--
-- An address in parts: `scheme`, `host`, `port`, `path`, and `hostport` as
-- it was written. No scheme means `http`, as every browser takes it.
--
function http.split(text)
  local rest = trim(text)
  local scheme = "http"
  local named, after = rest:match("^(%a[%w+.%-]*)://(.*)$")

  if named then
    named = named:lower()

    if named ~= "http" and named ~= "https" then
      return nil, ("this speaks http and https, not %s"):format(named)
    end

    scheme, rest = named, after
  end

  local hostport, path = rest:match("^([^/]+)(/.*)$")

  hostport = hostport or rest
  path = path or "/"

  if hostport == "" then return nil, "that address has no host" end

  local host, port = hostport:match("^([^:]+):(%d+)$")

  host = host or hostport

  return { scheme = scheme, host = host, hostport = hostport, path = path,
           port = tonumber(port) or (scheme == "https" and 443 or 80) }
end

-- Four numbers under 256, as the four bytes the Network Kit takes; nil for a
-- name, and nil and why for four numbers that are not an address.
local function numbers(host)
  local a, b, c, d = tostring(host):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then
    return nil, "those are not four numbers under 256"
  end

  return string.char(a, b, c, d)
end

-- Kosmos and its revision: `Kosmos/0.10.200`.
function http.agent()
  local build = sys.build and sys.build() or {}

  return ("%s/%s"):format(build.name or "Kosmos", build.version or "0")
end

--
-- The certificates this machine's person has said to trust, as DER.
--
function http.authorities()
  local out = {}

  for _, name in ipairs(fs.list(http.AUTHORITIES) or {}) do
    if name:match("%.[Dd][Ee][Rr]$") or name:match("%.[Cc][Ee][Rr]$") then
      local der = fs.read(http.AUTHORITIES .. "/" .. name)

      if type(der) == "string" and der ~= "" then out[#out + 1] = der end
    end
  end

  return out
end

--
-- The connection a request goes over: the TCP one as it is, or TLS laid on
-- it. Both answer `write`, `read`, `flush` and `done` - whether nothing more
-- will come, and why when that is an error - so the request does not care
-- which.
--
local function plain(conn)
  return {
    write = function(_, s) return conn:write(s) end,
    read = function() return conn:read() end,
    flush = function() end,
    done = function() return conn:closed(), nil end,
    close = function() end,
  }
end

local function secure(conn, t)
  return {
    write = function(_, s) return t:write(s) end,
    read = function() return t:read() end,
    flush = function() t:flush() end,
    done = function()
      local state, reason, code, certificate = t:state()

      if state == "closed" then
        return true, (code ~= 0) and reason or nil, certificate
      end

      -- The connection's bytes are the engine's to read, never this: a
      -- closed connection is enough, and the read after the loop takes what
      -- was left in the engine.
      return conn:closed(), nil
    end,
    close = function() t:close() end,
    tls = t,
  }
end

--
-- `http.get(address [, opts])`.
--
--   opts.anyway      over TLS, go on whatever the certificate says
--   opts.anchors     certificates in DER to trust besides the others
--   opts.name        the name to hold the certificate to and send, for an
--                    address given by number (`fetch --name`)
--   opts.say         told each step as it starts
--   opts.agent       the User-Agent; Kosmos and its revision without one
--   opts.progress    told `(bytes, total)` as the body arrives - total is the
--                    server's `Content-Length`, or nil when it gave none
--   opts.wait_ticks  how long a name may take to look up, scheduler ticks
--
function http.get(address, opts)
  opts = opts or {}

  local say = opts.say or function() end
  local parts, bad = address, nil

  if type(address) ~= "table" then parts, bad = http.split(address) end

  if not parts then return nil, bad, {} end

  local how = { scheme = parts.scheme }
  local hz = (sys.info() or {}).tick_hz or 250
  local where, wrong = numbers(parts.host)

  if wrong then return nil, wrong, how end

  if not where then
    say("looking up " .. parts.host .. " ...")

    local found, why, said = fs.resolve(parts.host, opts.wait_ticks or 5 * hz)

    if not found then
      return nil, ("cannot look up %s: %s"):format(parts.host, said or tostring(why)), how
    end

    where = found
  end

  say("connecting to " .. parts.hostport .. " ...")

  local conn, why, said = fs.connect("/Network", where, parts.port)

  if not conn then return nil, said or tostring(why), how end

  local stream = plain(conn)
  local name = opts.name or parts.host

  if parts.scheme == "https" then
    local tls = use("/Kosmos/Kits/tls")
    local anchors = http.authorities()

    for _, der in ipairs(opts.anchors or {}) do anchors[#anchors + 1] = der end

    local ok, t = pcall(tls.client, conn, name, { anchors = anchors,
                                                  insecure = opts.anyway })

    if not ok then
      conn:close()
      return nil, tostring(t), how
    end

    stream = secure(conn, t)
  end

  --
  -- `Host` because every server since 1.1 wants one even from a 1.0 client,
  -- and `Connection: close` because saying so is politer than relying on the
  -- version to imply it.
  --
  -- Written as it is taken: over TLS nothing is taken until the handshake is
  -- done, so the request waits for it here, and a handshake that ends
  -- instead says why - the certificate's reason, when it was that.
  --
  -- **And who is asking.** Wikipedia answers a request that does not say
  -- with 126 bytes of refusal, and it is not alone. `opts.agent` is the
  -- caller's to choose - a browser names the engine sites should write for
  -- - and without one this is Kosmos and its revision, which is what
  -- `fetch` is.
  local request = ("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n"
                   .. "Accept: text/html, */*\r\nConnection: close\r\n\r\n")
                  :format(parts.path, opts.name or parts.hostport,
                          opts.agent or http.agent())
  local sent = 0
  local tick = math.max(1, hz // 10)

  for _ = 1, 150 do
    if sent >= #request then break end

    local n = stream:write(request:sub(sent + 1))

    sent = sent + n

    if n == 0 then
      local over, reason, certificate = stream:done()

      if over then
        conn:close()

        if certificate then how.refused = reason end

        return nil, reason or "the connection closed before the request went", how
      end

      conn:wait(tick)
    end
  end

  if sent < #request then
    conn:close()
    return nil, ("only %d of %d bytes of the request went"):format(sent, #request), how
  end

  stream:flush()

  if stream.tls then
    local trusted, reason = stream.tls:trusted()

    how.secure = trusted == true
    how.reason = reason
  end

  say("waiting for " .. parts.hostport .. " ...")

  --
  -- Until the far end closes, or fifteen seconds pass with nothing new.
  --
  -- Bounded by quiet rather than by count: a large page arrives in many
  -- pieces and every one of them is progress, while a server that has
  -- stopped is the thing to give up on. The read after the loop is not
  -- redundant - the close and the last bytes can arrive together.
  --
  local parts_in, idle, reason = {}, 0, nil

  --
  -- **How much, of how much** - the head's end found once, and its
  -- `Content-Length` with it - for a progress bar (`roadmap.md` 6zz i) and
  -- for the check below. Told every 32 KB rather than every read, so a page
  -- of ten megabytes is a few hundred repaints and not thousands.
  --
  local got, head_at, total, told = 0, nil, nil, -1
  local progress = opts.progress

  while idle < 150 do
    local text = stream:read()

    if text then
      parts_in[#parts_in + 1] = text
      got = got + #text
      idle = 0

      if not head_at then
        local sofar = table.concat(parts_in)
        local e = sofar:find("\r\n\r\n", 1, true)

        if e then
          head_at = e + 3
          total = tonumber(sofar:sub(1, e):match("\r\n[Cc][Oo][Nn][Tt][Ee][Nn][Tt]%-[Ll][Ee][Nn][Gg][Tt][Hh]:%s*(%d+)"))
        end
      end

      if progress and head_at and got - head_at - told >= 32768 then
        told = got - head_at
        progress(told, total)
      end
    end

    local over, why_over = stream:done()

    if over then
      reason = why_over
      break
    end

    if not text then
      idle = idle + 1
      conn:wait(tick)
    end
  end

  local last = stream:read()

  if last then
    parts_in[#parts_in + 1] = last
    got = got + #last
  end

  -- The head, if the last read was the one that brought it.
  if not head_at then
    local e = table.concat(parts_in):find("\r\n\r\n", 1, true)

    if e then
      head_at = e + 3
      total = tonumber(table.concat(parts_in):sub(1, e)
                       :match("\r\n[Cc][Oo][Nn][Tt][Ee][Nn][Tt]%-[Ll][Ee][Nn][Gg][Tt][Hh]:%s*(%d+)"))
    end
  end

  stream:close()
  conn:close()

  local reply = table.concat(parts_in)

  if reply == "" then return nil, reason or "nothing came back", how end

  --
  -- **Cut short is said, and never passed on as whole.** A page that
  -- stalled past the fifteen seconds above arrived as its first part and
  -- was treated as all of it - Wikipedia's Dam article was 1,374,752 bytes
  -- of 1,435,447, and the parser refused what it was given. The server said
  -- how long it was; the length is held to that.
  --
  if head_at and total and got - head_at < total then
    how.short = { got = got - head_at, want = total }
  end

  if progress and head_at then progress(got - head_at, total) end

  how.ended = reason
  return reply, nil, how
end

--
-- A reply in parts: the status, the head, and the body.
--
function http.parse(reply)
  local status = tonumber(reply:match("^HTTP/%d%.%d%s+(%d%d%d)")) or 200
  local head = reply:match("^(.-)\r\n\r\n") or ""
  local body = reply:match("\r\n\r\n(.*)$") or reply

  return status, head, body
end

return http
