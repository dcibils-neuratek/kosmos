-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- automate: one step of an automation, run at the prompt (`docs/automations.md`
-- A2) - the verbs of `automate.lua`, each a line.
--
--   automate controls Calculator             every control it publishes
--   automate open Calculator                 opened, or found open
--   automate open "Text Editor" /Home/notes.md
--   automate press Calculator 7              a button, a key, by its label
--   automate menu "Text Editor" more "Find and replace"
--   automate read Calculator Result          what a control shows
--   automate type Mail To ana@example.com    a field's text
--   automate set Preferences Wi-Fi on        a switch, a checkbox, a slider
--   automate choose Cafesa3D Rendered        a tab or a segment
--   automate choose Preferences Theme Dark   a dropdown's item
--   automate mail ana@example.com "Subject" "The text"
--   automate check https://example.com Example
--   automate copy /Home/a.txt /Drives/STICK  (and move)
--   automate notify "Done" "Copied a.txt"
--   automate wait 2                          seconds
--   automate run hello world                 a program, to its end
--
-- Words with spaces in quotes. Says what it did, or why not, in a line, and
-- that line is what an automation's history will show for the same step.

local auto = use("/Kosmos/Libraries/automate.lua")
local files = use("/Kosmos/Libraries/files.lua")

local words = files.words(args or "")
local verb = table.remove(words, 1)

local USAGE = {
  "usage: automate <verb> ...   one step, as an automation runs it",
  "  controls <app>             open <app> [file]",
  "  press <app> <label>        menu <app> <menu> <item>",
  "  read <app> <label>         type <app> <label> <text>",
  "  set <app> <label> <value>  choose <app> <label> [item]",
  "  mail <to> <subject> <text> check <url> [words]",
  "  copy <from> <to>           move <from> <to>",
  "  notify <title> [line]      wait <seconds>",
  "  run <program> [words]",
}

local function done(ok, why, said)
  if ok then
    print(said or tostring(ok))
  else
    print("automate: " .. tostring(why))
  end
end

-- The application a verb is about: open already, never started by one -
-- `open` is the verb that starts it.
local function app(name)
  local a, why = auto.app(tostring(name))

  if not a then print("automate: " .. tostring(why)) end
  return a
end

if verb == nil then
  for _, line in ipairs(USAGE) do print(line) end
  return
end

local a1, a2, a3 = words[1], words[2], words[3]

if verb == "open" and a1 then
  local a, why = auto.open(a1, { file = a2 })

  done(a, why, a and ("open as " .. a.name))
elseif verb == "controls" and a1 then
  local a = app(a1)
  local list, why

  if a then list, why = a:controls() end

  if a and not list then print("automate: " .. tostring(why)) end

  for _, c in ipairs(list or {}) do
    local can = {}

    if c.read then can[#can + 1] = "read" end
    if c.write then can[#can + 1] = "write" end
    if c.press then can[#can + 1] = "press" end

    print(("%-40s %s%s"):format(c.address, table.concat(can, " "), c.disabled and " (disabled)" or ""))
  end
elseif verb == "press" and a2 then
  local a = app(a1)
  if a then done(a:press(a2)) end
elseif verb == "menu" and a3 then
  local a = app(a1)
  if a then done(a:menu(a2, a3)) end
elseif verb == "read" and a2 then
  local a = app(a1)
  if a then done(a:read(a2)) end
elseif verb == "type" and a3 then
  local a = app(a1)
  if a then done(a:type(a2, a3)) end
elseif verb == "set" and a3 then
  local a = app(a1)
  if a then done(a:set(a2, a3)) end
elseif verb == "choose" and a2 then
  local a = app(a1)
  if a then done(a:choose(a2, a3)) end
elseif verb == "mail" and a3 then
  local id, asked = auto.mail{ to = a1, subject = a2, text = a3 }

  done(id, asked, id and ("in the Outbox as %s%s"):format(id, asked and ", maild has it" or ", maild not running"))
elseif verb == "check" and a1 then
  local page, why = auto.check(a1, { has = a2 })

  done(page, why, page and ("%d %s"):format(page.status, page.title ~= "" and page.title or page.url))
elseif (verb == "copy" or verb == "move") and a2 then
  done(auto[verb](a1, a2))
elseif verb == "notify" and a1 then
  done(auto.notify(a1, a2))
elseif verb == "wait" then
  done(auto.wait(tonumber(a1) or 1), nil, "waited")
elseif verb == "run" and a1 then
  local ok, why = auto.run(a1, table.concat(words, " ", 2))

  done(ok, why, "ran " .. a1)
else
  for _, line in ipairs(USAGE) do print(line) end
end
