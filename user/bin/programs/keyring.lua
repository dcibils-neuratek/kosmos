-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs keyring
-- What the keyring keeps, at the prompt.
--
--   keyring                 how many, and how the file opened; then each entry
--   keyring show 3          entry 3's password, shown
--   keyring forget 3        entry 3 forgotten
--   keyring get <service> <account>
--                           refused: only smbfs's door may get a password,
--                           and this asks to show that it is
--
-- `docs/keyring.md`, step K4: the keyring's `manage` door, the one Passwords
-- holds (K6), reached the same way - a launcher hands it only to a program
-- the image serves that says `kosmos: needs keyring` above, and this file
-- copied into `/Home` and run from there is handed nothing. It lists, shows
-- and forgets; it cannot put a password in, which only smbfs's door can.

local words = use("/Kosmos/Libraries/files.lua").words(args)
local verb, which = words[1], tonumber(words[2])

if not fs.has_keyring() then
  print("keyring: this program was not handed the keyring")
  return
end

local function date(unix)
  if not unix or unix == 0 then return "never" end
  return os.date("%Y-%m-%d %H:%M", unix)
end

if verb == nil then
  local state, why = fs.keyring_state()

  if not state then
    print("keyring: " .. tostring(why))
    return
  end

  print(("%d %s, the file %s"):format(state.count,
                                      state.count == 1 and "entry" or "entries",
                                      state.file))

  for _, e in ipairs(fs.keyring_list() or {}) do
    print(("%4d  %-4s %-28s %-16s used %s, %d times%s"):format(
      e.id, e.kind, e.service, e.account, date(e.used), e.uses,
      e.at_start and ", at start" or ""))
  end
elseif (verb == "show" or verb == "forget") and which then
  if verb == "show" then
    local secret, why = fs.keyring_reveal(which)

    print(secret and secret or ("keyring: " .. tostring(why)))
  else
    local ok, why = fs.keyring_forget(which)

    print(ok and ("forgot entry " .. which) or ("keyring: " .. tostring(why)))
  end
elseif verb == "get" and words[3] then
  local secret, why = fs.keyring_get(words[2], words[3])

  print(secret and "keyring: the manage door was answered a secret" or
        ("keyring: " .. tostring(why)))
else
  print("usage: keyring | keyring show <id> | keyring forget <id>")
end
