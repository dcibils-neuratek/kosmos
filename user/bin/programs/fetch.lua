-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One HTTP request, and whatever comes back - over TLS too.
--
--   fetch https://example.com/
--   fetch http://10.0.2.2:8000/hello
--   fetch https://10.0.2.2:8443/ --name test.local --cacert /Home/ca.der
--   fetch 10.0.2.2 8000 /                      (the form it had first)
--
-- **The smallest thing that exercises a whole connection**: open it, send
-- bytes, read bytes, notice the far end hang up. Telnet needs a person and
-- a keyboard; this needs neither, which is what makes it the thing a test
-- can drive.
--
-- HTTP/1.0 on purpose. 1.1 keeps the connection open and would need this to
-- understand `Content-Length` or chunked encoding to know when to stop;
-- 1.0's answer is that the server closes, which is exactly the event the
-- ring's `closed` flag reports. A protocol that ends by ending is the right
-- one to test a stack with.
--
-- **`https://` through the TLS Kit** (`roadmap.md`, the browser: TLS, step
-- 4): the same request, written into a TLS connection laid over the TCP
-- one, the certificate held to Mozilla's roots and the machine's clock.
-- Two options, named after curl's: `--cacert FILE`, a certificate in DER
-- to trust besides the roots - a test's own authority, or a network's; and
-- `--name NAME`, the name to hold the certificate to and send, for an
-- address given rather than a name.

local words, named, cacert = {}, nil, nil
local given = {}

for w in tostring(args or ""):gmatch("%S+") do given[#given + 1] = w end

do
  local i = 1

  while i <= #given do
    if given[i] == "--name" then
      named, i = given[i + 1], i + 2
    elseif given[i] == "--cacert" then
      cacert, i = given[i + 1], i + 2
    else
      words[#words + 1], i = given[i], i + 1
    end
  end
end

local function address(text)
  local a, b, c, d = tostring(text or ""):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d)
end

local scheme, host, port, path = tostring(words[1] or ""):match("^(https?)://([^/:]+):?(%d*)(/?.*)$")

if scheme then
  port = tonumber(port) or (scheme == "https" and 443 or 80)
  path = (path ~= "") and path or "/"
else
  host, port, path = words[1], tonumber(words[2]) or 80, words[3] or "/"
end

local where = address(host)

if not where and host and host ~= "" then
  local hz = (sys.info() or {}).tick_hz or 250
  local found, why, said = fs.resolve(host, 5 * hz)

  if not found then
    print("fetch: " .. host .. ": " .. (said or tostring(why)))
    return
  end

  where = found
end

if not where then
  print("fetch: fetch https://name/path, or fetch <address> [port] [path]")
  return
end

local conn, why, said = fs.connect("/Network", where, port)

if not conn then
  print("fetch: " .. (said or tostring(why)))
  return
end

--
-- The connection a request goes over: the TCP one as it is, or TLS laid on
-- it. Both answer `write`, `read` and whether they are done, so what follows
-- does not care which.
--
local stream = conn

if scheme == "https" then
  local tls = use("/Kosmos/Kits/tls")
  local anchors = {}

  if cacert then
    local der = fs.read(cacert)

    if type(der) ~= "string" or der == "" then
      print("fetch: --cacert " .. cacert .. ": no such file")
      conn:close()
      return
    end

    anchors[1] = der
  end

  local ok, t = pcall(tls.client, conn, named or host, { anchors = anchors })

  if not ok then
    print("fetch: " .. tostring(t))
    conn:close()
    return
  end

  stream = {
    write = function(_, s) return t:write(s) end,
    read = function() return t:read() end,
    flush = function() t:flush() end,
    done = function()
      local state, reason, code = t:state()

      if state == "closed" then return true, (code ~= 0) and reason or nil end

      -- The connection's bytes are the engine's to read, never this: a
      -- closed connection is enough, and the read after the loop takes
      -- what was left in the engine.
      return conn:closed(), nil
    end,
    close = function() t:close() end,
  }
else
  stream = {
    write = function(_, s) return conn:write(s) end,
    read = function() return conn:read() end,
    flush = function() end,
    done = function() return conn:closed(), nil end,
    close = function() end,
  }
end

--
-- `Host` because every server since 1.1 wants one even from a 1.0 client,
-- and `Connection: close` because saying so is politer than relying on the
-- version to imply it.
--
local request = ("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n")
                :format(path, named or host)

--
-- Written as it is taken: over TLS nothing is taken until the handshake is
-- done, so a request waits for it here, and a handshake that ends instead
-- says why.
--
local sent = 0

for _ = 1, 400 do
  if sent >= #request then break end

  local n = stream:write(request:sub(sent + 1))

  sent = sent + n

  if n == 0 then
    local over, reason = stream:done()

    if over then
      print("fetch: " .. (reason or "the connection closed before the request went"))
      conn:close()
      return
    end

    conn:wait(25)
  end
end

stream:flush()

if sent < #request then
  print(("fetch: only %d of %d bytes went"):format(sent, #request))
end

--
-- Until the far end closes.
--
-- `wait` blocks, which is what keeps this from being a spin - and the read
-- after the loop is not redundant: the close and the last bytes can arrive
-- in the same segment, and a program that stopped at `closed` would lose
-- them. That is why `tcpring.h` says the flag means "no more will arrive"
-- rather than "stop".
--
local body = {}
local total = 0
local reason

for _ = 1, 400 do
  local text = stream:read()

  if text then
    body[#body + 1] = text
    total = total + #text
  end

  local over, why_over = stream:done()

  if over then
    reason = why_over
    break
  end

  if not text then conn:wait(25) end
end

local last = stream:read()

if last then
  body[#body + 1] = last
  total = total + #last
end

stream:close()
conn:close()

if reason then
  print("fetch: " .. reason)
end

local text = table.concat(body)

print(("%d bytes"):format(total))
print(text)
