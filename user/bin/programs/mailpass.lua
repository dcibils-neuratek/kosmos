-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs keyring-mail
-- Mail's passwords, at the prompt, through the keyring's `mail` door.
--
--   mailpass                                  every mail account kept
--   mailpass keep <service> <account> <password> [title]
--   mailpass check <service> <account> <password>
--                                             whether that is the one kept -
--                                             never the password itself
--   mailpass forget <id>
--
-- `docs/mail.md` M0 (Diego, 8 October: "as recommended, go ahead and build
-- it"): the door `maild` will hold, lent only to a program the image serves
-- that says `kosmos: needs keyring-mail` above. It sees mail's entries and
-- nothing else - a share's password is not this door's to read - and it
-- prints no password: what is kept is checked, as a sign-in would check it.

local words = use("/Kosmos/Libraries/files.lua").words(args)
local verb = words[1]

if not fs.has_mail_keys() then
  print("mailpass: this program was not handed mail's passwords")
  return
end

if verb == nil then
  local list, why = fs.mail_passwords()

  if not list then
    print("mailpass: " .. tostring(why))
    return
  end

  print(("%d mail account%s kept"):format(#list, #list == 1 and "" or "s"))

  for _, e in ipairs(list) do
    print(("  %d  %s  %s%s"):format(e.id, e.service, e.account,
          e.title ~= "" and ("  (" .. e.title .. ")") or ""))
  end
elseif verb == "keep" and words[4] then
  local e, why = fs.mail_password_keep(words[2], words[3], words[4], words[5])

  print(e and ("mailpass: kept, entry %d"):format(e.id) or ("mailpass: " .. tostring(why)))
elseif verb == "check" and words[4] then
  local kept, why = fs.mail_password(words[2], words[3])

  if not kept then
    print("mailpass: " .. tostring(why))
  else
    print(kept == words[4] and "mailpass: it matches" or "mailpass: it differs")
  end
elseif verb == "forget" and tonumber(words[2]) then
  local ok, why = fs.mail_password_forget(tonumber(words[2]))

  print(ok and "mailpass: forgotten" or ("mailpass: " .. tostring(why)))
else
  print("usage: mailpass [keep <service> <account> <password> [title]"
        .. " | check <service> <account> <password> | forget <id>]")
end
