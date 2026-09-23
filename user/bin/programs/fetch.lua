-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One HTTP request, and whatever comes back.
--
--   fetch 10.0.2.2 8000 /
--   fetch 93.184.216.34 80 /index.html
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

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local function address(text)
  local a, b, c, d = tostring(text or ""):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d)
end

local where = address(words[1])
local port = tonumber(words[2]) or 80
local path = words[3] or "/"

if not where then
  print("fetch: fetch <address> [port] [path]")
  return
end

local conn, why = fs.connect("/net", where, port)

if not conn then
  local said = ({ [4] = "no route to it", [7] = "connection refused",
                  [9] = "timed out", [5] = "too many connections open" })[why]

  print("fetch: " .. (said or tostring(why)))
  return
end

--
-- `Host` because every server since 1.1 wants one even from a 1.0 client,
-- and `Connection: close` because saying so is politer than relying on the
-- version to imply it.
--
local request = ("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n")
                :format(path, words[1])

local wrote = conn:write(request)

if wrote < #request then
  print(("fetch: only %d of %d bytes fitted"):format(wrote, #request))
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

for _ = 1, 200 do
  local text = conn:read()

  if text then
    body[#body + 1] = text
    total = total + #text
  end

  if conn:closed() then break end

  conn:wait(25)
end

local last = conn:read()

if last then
  body[#body + 1] = last
  total = total + #last
end

conn:close()

local text = table.concat(body)

print(("%d bytes"):format(total))
print(text)
