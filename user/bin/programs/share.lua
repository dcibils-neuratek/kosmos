-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Shares on other machines, over SMB 2 and 3.
--
--   share connect smb://10.0.2.2:4450/Projects diego   sign in; asks the password
--   share connect smb://10.0.2.2:4450 diego            sign in, no share yet
--   share shares 10.0.2.2:4450                         what a server signed into offers
--   share probe 10.0.2.2:4450                          does a server answer there?
--   share status                                       what each server is doing
--   share retry 10.0.2.2:4450                          Try now: one that is away
--   share disconnect 10.0.2.2:4450                     let it go, away or not
--
--   --no-wait after `connect` or `probe`: say it has begun, and come back
--   --remember after `connect`: once the server takes the password, the
--     keyring keeps it, and the share connects when Kosmos starts
--   share connect smb://10.0.2.2:4450/Projects         no account: the one
--     the keyring remembers there, with its password - nothing asked
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
-- console has no way to read a line without echoing it. It crosses to
-- smbfs once, which keeps its NT hash and not the password. **Connect to
-- Server** (step N6, Tracker's Network group) is the door where it is not
-- shown - its field draws a star a character - and the keyring (N8) where
-- it stops being typed at all.
--
-- **Why the prompt did not gain an unechoed read at N6**: the console's
-- line is read by three servers that each implement the same protocol -
-- `console.c`, the Terminal and `telnetd`'s sessions - and a read that does
-- not echo is a change to `conproto.h` and to all three, for one program
-- whose password now has a window. Said in `docs/sharing.md`, N6.
--
-- **Several shares on one connection** (N6): a second `connect` to a
-- server already signed into, as the same account, asks for the share on
-- the same session and for no password; one with no share signs in alone,
-- and `shares` says what the server offers.
--
-- **A server that goes away is kept** (step N5): `status` says since when,
-- and when smbfs tries it again by itself; `retry` is the mockup's Try now,
-- signing in from what smbfs kept, with nothing asked; and `disconnect`
-- forgets one that is away as it does one that is connected.

-- A share whose name has a space in it - `"diego’s Public Folder"` - is
-- one word when it is quoted (`files.words`, `testing.md` 18.414).
local words = use("/Kosmos/Libraries/files.lua").words(args)

local wait, remember = true, false

for i = #words, 1, -1 do
  if words[i] == "--no-wait" then
    wait = false
    table.remove(words, i)
  elseif words[i] == "--remember" then
    remember = true
    table.remove(words, i)
  end
end

local verb = words[1]

local function usage()
  print("usage: share connect smb://server[:port][/share] [account] [--remember] [--no-wait]")
  print("       share shares server[:port]")
  print("       share probe server[:port] [--no-wait]")
  print("       share status")
  print("       share retry server[:port]")
  print("       share disconnect server[:port]")
end

-- "smb://10.0.2.2:4450/Projects", "10.0.2.2:4450/Projects", "nas": the
-- server with its port as typed, and the share after the slash - read as
-- Connect to Server reads it, by the one library that does.
local netshares = use("/Kosmos/Libraries/netshares.lua")

local function split(text)
  local server, share = netshares.split(text)

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
    parts[#parts + 1] = (s.share ~= "" and s.share or "signed in") .. " as " .. s.account

    if (s.sign_ins or 0) > 1 then
      parts[#parts + 1] = ("signed in %d times"):format(s.sign_ins)
    end
  elseif s.state == "refused" or s.state == "away" then
    parts[#parts + 1] = s.why
  end

  -- Away and kept: since when, and when it is next tried (`docs/sharing.html`,
  -- *Gone away*: "retrying", and the next try's time).
  if s.state == "away" and s.trying then
    parts[#parts + 1] = "since " .. seconds(s.in_state_ms) .. ", trying again now"
  elseif s.state == "away" and (s.next_try_ms or 0) > 0 then
    parts[#parts + 1] = ("since %s, next try in %d s")
                        :format(seconds(s.in_state_ms), (s.next_try_ms + 999) // 1000)
  else
    parts[#parts + 1] = "for " .. seconds(s.in_state_ms)
  end

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
    if s.state ~= "asking" and not s.trying then return s end

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
    print(("share: %s answered - %s, SMB %s%s%s; %s as %s")
          :format(s.address, s.name ~= "" and s.name or "a server",
                  tostring(s.dialect), s.signing and ", signed" or "",
                  s.sealing and ", sealed" or "",
                  s.share ~= "" and (s.share .. " connected") or "signed in",
                  s.account))
  elseif s.state == "answered" then
    print(("share: %s answered - SMB %s%s"):format(s.address, tostring(s.dialect),
          s.signing and ", signing required" or ""))
  else
    print("share: " .. s.why)
  end
end

-- Signed into already, as this account? Then a share more is asked for on
-- the same session, and no password (N6).
local function signed_in(server, account)
  for _, s in ipairs(fs.share_status() or {}) do
    if s.address == server and s.state == "connected" and s.account == account then
      return true
    end
  end

  return false
end

-- A share asked for on a server signed into: connected, or why not.
local function report_share(server, share)
  for _ = 1, 15 * 10 do
    local list = fs.share_shares(server) or {}

    for _, one in ipairs(list) do
      if one.name:lower() == share:lower() and one.state == "connected" then
        print(("share: %s connected on the same session"):format(one.name))
        return
      end

      if one.name:lower() == share:lower() and one.state == "refused" then
        local s = find(server)
        print("share: " .. tostring(s and s.why or "refused"))
        return
      end
    end

    sys.sleep(25)
  end

  print("share: " .. share .. " did not connect")
end

if verb == "connect" then
  local server, share = split(words[2])
  local remembered = server and fs.share_remembered(server)
  local account = words[3] or remembered

  if not server or not account then
    usage()
    return
  end

  if signed_in(server, account) then
    local ok, why = fs.share_connect(server, share, account, "")

    if not ok then
      print("share: " .. tostring(why))
    elseif share ~= "" and wait then
      report_share(server, share)
    else
      print("share: asked " .. server .. " for " .. (share ~= "" and share or "its shares"))
    end

    return
  end

  -- Remembered (`keyring.md`, K5): no password asked, and none sent -
  -- smbfs takes the keyring's.
  local password = ""

  if remembered and account == remembered and not remember then
    print("share: signing in as " .. account .. " with the remembered password")
  else
    print("password for " .. account .. " at " .. server
          .. " (shown as it is typed):")
    password = (fs.read("/Devices/console") or ""):gsub("[\r\n]+$", "")
  end

  local ok, why = fs.share_connect(server, share, account, password, remember)
  password = nil

  if not ok then
    print("share: " .. tostring(why))
    return
  end

  report(server)
elseif verb == "shares" then
  local server = split(words[2])

  if not server then
    usage()
    return
  end

  -- Asked again until the server's own list is heard, a few times a second.
  local list, why

  for _ = 1, 15 * 10 do
    list, why = fs.share_shares(server)

    if not list or list.known then break end

    sys.sleep(25)
  end

  if not list then
    print("share: " .. tostring(why))
  elseif #list == 0 then
    print("share: " .. server .. " offers no shares")
  else
    for _, one in ipairs(list) do
      print(("%s  %s"):format(one.name, one.state))
    end

    if not list.known then print("share: its own list was not heard yet") end
  end
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
elseif verb == "retry" then
  local server = split(words[2])

  if not server then
    usage()
    return
  end

  local ok, why = fs.share_retry(server)

  if not ok then
    print("share: " .. tostring(why))
    return
  end

  report(server)
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
