-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Shares on other machines, over SMB 2 and 3.
--
--   share connect smb://10.0.2.2:4450/Projects diego   sign in; asks the password
--   share probe 10.0.2.2:4450                          does a server answer there?
--   share status                                       what each server is doing
--   share disconnect 10.0.2.2:4450                     let it go
--
--   --no-wait after `connect` or `probe`: say it has begun, and come back
--
-- `docs/sharing.md` step N2: the client connects, and this is how a person
-- asks it to before any window does. It speaks to smbfs through the
-- namespace - `fs.share_*`, which `/Network`'s mount carries - and never
-- holds a capability of its own.
--
-- **smbfs answers at once and this asks again on its own clock**: `connect`
-- says the asking has begun, and the outcome is `fs.share_status`'s to tell,
-- asked a few times a second until it is known or smbfs's own bound has
-- passed. So a server that never answers costs this program ten seconds and
-- costs smbfs, and every other program asking it, nothing.
--
-- **The password is typed at the prompt, and shown as it is typed**: the
-- console has no way yet to read a line without echoing it. It crosses to
-- smbfs once, which keeps its NT hash and not the password; Connect to
-- Server's field (step N6) and the keyring (N8) are where it stops being
-- typed here at all.

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local wait = true

for i = #words, 1, -1 do
  if words[i] == "--no-wait" then
    wait = false
    table.remove(words, i)
  end
end

local verb = words[1]

local function usage()
  print("usage: share connect smb://server[:port]/share account [--no-wait]")
  print("       share probe server[:port] [--no-wait]")
  print("       share status")
  print("       share disconnect server[:port]")
end

-- "smb://10.0.2.2:4450/Projects", "10.0.2.2:4450/Projects", "nas": the
-- server with its port as typed, and the share after the slash.
local function split(text)
  local rest = tostring(text or ""):gsub("^smb://", "")
  local server, share = rest:match("^([^/]+)/?(.*)$")

  if not server then return nil end

  share = (share or ""):gsub("/+$", "")
  return server, share
end

local function seconds(ms)
  return string.format("%.1f s", (ms or 0) / 1000)
end

-- One server, as a line.
local function line(s)
  local parts = { s.address, s.state }

  if s.state == "connected" or s.state == "answered" then
    parts[#parts + 1] = (s.name ~= "" and s.name or "a server")
    parts[#parts + 1] = "SMB " .. tostring(s.dialect or "?")
                        .. (s.signing and ", signed" or "")
                        .. (s.sealing and ", sealed" or "")
  end

  if s.state == "connected" then
    parts[#parts + 1] = s.share .. " as " .. s.account
  elseif s.state == "refused" or s.state == "away" then
    parts[#parts + 1] = s.why
  end

  parts[#parts + 1] = "for " .. seconds(s.in_state_ms)
  return table.concat(parts, "  ")
end

local function find(address)
  local list, why = fs.share_status()

  if not list then return nil, why end

  for _, s in ipairs(list) do
    if s.address == address then return s end
  end

  return nil, "smbfs has forgotten " .. address
end

-- Asked again until it is known: smbfs gives up on a server after ten
-- seconds, and this gives smbfs five more before saying so itself.
local function outcome(address)
  for _ = 1, 15 * 10 do
    local s, why = find(address)

    if not s then return nil, why end
    if s.state ~= "asking" then return s end

    sys.sleep(25)                       -- a tenth of a second
  end

  return nil, "smbfs did not say how " .. address .. " went"
end

local function report(address)
  if not wait then
    print("share: asking " .. address .. "; `share status` says how it goes")
    return
  end

  local s, why = outcome(address)

  if not s then
    print("share: " .. tostring(why))
  elseif s.state == "connected" then
    print(("share: %s answered - %s, SMB %s%s%s; %s connected as %s")
          :format(s.address, s.name ~= "" and s.name or "a server",
                  tostring(s.dialect), s.signing and ", signed" or "",
                  s.sealing and ", sealed" or "", s.share, s.account))
  elseif s.state == "answered" then
    print(("share: %s answered - SMB %s%s"):format(s.address, tostring(s.dialect),
          s.signing and ", signing required" or ""))
  else
    print("share: " .. s.why)
  end
end

if verb == "connect" then
  local server, share = split(words[2])
  local account = words[3]

  if not server or share == "" or not account then
    usage()
    return
  end

  print("password for " .. account .. " at " .. server
        .. " (shown as it is typed):")

  local password = fs.read("/Devices/console") or ""
  password = password:gsub("[\r\n]+$", "")

  local ok, why = fs.share_connect(server, share, account, password)
  password = nil

  if not ok then
    print("share: " .. tostring(why))
    return
  end

  report(server)
elseif verb == "probe" then
  local server = split(words[2])

  if not server then
    usage()
    return
  end

  local ok, why = fs.share_probe(server)

  if not ok then
    print("share: " .. tostring(why))
    return
  end

  report(server)
elseif verb == "status" then
  local list, why = fs.share_status()

  if not list then
    print("share: " .. tostring(why))
  elseif #list == 0 then
    print("share: nothing has been asked of any server")
  else
    for _, s in ipairs(list) do print(line(s)) end
  end
elseif verb == "disconnect" then
  local server = split(words[2])

  if not server then
    usage()
    return
  end

  local ok, why = fs.share_disconnect(server)
  print(ok and ("share: " .. server .. " let go") or ("share: " .. tostring(why)))
else
  usage()
end
