-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What address is that name?
--
--   host example.com          the address, or why not
--   host example.com 5        give up after five seconds
--
-- The other half of `ping`. That one answers "is the machine there"; this
-- one answers "which machine did you mean", and between them a person can
-- tell a broken name from a broken route without guessing.
--
-- **The resolver was built and nothing could reach it.** `/net` has spoken
-- DNS for as long as it has spoken TCP - a query, a reply, names written
-- with a length in front of each label, and the compression pointers a real
-- server answers with - and the only caller was the browser's address bar.
-- So a lookup could not be tried on its own, a failure could not be told
-- from a routing failure, and `make test` had nothing to check. Three
-- separate consequences of there being no command for it.

--
-- What `/net` can say about a lookup, by the numbers `netproto.h` gives
-- them.
--
-- Named here rather than reached through the kit, because this program does
-- not load the kit - the namespace does, and hands back a number. Only the
-- refusals *this* operation can produce: the header has thirteen and a
-- resolve can end in seven of them, and listing the rest would read as
-- thoroughness while being six more lines to keep in step with a file that
-- can change.
--
local ERRORS = {
  [2]  = "this machine has no network card",
  [3]  = "no route to the resolver",
  [5]  = "the stack has no room for another question",
  [6]  = "that is not a name this understands",
  [9]  = "the resolver did not answer in time",
  [11] = "no resolver is configured; set one in Network preferences",
  [12] = "no such name",
}

local function dotted(bytes)
  if type(bytes) ~= "string" or #bytes ~= 4 then return "?" end

  return ("%d.%d.%d.%d"):format(bytes:byte(1, 4))
end

--------------------------------------------------------------------------

--
-- Through the namespace, not a capability.
--
-- `fs.resolve` finds `/net`, checks that what is mounted there really is a
-- network stack, and hands the kit the capability - so this program never
-- holds one. The same rule `ping` obeys and the same reason: a program that
-- could ask for a raw capability by path could reach past whoever decided
-- what to mount for it.
--
local info, why = fs.net_info("/net")

if not info then
  print("host: " .. tostring(why))
  return
end

if not info.card then
  print("host: this machine has no network card")
  return
end

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local name = words[1]

if not name then
  print("host: which name?")
  print("      host example.com")
  return
end

--
-- A name that is already an address is answered without asking anybody.
--
-- Not a shortcut: it is the correct answer, and it is what stops
-- `host 10.0.2.2` from failing on a machine whose resolver is wrong. The
-- same courtesy every resolver library extends, and the reason the check is
-- four numbers and three dots rather than "does it contain a dot".
--
if name:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$") then
  print(name .. " is an address already")
  return
end

--
-- The wait, in *scheduler* ticks.
--
-- `/net` counts a timeout the way `sys.sleep` does, and `tick_hz` says how
-- many of those go in a second - 250 today and not a number to write out,
-- for the reason `wmproto.lua` gives at length: this system has two clocks
-- and the one a timeout is in is not the one `sys.ticks()` returns.
--
local hz = (sys.info() or {}).tick_hz or 250
local seconds = tonumber(words[2]) or 5

local address, err = fs.resolve(name, math.floor(seconds * hz))

if not address then
  print("host: " .. name .. ": " .. (ERRORS[err] or ("error " .. tostring(err))))
  return
end

print(name .. " is " .. dotted(address))
