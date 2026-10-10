-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Datagrams, from the prompt (`docs/sharing.md` N11a).
--
--   udp send 192.168.1.38 9000 hello       one datagram, and done
--   udp listen 9000                        each one that arrives, said
--   udp listen 5353 224.0.0.251            the same, a group joined
--   udp echo 9000 [GROUP]                  said, and sent back to its sender
--
-- What `ping` is for ICMP: the first thing to try when something speaking
-- UDP does not answer - multicast DNS, a time server, sound over the
-- network - because it separates the wire from the program in one line.
-- `echo` says hello to its group when it starts, so a listener on the far
-- side sees it arrive, and answers whatever comes; it is also what
-- `tools/run_udp.py` drives from the Mac.
--
-- Through the namespace (`fs.udp`), so this program never holds the
-- network's capability, as `ping` does not.
--

local ipv4 = use("/Kosmos/Libraries/ipv4.lua")

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local verb = words[1]

local function usage()
  print("udp: send ADDRESS PORT TEXT | listen PORT [GROUP] | echo PORT [GROUP]")
end

-- The bytes as text a person can read: printable as they are, the rest as
-- a byte's number.
local function shown(bytes)
  return (bytes:gsub("[^\32-\126]", function(c) return ("\\%d"):format(c:byte()) end))
end

if verb == "send" then
  local to, port, text = ipv4.bytes(words[2] or ""), tonumber(words[3]), table.concat(words, " ", 4)

  if not to or not port then return usage() end

  local sock, why = fs.udp("/Network", 0)

  if not sock then
    print("udp: " .. tostring(why))
    return
  end

  print(sock:send(to, port, text) and ("udp: sent %d bytes to %s:%d from %d"):format(
          #text, words[2], port, sock:port())
        or "udp: no room to send it")

  -- A moment for an address's question to be answered and the datagram to
  -- leave, before the port is let go with it still waiting.
  sys.sleep(50)
  sock:close()
  return
end

if verb ~= "listen" and verb ~= "echo" then return usage() end

local port = tonumber(words[2])
local group = words[3] and ipv4.bytes(words[3])

if not port or (words[3] and not group) then return usage() end

local sock, why = fs.udp("/Network", port, group)

if not sock then
  print("udp: " .. tostring(why))
  return
end

print(("udp: on port %d%s"):format(sock:port(), group and (", in " .. words[3]) or ""))

if verb == "echo" and group then
  sock:send(group, port, "hello from kosmos")
end

while true do
  local bytes, from, from_port = sock:receive()

  if bytes then
    print(("udp: from %s:%d, %d bytes: %s"):format(ipv4.text(from), from_port, #bytes, shown(bytes)))

    if verb == "echo" then sock:send(from, from_port, "echo: " .. bytes) end
  else
    sock:wait(250)
  end
end
