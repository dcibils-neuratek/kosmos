-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs sharing
-- This machine's folders shared over SMB, at the prompt.
--
--   sharing                              how it is
--   sharing account NAME PASSWORD        NAME's password kept, as its NT hash
--   sharing folders NAME:/Home/FOLDER ...
--                                        what is shared, in place of what was
--   sharing start ACCOUNT [port N] [name HOST]
--   sharing stop
--
-- `docs/sharing.md` N12c: smbd's own door, the one the Servers window holds,
-- reached the same way - a launcher hands it only to a program the image
-- serves that says `kosmos: needs sharing` above, and this file copied into
-- `/Home` and run from there is handed nothing. For a Telnet session and for
-- `make test`; the window is the way a person shares a folder.
--
-- **A password typed here is on the command line**, which a session's
-- scrollback keeps. The window asks for it in a field that shows dots.

local words = use("/Kosmos/Libraries/files.lua").words(args)
local verb = words[1]

if not fs.has_sharing() then
  print("sharing: this program was not handed smbd's door")
  return
end

local function said(r, why)
  if not r then
    print("sharing: " .. tostring(why))
    return false
  end

  return true
end

local function show(r)
  local line = ("sharing: %s"):format(r.state)

  if r.state ~= "stopped" then
    line = line .. (" on port %d as %s, seen as %s"):format(r.port, r.account,
                                                         r.hostname ~= "" and r.hostname or "KOSMOS")
  end

  line = line .. ("; %d folder%s, %d connected"):format(r.shares, r.shares == 1 and "" or "s",
                                                       r.connections)

  if r.state == "waiting" then
    line = line .. (" - the port would not open yet (%d)"):format(r.listen_error)
  end

  print(line)
end

if verb == nil or verb == "state" then
  local r, why = fs.sharing_state()
  if said(r, why) then show(r) end

elseif verb == "account" and words[2] and words[3] then
  local r, why = fs.sharing_account(words[2], words[3])
  if said(r, why) then print(("sharing: %s's password kept, as its NT hash"):format(words[2])) end

elseif verb == "folders" then
  local list = {}

  for i = 2, #words do
    local name, path = words[i]:match("^([^:]+):(/.*)$")

    if not name then
      print("sharing: a folder is NAME:/Home/FOLDER, not " .. words[i])
      return
    end

    list[#list + 1] = { name = name, path = path }
  end

  local r, why = fs.sharing_shares(list)
  if said(r, why) then show(r) end

elseif verb == "start" and words[2] then
  local o = { account = words[2] }
  local i = 3

  while words[i] do
    if words[i] == "port" then o.port = tonumber(words[i + 1])
    elseif words[i] == "name" then o.hostname = words[i + 1]
    else
      print("sharing: start ACCOUNT [port N] [name HOST], not " .. words[i])
      return
    end
    i = i + 2
  end

  local r, why = fs.sharing_start(o)
  if said(r, why) then show(r) end

elseif verb == "stop" then
  local r, why = fs.sharing_stop()
  if said(r, why) then show(r) end

else
  print("usage: sharing [account NAME PASSWORD | folders NAME:/Home/FOLDER ... |")
  print("                start ACCOUNT [port N] [name HOST] | stop]")
end
