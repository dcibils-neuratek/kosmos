-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- An IPv4 address, between the four bytes the network carries and the
-- four numbers a person writes.
--
--   local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
--   ipv4.text("\10\0\2\15")            "10.0.2.15"; "?" for what is not one
--   ipv4.text(bytes, "")               "" instead, where a field shows it
--   ipv4.bytes("10.0.2.15")            four bytes, or nil
--   ipv4.given(info.address)           four bytes, and not 0.0.0.0
--   ipv4.neighbour(from, info)         on the network `info` says this is on
--
-- **Text is a presentation**, as `ping` says: the stack carries four bytes
-- in the order they go on the wire, and how a person writes them is the
-- program's business - which is why the Network Kit does not do this. So
-- each program did it itself: `dotted` was written out in `host`, `ping`,
-- `httpd`, `telnetd`, `vncd`, `neofetch`, Network and Preferences, and four
-- numbers read back into bytes in `telnet`, `ping` and Network. One copy,
-- here (`CLAUDE.md`, *Kits, servers and drivers supply*).

local ipv4 = {}

-- Whether `bytes` are an address at all: four of them.
local function four(bytes)
  return type(bytes) == "string" and #bytes == 4
end

--
-- Four bytes as four numbers with dots, or `otherwise` - "?" unless said -
-- for anything that is not four bytes. What stands in for a missing
-- address is the caller's: a log says "?", a field shows nothing.
--
function ipv4.text(bytes, otherwise)
  if not four(bytes) then
    if otherwise == nil then return "?" end

    return otherwise
  end

  return ("%d.%d.%d.%d"):format(bytes:byte(1, 4))
end

--
-- Four numbers with dots, each under 256, as four bytes; nil for anything
-- else. Spaces at either end are allowed, since a field a person typed in
-- has them.
--
function ipv4.bytes(text)
  local a, b, c, d = tostring(text or ""):match("^%s*(%d+)%.(%d+)%.(%d+)%.(%d+)%s*$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d)
end

--
-- **An address the machine has been given**: four bytes that are not all
-- zero, which is what the stack answers until DHCP or a person has said
-- one.
--
function ipv4.given(bytes)
  return four(bytes) and bytes ~= "\0\0\0\0"
end

--
-- **Whether `from` is on this machine's own network**: the same bits under
-- the mask as this machine's address, as `info` - what `fs.net_info` says -
-- has them. False for anything that is not four bytes, on either side.
-- What `telnetd` and `vncd` let in, Diego's "no key, local network only".
--
function ipv4.neighbour(from, info)
  local mine, mask = info and info.address, info and info.netmask

  if not (four(from) and four(mine) and four(mask)) then return false end

  for i = 1, 4 do
    if (from:byte(i) & mask:byte(i)) ~= (mine:byte(i) & mask:byte(i)) then
      return false
    end
  end

  return true
end

return ipv4
