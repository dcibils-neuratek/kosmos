-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One HTTP request, over TCP or over TLS, and the whole reply.
--
--   local http = use("/Kosmos/Libraries/http.lua")
--   local reply, why, how = http.get("https://example.com/")
--
-- **One place for what `fetch` and the browser both do** (`roadmap.md` 6zz
-- c): an address taken apart, a name looked up, a connection made, TLS laid
-- over it when the address says `https`, a request written until it has all
-- gone and a reply read until it is whole. Each had its own copy of
-- the plain half, and HTTPS would have made two copies of the harder one.
--
-- HTTP/1.1, and the connection kept for the next request to the same
-- place (`keep`, below). It was 1.0 on purpose, as `fetch` had always
-- spoken it: 1.0's answer to when a reply ends is that the server closes,
-- which is exactly what the connection reports - and every request paid a
-- lookup, a connection and a handshake for it. So a reply now ends where
-- its head says - its `Content-Length`, its last chunk - and the connection
-- goes on.
--
-- **Whom it trusts**: the roots the image carries - Mozilla's, in the TLS
-- Kit - and every certificate in `/Home/Preferences/Authorities`, in DER, a
-- person's own: a network's authority, or a test's. Nothing else.
--
-- What comes back is the reply, head and all, and a table saying how it
-- came:
--
--   how.scheme      "http" or "https"
--   how.secure      over TLS: true when the certificate checked out, false
--                   when it was taken anyway (`anyway`); nil over plain HTTP
--   how.reason      why it did not check out
--   how.resumed     over TLS, the session this process had with the host
--                   taken back, the key exchange skipped
--   how.refused     the request was refused for its certificate, and why -
--                   what an Open anyway would go past
--   how.short       the body ended before the length the server gave, or
--                   before its last chunk: { got = bytes, want = bytes },
--                   `want` nil for chunks - never passed on as whole
--   how.kept        it went over a connection kept from a request before
--   how.gzip        it came gzipped: how many bytes it was, compressed
--
-- A body sent in chunks comes back put together, and one sent gzipped comes
-- back inflated; the head is as it came.
--
-- Nothing here prints. `opts.say`, when given, is told each step as it
-- starts, which is what the browser's status line shows.
--
-- A GET unless `opts.body` is given: then a POST of that body, with
-- `opts.content_type` its type - a form sent (`roadmap.md` 6zz j6).

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

