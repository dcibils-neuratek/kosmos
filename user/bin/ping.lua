-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Is that machine there?
--
--   ping 8.8.8.8              four echoes
--   ping 8.8.8.8 10           ten of them
--   ping                      the gateway
--
-- The oldest question on a network and still the useful one: it separates
-- "the wire is broken" from "the program is wrong" in one command, which is
-- why it is the first thing built on the stack rather than something added
-- later for completeness.
--
-- **The round trip is in counter ticks until this program divides it.**
-- `/net` reports what it measured and `/dev/cpu` says how fast the counter
-- runs, because that is 62.5 MHz under QEMU's TCG and 24 MHz when the same
-- machine runs natively under `hvf`. A stack that converted to milliseconds
-- would have baked in one of them, and the number would be silently four
-- times wrong on the other.

--
-- An address, from four numbers to four bytes.
--
-- Written here rather than in the kit because text is a *presentation*: the
-- protocol carries four bytes in the order they go on the wire, and how a
-- person writes them is this program's business. The same division `gfx`
-- draws by taking a colour and not a colour name.
--
local function address(text)
  local a, b, c, d = tostring(text or ""):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d)
end

-- The two refusals worth telling apart, by the numbers `netproto.h` gives
-- them. Named here rather than reached through the kit, because this program
-- does not load the kit - the namespace does.
local ERR_NO_ROUTE, ERR_UNREACHABLE = 3, 4

local function dotted(bytes)
  if type(bytes) ~= "string" or #bytes ~= 4 then return "?" end

  return ("%d.%d.%d.%d"):format(bytes:byte(1, 4))
end

--------------------------------------------------------------------------

--
-- Through the namespace, not a capability.
--
-- `fs.ping` resolves `/net`, checks that what is mounted there really is a
-- network stack, and hands the kit the capability - so this program never
-- holds one. That is the rule the whole system runs on and it is why there
-- is no `fs.capability`: a program that could ask for a raw capability by
-- path could reach past whoever decided what to mount for it.
--
local info, why = fs.net_info("/net")

if not info then
  print("ping: " .. tostring(why))
  return
end

if not info.card then
  print("ping: this machine has no network card")
  return
end

--
-- Where to send it.
--
-- With no argument, the gateway - which is the useful default: it is the
-- first thing that can be wrong and the only address the machine is certain
-- to have been told about.
--
local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local target = words[1] and address(words[1]) or info.gateway
local count = tonumber(words[2] or words[1]) or 4

if words[1] and not address(words[1]) and not tonumber(words[1]) then
  print("ping: " .. words[1] .. " is not an address this understands")
  print("      four numbers and three dots; there is no DNS yet")
  return
end

if target == "\0\0\0\0" then
  print("ping: there is no gateway configured, and no address was given")
  return
end

--
-- The counter's frequency, read rather than assumed. See the note at the
-- top: this is the number that differs between TCG and hvf.
--
local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

print(("PING %s from %s, 56 bytes"):format(dotted(target),
                                           dotted(info.address)))

--
-- Fifty-six bytes, which is what every other ping sends.
--
-- Not arbitrary: it makes a 64-byte ICMP message and an 84-byte IP packet,
-- which are the numbers a person recognises in the output of the tool they
-- already know. A payload that is *content* rather than zeroes also means a
-- corrupted reply is visible as text rather than as a length.
--
local PAYLOAD = ("kosmos ping payload, fifty-six bytes of it, padded"):sub(1, 56)
PAYLOAD = PAYLOAD .. ("."):rep(56 - #PAYLOAD)

local sent, got = 0, 0
local best, worst, total = nil, nil, 0

for seq = 1, count do
  sent = sent + 1

  local reply, err = fs.ping("/net", target, seq, PAYLOAD)

  if reply then
    -- Milliseconds to two places, from ticks. Integer arithmetic until the
    -- last step, because `ticks` is a count and dividing early loses it.
    local us = reply.ticks * 1000000 // hz

    got = got + 1
    total = total + us
    best = (not best or us < best) and us or best
    worst = (not worst or us > worst) and us or worst

    print(("%d bytes from %s: seq=%d ttl=%d time=%d.%03d ms")
          :format(reply.bytes + 8, dotted(reply.from), reply.seq,
                  reply.ttl, us // 1000, us % 1000))
  elseif err == ERR_UNREACHABLE then
    print(("seq=%d: no answer"):format(seq))
  elseif err == ERR_NO_ROUTE then
    print("ping: this machine has no address yet")
    break
  else
    print(("seq=%d: %s"):format(seq, tostring(err)))
  end

  -- Once a second, like every other ping, and for the same reason: it is
  -- slow enough to read and slow enough not to be a flood.
  if seq < count then sys.sleep(250) end
end

print("")
print(("--- %s ping statistics ---"):format(dotted(target)))
print(("%d sent, %d received, %d%% lost")
      :format(sent, got, (sent > 0) and ((sent - got) * 100 // sent) or 0))

if got > 0 then
  local mean = total // got

  print(("round trip min/avg/max = %d.%03d/%d.%03d/%d.%03d ms")
        :format(best // 1000, best % 1000, mean // 1000, mean % 1000,
                worst // 1000, worst % 1000))
end
