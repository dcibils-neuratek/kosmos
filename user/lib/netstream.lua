-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A conversation with a server over TCP, plain or with TLS laid on it: a
-- name looked up, a connection made, and one stream to write and read
-- whichever it is.
--
--   local netstream = use("/Kosmos/Libraries/netstream.lua")
--   local address, why = netstream.address(host[, wait_ticks])
--   local stream = netstream.plain(conn)
--   local stream = netstream.secure(conn, t)       t from the TLS Kit
--   local stream, why = netstream.open{ host =, port =, tls =, name =,
--                                       anchors =, insecure = }
--   stream:write(s) -> taken   stream:read() -> s   stream:flush()
--   stream:done() -> over, why, certificate        stream:close()
--   stream.conn                the TCP connection, to wait or poll on
--
-- **Taken out of `http.lua`** (8 October 2026) when mail became its second
-- user (`docs/mail.md` M2): IMAP and SMTP are conversations over the same
-- two kinds of connection, and a second copy of the TLS half is the kind
-- `CLAUDE.md`'s premise calls a defect. `http.lua` keeps what is HTTP's -
-- requests, replies, connections kept between them - and asks this for the
-- rest.
--
-- **Nothing here waits**, past the lookup: a connection is made at once and
-- the handshake goes on as the stream is written and read, so whoever holds
-- it waits on `stream.conn` with everything else it is waiting on.

local netstream = {}

local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
local prefs = use("/Kosmos/Libraries/prefs.lua")

-- The certificates a person trusts, a folder the settings kit keeps.
netstream.AUTHORITIES = prefs.path("Authorities")

--
-- **Names, remembered for a minute.** Every fetch looked its host up again:
-- gnu.org's twelve pictures were twelve questions to the resolver about one
-- name, each a round trip before anything else could start (`roadmap.md`
-- 6zz g). The resolver gives no lifetime back, so a minute is this side's,
-- and a failed lookup is not remembered.
--
local names = {}
local counter_hz

-- The counter's rate, read once: what a `sys.ticks()` difference is in.
function netstream.counter()
  counter_hz = counter_hz or (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  return counter_hz
end

-- Four numbers written as an address are the address, and need no lookup;
-- nil for a name, and nil and why for four numbers that are not one.
function netstream.numbers(host)
  if not tostring(host):match("^%d+%.%d+%.%d+%.%d+$") then return nil end

  local bytes = ipv4.bytes(host)

  if not bytes then return nil, "those are not four numbers under 256" end

  return bytes
end

--
-- A host's address: its numbers, or the resolver's answer, kept a minute.
-- Nil, why and what the resolver said when there is none.
--
function netstream.address(host, wait_ticks)
  local where, wrong = netstream.numbers(host)

  if wrong then return nil, wrong end
  if where then return where end

  local now = sys.ticks()
  local known = names[host]

  if known and now - known.at < 60 * netstream.counter() then
    return known.address
  end

  local hz = (sys.info() or {}).tick_hz or 250
  local found, why, said = fs.resolve(host, wait_ticks or 5 * hz)

  if found then
    names[host] = { address = found, at = now }
  end

  return found, why, said
end

--
-- The certificates this machine's person has said to trust: each file in
-- `AUTHORITIES` named `.der` or `.cer` that is not empty. As DER, for
-- the handshake; and by name, for whoever shows what is trusted - the
-- browser's Settings and its page of authorities, which listed every file
-- in the folder as trusted, whatever it was. One walk for both, so what is
-- shown is what is trusted.
--
function netstream.trusted()
  local ders, files = {}, {}

  for _, name in ipairs(fs.list(netstream.AUTHORITIES) or {}) do
    if name:match("%.[Dd][Ee][Rr]$") or name:match("%.[Cc][Ee][Rr]$") then
      local der = fs.read(netstream.AUTHORITIES .. "/" .. name)

      if type(der) == "string" and der ~= "" then
        ders[#ders + 1], files[#files + 1] = der, name
      end
    end
  end

  return ders, files
end

--
-- The connection a conversation goes over: the TCP one as it is, or TLS laid
-- on it. Both answer `write`, `read`, `flush` and `done` - whether nothing
-- more will come, and why when that is an error - so the conversation does
-- not care which.
--
function netstream.plain(conn)
  return {
    conn = conn,
    write = function(_, s) return conn:write(s) end,
    read = function() return conn:read() end,
    flush = function() end,
    done = function() return conn:closed(), nil end,
    close = function() end,
  }
end

function netstream.secure(conn, t)
  return {
    conn = conn,
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
-- TLS laid on a connection already open - at once, for IMAP on 993 and
-- SMTP on 465, or after `STARTTLS` was agreed in the clear. Checked against
-- the image's roots, the person's authorities and `opts.anchors`; `name` is
-- what the certificate must be for.
--
function netstream.start_tls(conn, name, opts)
  opts = opts or {}

  local tls = use("/Kosmos/Kits/tls")
  local anchors = netstream.trusted()

  for _, der in ipairs(opts.anchors or {}) do anchors[#anchors + 1] = der end

  local ok, t = pcall(tls.client, conn, name, { anchors = anchors,
                                                insecure = opts.insecure })

  if not ok then return nil, tostring(t) end

  return netstream.secure(conn, t)
end

--
-- A host's port, connected, and TLS laid on it when `opts.tls` says: the
-- stream, or nil and why. The lookup is the one wait.
--
function netstream.open(opts)
  local where, why, said = netstream.address(opts.host, opts.wait_ticks)

  if not where then
    return nil, ("cannot look up %s: %s"):format(opts.host, said or tostring(why))
  end

  local conn, cwhy, csaid = fs.connect("/Network", where, opts.port, true)

  if not conn then return nil, csaid or tostring(cwhy) end

  if not opts.tls then return netstream.plain(conn) end

  local stream, twhy = netstream.start_tls(conn, opts.name or opts.host, opts)

  if not stream then
    conn:close()
    return nil, twhy
  end

  return stream
end

return netstream