--
-- **A refresh's words**, `5; url=next.html`, as a `<meta http-equiv=
-- "refresh">` and the `Refresh` header both say them: the seconds, and the
-- address or nil for the page itself - or nil for words that are not one.
-- The grammar is HTML's ("shared declarative refresh steps"): a number,
-- then `;` or `,` and `url=`, which may be left out, and the address,
-- which may be in quotes. DuckDuckGo's, to a browser that runs no scripts:
-- `0; url="https://html.duckduckgo.com/html"`.
--
function http.refresh(content)
  local s = tostring(content or "")
  local i = s:find("%S") or #s + 1
  local whole = s:match("^%d*", i)

  i = i + #whole

  if whole == "" and s:sub(i, i) ~= "." then return nil end

  i = i + #s:match("^[%d.]*", i)

  local seconds = tonumber(whole) or 0

  if i > #s then return seconds, nil end

  if not s:sub(i, i):match("[;,%s]") then return nil end

  i = s:find("%S", i) or #s + 1

  if s:sub(i, i):match("[;,]") then i = s:find("%S", i + 1) or #s + 1 end

  if s:sub(i, i + 2):lower() == "url" then
    local j = s:find("%S", i + 3) or #s + 1

    if s:sub(j, j) == "=" then i = s:find("%S", j + 1) or #s + 1 end
  end

  local quote, address = s:sub(i, i), nil

  if quote == "'" or quote == '"' then
    local close = s:find(quote, i + 1, true)

    address = s:sub(i + 1, (close or #s + 1) - 1)
  else
    address = s:sub(i):match("^(.-)%s*$")
  end

  return seconds, address ~= "" and address or nil
end

-- Kosmos and its revision: `Kosmos/0.10.200`.
function http.agent()
  local build = sys.build and sys.build() or {}

  return ("%s/%s"):format(build.name or "Kosmos", build.version or "0")
end

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
local function counter()
  counter_hz = counter_hz or (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  return counter_hz
end

--
-- **Fifteen seconds with nothing new** is when a request is given up on -
-- measured on the counter, not counted in waits. It was a hundred and fifty
-- waits of a tenth of a second each, which is what a wait is when this
-- waits for itself; a window stepping a fetch between its passes
-- (`roadmap.md` 6zz l2) waits a pass at a time, and a hundred and fifty of
-- those is half a second.
--
local QUIET_SECONDS = 15

local function quiet_for(since_ticks)
  return sys.ticks() - since_ticks >= QUIET_SECONDS * counter()
end

local function lookup(host, wait_ticks, hz)
  local now = sys.ticks()
  local known = names[host]

  if known and now - known.at < 60 * counter() then
    return known.address
  end

  local found, why, said = fs.resolve(host, wait_ticks or 5 * hz)

  if found then
    names[host] = { address = found, at = now }
  end

  return found, why, said
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
-- **Connections kept** (`roadmap.md` 6zz g). HTTP/1.1, and a connection the
-- server keeps open is kept here too, for the next request to the same
-- place: no lookup, no connection and no handshake - where gnu.org's twelve
-- pictures were twelve of each. Kept by where it goes and how it was
-- checked - the scheme, the host and port, the name the certificate was
-- held to, and whether it was opened anyway - so a connection is only ever
-- taken back by a request that would have opened the same one. A request
-- with authorities of its own (`fetch --cacert`) neither keeps one nor
-- takes one: what it trusts is not what the next request trusts.
--
-- A kept connection the server has since closed is let go when it is next
-- looked at. One it closed as the request went - a race nobody can win -
-- answers with nothing at all, and the request is made once more on a new
-- connection, as every browser does for a GET.
--
-- For half a minute, which is less than most servers keep one open (nginx
-- 75 seconds; Apache 5, and a server that closes is noticed), and eight to
-- a place at most - the number `get_many` has going at once.
--
local kept = {}
local KEEP_SECONDS = 30
local KEEP_EACH = 8

local function let_go(k)
  k.stream:close()
  k.conn:close()
end

local function stale(k, now)
  return k.conn:closed() or now - k.at >= KEEP_SECONDS * counter()
end

local function take_kept(key)
  local list = kept[key]
  local now = sys.ticks()

  while list and #list > 0 do
    local k = table.remove(list)

    if not stale(k, now) then return k end

    let_go(k)
  end

  return nil
end

local function keep(key, conn, stream)
  local now = sys.ticks()

  -- The stale ones anywhere, while here: a place never asked again would
  -- otherwise hold its connections for as long as the process lives.
  for _, list in pairs(kept) do
    for i = #list, 1, -1 do
      if stale(list[i], now) then let_go(table.remove(list, i)) end
    end
  end

  local list = kept[key] or {}

  kept[key] = list

  if #list >= KEEP_EACH then
    let_go({ conn = conn, stream = stream })
  else
    list[#list + 1] = { conn = conn, stream = stream, at = now }
  end
end

--
-- **A body in chunks** - `Transfer-Encoding: chunked`, which is how a
-- server says it is done without knowing the length when it started: each
-- chunk's length in hex on a line of its own, its bytes, a line's end, and
-- a length of 0 for the last. Fed the bytes in whatever pieces they came
-- in, sliced rather than looked at one by one.
--
-- `state` is "done" at the last chunk's end and "bad" at anything that is
-- not a chunk; `extra` says bytes came after the end, which a connection
-- that is to be used again must not have. `emit`, when there is one, is
-- handed each piece of the body as it is sliced out (`roadmap.md` 6zz l1).
--
local function dechunk(emit)
  local d = { got = 0, state = "size", extra = false }
  local out, line, need = {}, "", 0

  function d.feed(s)
    local i, n = 1, #s

    while i <= n and d.state ~= "done" and d.state ~= "bad" do
      if d.state == "data" then
        local take = math.min(need, n - i + 1)

        local piece = s:sub(i, i + take - 1)

        out[#out + 1] = piece
        d.got, need, i = d.got + take, need - take, i + take

        if emit then emit(piece) end

        if need == 0 then d.state = "gap" end
      else
        local e = s:find("\n", i, true)

        if not e then
          line = line .. s:sub(i)
          i = n + 1

          -- A length, or a trailer, is a line and not a page.
          if #line > 4096 then d.state = "bad" end
        else
          local l = (line .. s:sub(i, e - 1)):gsub("\r$", "")

          line, i = "", e + 1

          if d.state == "gap" then
            d.state = (l == "") and "size" or "bad"
          elseif d.state == "size" then
            local size = tonumber(l:match("^%s*(%x+)") or "", 16)

            if not size then
              d.state = "bad"
            elseif size == 0 then
              d.state = "trailer"
            else
              need, d.state = size, "data"
            end
          elseif l == "" then
            d.state = "done"
          end
        end
      end
    end

    if d.state == "done" and i <= n then d.extra = true end
  end

  function d.body() return table.concat(out) end

  return d
end

--
-- How a reply's body ends, from its head: `length` bytes, `chunked`, `none`
-- - the statuses that never have one - or at the `close`; and whether the
-- connection may be used again after it. 1.1 keeps unless it says close,
-- 1.0 closes unless it says keep-alive, and a body that ends at the close
-- ends the connection with it.
--
local function framing(head)
  local version = head:match("^HTTP/(%d%.%d)")
  local status = tonumber(head:match("^HTTP/%d%.%d%s+(%d%d%d)")) or 200
  local low = head:lower()
  local said = low:match("\r\nconnection:%s*([^\r\n]*)") or ""
  local again = (version == "1.1" and not said:find("close", 1, true))
                or (version == "1.0" and said:find("keep-alive", 1, true) ~= nil)
  local encoding = low:match("\r\ntransfer%-encoding:%s*([^\r\n]*)")
  local length = tonumber(low:match("\r\ncontent%-length:%s*(%d+)"))

  if encoding and encoding:find("chunked", 1, true) then
    return "chunked", nil, again
  elseif status < 200 or status == 204 or status == 304 then
    return "none", 0, again
  elseif length then
    return "length", length, again
  end

  return "close", nil, false
end

--
-- `http.get(address [, opts])`.
--
--   opts.anyway      over TLS, go on whatever the certificate says - or a
--                    function of the address's parts that says it for each
--   opts.anchors     certificates in DER to trust besides the others
--   opts.name        the name to hold the certificate to and send, for an
--                    address given by number (`fetch --name`)
--   opts.say         told each step as it starts
--   opts.agent       the User-Agent; Kosmos and its revision without one
--   opts.progress    told `(bytes, total)` as the body arrives - total is the
--                    server's `Content-Length`, or nil when it gave none
--   opts.pause       called with the connection where this would wait for it;
--                    `http.get_many` passes one that yields to its scheduler,
--                    and takes one itself, called where it would wait for
--                    any of its fetches - a window stepping it (6zz l2);
--                    `get_many`'s `opts.each` is told `(i, reply, why, how)`
--                    as each one ends
--   opts.wait_ticks  how long a name may take to look up, scheduler ticks
--   opts.body        sent as a POST, `opts.content_type` its type
--   opts.headers     more fields for the request, by name
--   opts.cache       a cache (`httpcache.lua`) to answer from and keep in;
--                    `opts.revalidate` asks the server even when it is fresh
--   opts.on_body     called with the head when it arrives; may return a
--                    function, which is handed the body as it comes - its
--                    chunks undone and inflated when it came gzipped, never
--                    past the length the head gave. `how.streamed` says it
--                    was handed exactly the body returned (6zz l1)
--

--
-- One request over a connection that is open, and its reply. The fourth
-- value says the connection was a kept one that turned out to be closed -
-- nothing written, or nothing back - and the request should go again on a
-- new one.
--
local function exchange(parts, opts, conn, stream, key, was_kept, how)
  local say = opts.say or function() end
  local hz = (sys.info() or {}).tick_hz or 250

  --
  -- `Host` because every server since 1.1 wants one, and HTTP/1.1 so that
  -- the server may keep the connection for the next request (above).
  --
  -- **And gzip**, which every server on the web will send a client that
  -- says it takes it: Wikipedia's Dam article is 1.4 MB as HTML and a
  -- fifth of that compressed. Inflated in C, by the Compression Kit
  -- (`gzip.c`), below.
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
  local tick = math.max(1, hz // 10)
  local pause = opts.pause or function(c) c:wait(tick) end

  --
  -- **A body makes it a POST** (`roadmap.md` 6zz j6): a form sent, its
  -- fields encoded by whoever sent it and said what they are with
  -- `opts.content_type`.
  --
  local posting = opts.body ~= nil

  -- And the caller's own fields - the cache's validators, asking whether
  -- what it kept is still right (`httpcache.lua`) - in a fixed order.
  local fields, names = "", {}

  for name in pairs(opts.headers or {}) do names[#names + 1] = name end

  table.sort(names)

  for _, name in ipairs(names) do
    fields = fields .. name .. ": " .. tostring(opts.headers[name]) .. "\r\n"
  end

  local request = ("%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
                   .. "Accept: text/html, */*\r\nAccept-Encoding: gzip\r\n%s%s\r\n%s")
                  :format(posting and "POST" or "GET", parts.path,
                          opts.name or parts.hostport,
                          opts.agent or http.agent(),
                          posting and ("Content-Type: %s\r\nContent-Length: %d\r\n")
                                      :format(opts.content_type
                                              or "application/x-www-form-urlencoded",
                                              #opts.body) or "",
                          fields, opts.body or "")
  local sent, moved = 0, sys.ticks()

  while sent < #request and not quiet_for(moved) do
    local n = stream:write(request:sub(sent + 1))

    sent = sent + n

    if n > 0 then moved = sys.ticks() end

    if n == 0 then
      local over, reason, certificate = stream:done()

      if over then
        let_go({ conn = conn, stream = stream })

        if was_kept and sent == 0 then return nil, nil, how, true end

        if certificate then how.refused = reason end

        return nil, reason or "the connection closed before the request went", how
      end

      pause(conn)
    end
  end

  if sent < #request then
    let_go({ conn = conn, stream = stream })
    return nil, ("only %d of %d bytes of the request went"):format(sent, #request), how
  end

  stream:flush()

  if stream.tls then
    local trusted, reason = stream.tls:trusted()

    how.secure = trusted == true
    how.reason = reason
    how.resumed = not was_kept and stream.tls:resumed() == true
  end

  how.kept = was_kept or nil

  say("waiting for " .. parts.hostport .. " ...")

  --
  -- Until the body is whole, or the far end closes, or fifteen seconds pass
  -- with nothing new.
  --
  -- Bounded by quiet rather than by count: a large page arrives in many
  -- pieces and every one of them is progress, while a server that has
  -- stopped is the thing to give up on.
  --
  -- **Whole** is the head's to say (`framing`): its length, its chunks, or
  -- the close. The head is found once, in what has come so far; what came
  -- after it is the body's.
  --
  local pre, head = "", nil          -- head: false when there is none
  local rule, want, again
  local chunks, body, got = nil, {}, 0
  local heard, reason, ended = sys.ticks(), nil, false

  --
  -- **The body handed on as it comes** (`roadmap.md` 6zz l1), to whoever
  -- asked with `opts.on_body` - a browser feeding its parser while the rest
  -- arrives. The same bytes this returns, and only those: never past the
  -- head's length, its chunks undone, and inflated as a stream when it came
  -- gzipped, the pieces kept so the whole need not be inflated again.
  -- `handed` stays true only while that holds; anything that would make
  -- the body returned differ from what was handed on clears it, and the
  -- caller then reads the body returned instead.
  --
  local sink, inflating, inflated, handed = nil, nil, {}, false
  local sent_on = 0

  local function hand_on(piece)
    if not handed or piece == "" then return end

    if inflating then
      local text, why = inflating:feed(piece)

      if not text then
        handed = false
        how.stream_said = why
        return
      end

      if text == "" then return end

      inflated[#inflated + 1] = text
      piece = text
    end

    sink(piece)
  end

  local function begin_handing()
    sink = opts.on_body and opts.on_body(head) or nil

    if not sink then return end

    handed = true

    local coding = head:lower():match("\r\ncontent%-encoding:%s*([^\r\n]*)")

    if coding and coding:find("gzip", 1, true) then
      local room = ((sys.info() or {}).pages_free or 16384) * 4096 // 2

      inflating = use("/Kosmos/Kits/compress").gunzip_stream(room)
    end
  end

  --
  -- **How much, of how much**, for a progress bar (`roadmap.md` 6zz i) and
  -- for the check below. Told every 32 KB rather than every read, so a page
  -- of ten megabytes is a few hundred repaints and not thousands.
  --
  local progress, told = opts.progress, -1

  local function take(text)
    if head == nil then
      local from = math.max(1, #pre - 2)

      pre = pre .. text

      local e = pre:find("\r\n\r\n", from, true)

      -- A head is a few hundred bytes. Sixty-four kilobytes without the
      -- end of one is a reply that has none, kept as it came.
      if not e and #pre > 65536 then
        head, rule, text, pre = false, "close", pre, nil
      elseif not e then
        return
      else
        head = pre:sub(1, e + 3)
        rule, want, again = framing(head)
        text, pre = pre:sub(e + 4), nil

        begin_handing()

        if rule == "chunked" then chunks = dechunk(sink and hand_on) end
      end
    end

    if chunks then
      chunks.feed(text)
      got = chunks.got
    elseif text ~= "" then
      body[#body + 1] = text
      got = got + #text

      -- Never past the length the head gave: the body returned is cut
      -- there, so what is handed on is too.
      if sink then
        local piece = text

        if want then piece = text:sub(1, math.max(0, want - sent_on)) end

        sent_on = sent_on + #piece
        hand_on(piece)
      end
    end

    if progress and got - told >= 32768 then
      told = got
      progress(told, want)
    end
  end

  local function whole()
    if not head then return false end
    if chunks then return chunks.state == "done" or chunks.state == "bad" end
    if rule == "close" then return false end

    return got >= want
  end

  while not quiet_for(heard) do
    local text = stream:read()

    if text then
      take(text)
      heard = sys.ticks()
    end

    if whole() then break end

    local over, why_over = stream:done()

    if over then
      reason, ended = why_over, true
      break
    end

    if not text then pause(conn) end
  end

  --
  -- **Everything still in hand, not one read of it**, once it has ended by
  -- closing. Over TLS a read gives one record, and the connection can close
  -- with several decrypted and waiting: gnu.org arrived 24 KB of 30 on 30
  -- September, the rest left in the engine.
  --
  if ended then
    for _ = 1, 4096 do
      local last = stream:read()

      if not last then break end

      take(last)
    end
  end

  if not head then
    let_go({ conn = conn, stream = stream })

    local reply = head == false and table.concat(body) or pre

    if reply == "" then
      if was_kept then return nil, nil, how, true end

      return nil, reason or "nothing came back", how
    end

    how.ended = reason
    return reply, nil, how
  end

  local text = chunks and chunks.body() or table.concat(body)
  local over = chunks and chunks.extra or (want and #text > want)

  if want and #text > want then text = text:sub(1, want) end

  --
  -- **Cut short is said, and never passed on as whole.** A page that
  -- stalled arrived as its first part and was treated as all of it -
  -- Wikipedia's Dam article was 1,374,752 bytes of 1,435,447, and the
  -- parser refused what it was given. A length is held to; chunks are held
  -- to their last.
  --
  if chunks and chunks.state ~= "done" then
    how.short = { got = chunks.got }
  elseif rule == "length" and got < want then
    how.short = { got = got, want = want }
  end

  if progress then progress(#text, want) end

  --
  -- **Inflated, when it came gzipped** - in C, the Compression Kit's
  -- `gunzip`, each member held to its CRC and its length. What it inflates
  -- to is let as far as half the memory free and no further, so a small
  -- reply that would inflate to more than the machine has is refused
  -- rather than obeyed. A stream that stops or does not check out passes
  -- on what it inflated to, said to be cut short, as a body that stopped
  -- early is.
  --
  local coding = head:lower():match("\r\ncontent%-encoding:%s*([^\r\n]*)")

  -- Only a body there is: a 304 says the encoding of the one it stands for
  -- and brings none, and nothing inflated is not a stream cut short - which
  -- is what a page from the cache said, until it was told apart (6zz k).
  --
  -- Already inflated, as it came, when it was handed on and the stream
  -- came out whole: the pieces are the body, and inflating the whole of it
  -- again would be the same work twice. Anything else - a stream that
  -- stopped short or did not check out - is inflated here as it always was,
  -- so what is returned does not depend on whether anybody was handed it.
  --
  local streamed_whole = false

  if inflating and handed and #text > 0 then
    if inflating:finish() then
      how.gzip = #text
      text = table.concat(inflated)
      streamed_whole = true
    else
      handed = false
    end
  end

  if not streamed_whole and coding and coding:find("gzip", 1, true) and #text > 0 then
    local compress = use("/Kosmos/Kits/compress")
    local room = ((sys.info() or {}).pages_free or 16384) * 4096 // 2
    local inflated_now, why = compress.gunzip(text, room)

    if inflated_now then
      how.gzip = #text
      text = inflated_now

      if why then
        how.short = how.short or { got = #inflated_now }
        reason = reason or why
      end
    else
      reason = reason or why
    end
  end

  how.streamed = sink ~= nil and handed or nil

  if key and again and whole() and not ended and not over and not how.short then
    keep(key, conn, stream)
  else
    let_go({ conn = conn, stream = stream })
  end

  how.ended = reason
  return head .. text, nil, how
end

local function get_uncached(address, opts)
  opts = opts or {}

  local say = opts.say or function() end
  local parts, bad = address, nil

  if type(address) ~= "table" then parts, bad = http.split(address) end

  if not parts then return nil, bad, {} end

  local name = opts.name or parts.host

  -- Yes or no, or asked of each address - a browser opening pictures from
  -- hosts some of which it was told to open anyway.
  local anyway = opts.anyway

  if type(anyway) == "function" then anyway = anyway(parts) end

  local key = not (opts.anchors and #opts.anchors > 0)
              and table.concat({ parts.scheme, parts.hostport, name,
                                 anyway and "anyway" or "checked" }, " ")
              or nil

  -- Never a POST on a kept connection: one the server has closed is tried
  -- again, which for a form would be sending it twice.
  local k = key and opts.body == nil and take_kept(key)

  if k then
    local reply, why, how, again = exchange(parts, opts, k.conn, k.stream, key, true,
                                            { scheme = parts.scheme })

    if not again then return reply, why, how end
  end

  local how = { scheme = parts.scheme }
  local hz = (sys.info() or {}).tick_hz or 250
  local where, wrong = numbers(parts.host)

  if wrong then return nil, wrong, how end

  if not where then
    say("looking up " .. parts.host .. " ...")

    local found, why, said = lookup(parts.host, opts.wait_ticks, hz)

    if not found then
      return nil, ("cannot look up %s: %s"):format(parts.host, said or tostring(why)), how
    end

    where = found
  end

  say("connecting to " .. parts.hostport .. " ...")

  -- At once when something else schedules the waiting: the handshake goes
  -- on while the others' do, and what is written waits in the ring.
  local conn, why, said = fs.connect("/Network", where, parts.port, opts.pause ~= nil)

  if not conn then return nil, said or tostring(why), how end

  local stream = plain(conn)

  if parts.scheme == "https" then
    local tls = use("/Kosmos/Kits/tls")
    local anchors = http.authorities()

    for _, der in ipairs(opts.anchors or {}) do anchors[#anchors + 1] = der end

    local ok, t = pcall(tls.client, conn, name, { anchors = anchors,
                                                  insecure = anyway })

    if not ok then
      conn:close()
      return nil, tostring(t), how
    end

    stream = secure(conn, t)
  end

  return exchange(parts, opts, conn, stream, key, false, how)
end

--
-- **`http.get`, through the cache when it is given one** (`opts.cache`,
-- `httpcache.lua`, `roadmap.md` 6zz k). A reply kept and still fresh is
-- answered from it, with no request at all; one kept and stale is asked
-- about with its validators, and a `304` means the kept one is used again
-- for as long as the 304 says; a new `200` is kept if it says it may be.
-- `how.cached` is "fresh" or "revalidated" when the cache answered.
-- `opts.revalidate` asks even about a fresh one - Reload.
--
-- A POST is never answered from the cache, and nothing is without the
-- time of day, which is what freshness is measured against.
--
function http.get(address, opts)
  opts = opts or {}

  local c = opts.cache

  if c == nil or opts.body ~= nil then return get_uncached(address, opts) end

  local parts, bad = address, nil

  if type(address) ~= "table" then parts, bad = http.split(address) end

  if not parts then return nil, bad, {} end

  local clock = fs.read("/Devices/clock")
  local now = type(clock) == "table" and clock.epoch or nil

  if not now then return get_uncached(parts, opts) end

  local url = parts.scheme .. "://" .. parts.hostport .. parts.path
  local kept = c:lookup(url, now)

  if kept and kept.fresh and not opts.revalidate then
    c:used(url, now)

    return kept.reply, nil, { scheme = parts.scheme, cached = "fresh",
                              secure = parts.scheme == "https"
                                       and kept.secure or nil }
  end

  local asking = opts

  if kept and (kept.etag or kept.last_modified) then
    asking = setmetatable({ headers = { ["If-None-Match"] = kept.etag,
                                        ["If-Modified-Since"] = kept.last_modified } },
                          { __index = opts })
  end

  local reply, why, how = get_uncached(parts, asking)

  if reply then
    local status = tonumber(reply:match("^HTTP/%d%.%d%s+(%d%d%d)"))

    if status == 304 and kept then
      c:revalidated(url, kept, reply:match("^(.-)\r\n\r\n") or reply, now)
      how.cached = "revalidated"
      return kept.reply, nil, how
    elseif status == 200 then
      c:store(url, reply, how, now)
    end
  end

  return reply, why, how
end

--
-- **Several at once** (`roadmap.md` 6zz g): `http.get_many(addresses [,
-- opts])` -> a list of `{ reply, why, how }` in the same order.
--
-- gnu.org's page waited, it did not work: twelve pictures in a row, each a
-- lookup, a connection and a handshake to a server across an ocean, and the
-- processor idle for nine seconds of ten. Here each fetch is `http.get` in a
-- coroutine whose waiting yields the connection it waits on, and one
-- `fs.poll` over all of them wakes whichever have news - so the round trips
-- overlap rather than add up. Eight at a time, as browsers keep to about
-- that many at once; each finishing starts the next.
--
local AT_ONCE = 8

function http.get_many(addresses, opts)
  local results, waiting, queue = {}, {}, {}
  local hz = (sys.info() or {}).tick_hz or 250
  local tick = math.max(1, hz // 10)

  for i, address in ipairs(addresses) do queue[#queue + 1] = { at = i, address = address } end

  -- Each told as it ends, to a caller who can use it before the slowest
  -- is in - a page's pictures, decoded as they come (6zz l2).
  local each = opts and opts.each

  -- One step of one fetch: to its next wait, or to its end.
  local function step(job)
    local ok, a, b, c = coroutine.resume(job.co)

    if not ok then
      results[job.at] = { nil, tostring(a), {} }
      if each then each(job.at, nil, tostring(a), {}) end
    elseif coroutine.status(job.co) == "dead" then
      results[job.at] = { a, b, c }
      if each then each(job.at, a, b, c) end
    else
      job.conn = a
      waiting[#waiting + 1] = job
    end
  end

  -- As many running as there is room for: a fetch that ends on its first
  -- step - a name that will not resolve - makes room at once.
  local function fill()
    while #waiting < AT_ONCE and #queue > 0 do
      local job = table.remove(queue, 1)
      local mine = {}

      for k, v in pairs(opts or {}) do mine[k] = v end

      mine.pause = function(conn) coroutine.yield(conn) end
      job.co = coroutine.create(function() return http.get(job.address, mine) end)
      step(job)
    end
  end

  fill()

  --
  -- **Stepped by somebody else** (`roadmap.md` 6zz l2): given `opts.pause`,
  -- this waits by calling it, rather than in `fs.poll` - a window fetching
  -- a page's pictures between its own passes, and back at the next one. No
  -- poll answers at once, so then each fetch is simply stepped: a read of a
  -- ring that holds nothing is a look at two numbers, and quiet is measured
  -- in time, so stepping often gives nothing up sooner.
  --
  local outer = opts and opts.pause

  while #waiting > 0 do
    local ready

    if outer then
      outer()
      ready = {}
    else
      local conns = {}

      for _, job in ipairs(waiting) do conns[#conns + 1] = job.conn end

      ready = fs.poll("/Network", conns, {}, nil, tick) or {}
    end

    local news = {}

    for _, c in ipairs(ready) do news[c] = true end

    -- A pass with news steps those that have it; a quiet one steps them all,
    -- which is how each counts its own quiet towards giving up.
    local these = waiting
    waiting = {}

    for _, job in ipairs(these) do
      if #ready == 0 or news[job.conn] then
        step(job)
      else
        waiting[#waiting + 1] = job
      end
    end

    fill()
  end

  return results
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
