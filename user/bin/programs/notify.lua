-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- notify: say something as a notification, or read what has been said.
--
--   notify Render finished              a banner, titled that
--   notify Render finished | Kitchen.c3d, in 4 min 12 s
--                                       with a line under the title
--   notify --alert Timer | The 25 minutes are up
--                                       one that stays until it is closed
--   notify --list                       everything kept, oldest first
--   notify --clear                      and nothing kept
--
-- Posted as this program - `/Kosmos/Programs/notify.lua` - whatever it is
-- asked to say, because who posted is the kernel's to tell the server
-- (`notifyproto.h`), not the post's.

local notify = use("/Kosmos/Libraries/notify.lua")
local text = tostring(args or ""):match("^%s*(.-)%s*$")

if text == "" then
  print("usage: notify <title> [| <line>]   say something")
  print("       notify --alert <title> [| <line>]   one that stays until closed")
  print("       notify --list | --clear")
  return
end

if text == "--list" then
  local all = notify.all(0)

  for _, e in ipairs(all) do
    print(("%d  %s  %s%s  %s%s  (%d s ago)"):format(
      e.id, notify.who(e), e.alert and "[alert] " or "", e.title,
      e.body, e.open ~= "" and ("  -> " .. e.open) or "", math.floor(notify.age(e))))
  end

  print(("notify: %d kept"):format(#all))
  return
end

if text == "--clear" then
  print(notify.clear() and "notify: cleared" or "notify: the server did not answer")
  return
end

local alert = false
local rest = text:match("^%-%-alert%s+(.*)$")

if rest then alert, text = true, rest end

local title, body = text:match("^(.-)%s*|%s*(.*)$")

title = title or text

local id, why = notify.post{ title = title, body = body or "", alert = alert }

if id then
  print(("notify: said, as %d"):format(id))
else
  print("notify: " .. tostring(why))
end
