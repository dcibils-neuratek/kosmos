-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Talk to a machine over TCP.
--
--   telnet 10.0.2.2 23
--   telnet 93.184.216.34 80
--
-- **A line protocol, which is why it is the first thing built on TCP.**
-- Every mistake a stack can make shows up here as text that does not arrive
-- or arrives twice, and there is nothing else in the way to blame. SSH is
-- the same path with cryptography on it; getting this right first means a
-- broken handshake later is the cryptography and not the sequence numbers.
--
-- **This is not a terminal.** Real telnet negotiates options - window size,
-- echo, line mode - and this sends what you type and prints what comes
-- back. That is enough to speak to an SMTP server, an HTTP server or a
-- daemon that prints a banner, which is what it is for. Option negotiation
-- is a protocol of its own and belongs in a program that means to be a
-- terminal.

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
local port = tonumber(words[2]) or 23

if not where then
  print("telnet: telnet <address> [port]")
  print("        four numbers and three dots; there is no DNS yet")
  return
end

print(("connecting to %s port %d"):format(words[1], port))

local conn, why = fs.connect("/net", where, port)

if not conn then
  --
  -- The refusals worth telling apart, by the numbers `netproto.h` gives
  -- them. "Refused" and "timed out" are different facts about the far end
  -- and a person debugging needs to know which.
  --
  local said = ({ [4] = "no route to it", [7] = "connection refused",
                  [9] = "timed out", [5] = "too many connections open" })[why]

  print("telnet: " .. (said or tostring(why)))
  return
end

print("connected. Control-C to stop.")

--
-- What the person types goes out; what arrives comes in.
--
-- The loop blocks in `conn:wait`, which is what stops this being a spin: a
-- program polling `read` never blocks and costs a core, which is the
-- measurement that put a deadline in every server's receive.
--
-- Reading the keyboard is the part that has to be a poll, because the
-- console has one reader and no way to wait on two things at once. That is
-- the same missing `select` the query watch ran into, written down twice
-- now, and it is the thing to build when something needs it a third time.
--
local line = ""

while not conn:closed() do
  local text = conn:read()

  if text then io.write(text) end

  if fs.interrupted and fs.interrupted("/dev/console") then
    print("")
    break
  end

  local key = fs.keys and fs.keys("/dev/console")

  for _, code in ipairs(key or {}) do
    if code == 13 or code == 10 then
      conn:write(line .. "\r\n")
      line = ""
    elseif code == 8 or code == 127 then
      line = line:sub(1, -2)
    elseif code >= 32 and code < 127 then
      line = line .. string.char(code)
    end
  end

  conn:wait(5)
end

local last = conn:read()

if last then fs.write("/dev/console", last) end

conn:close()
print("")
print("connection closed")
