-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `user/lib/automate.lua` on the host (`docs/automations.md` A2): which
-- control a label means, and which windows are an application's. Both are
-- where a wrong answer is silent - a press on the wrong button, a step that
-- drives another window - so they are held here, on what the kit publishes,
-- without booting a machine. The system's answers are stood in for.

fs = { read = function() return nil end, list = function() return {} end,
       program = function() return nil end, getattr = function() return nil end }
sys = { info = function() return { tick_hz = 250 } end, ticks = function() return 0 end,
        sleep = function() end }
use = function(path)
  if path == "/Kosmos/Libraries/files.lua" then return {} end
  error("no " .. path)
end

local auto = dofile("user/lib/automate.lua")
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

-- What Calculator and the Text Editor publish, as `addresses` answers.
local controls = {
  { address = "button/C", kind = "button" },
  { address = "button/7", kind = "button" },
  { address = "button/×", kind = "button" },
  { address = "display/Result", kind = "display" },
  { address = "button/Save", kind = "button" },
  { address = "button/OK", kind = "button" },
  { address = "button/OK#2", kind = "button" },
  { address = "field/Find", kind = "field" },
  { address = "button/Find", kind = "button" },
  { address = "menu/more/Find and replace", kind = "menu" },
  { address = "menu/File/Save", kind = "menu" },
  { address = "tab/Rendered", kind = "tab" },
}

local function means(label, kinds)
  local c, why = auto.resolve(controls, label, kinds)
  return c and c.address, why
end

-- An address said exactly is that control, whatever else shares its words.
check(means("button/Save") == "button/Save", "button/Save said exactly was not that control")
check(means("menu/File/Save") == "menu/File/Save", "menu/File/Save said exactly was not that menu item")

-- A label alone, when one control has it.
check(means("7") == "button/7", "the label 7 did not mean button/7")
check(means("×") == "button/×", "the label × did not mean button/×")
check(means("Result") == "display/Result", "Result did not mean display/Result")
check(means("Find and replace") == "menu/more/Find and replace",
      "a menu item's own words did not mean it")
check(means("Rendered") == "tab/Rendered", "a tab's words did not mean it")

-- A label two controls have: refused, with both addresses named.
local none, why = means("Save")
check(none == nil and tostring(why):find("button/Save", 1, true)
      and tostring(why):find("menu/File/Save", 1, true),
      "Save, a button and a menu item, was not refused with both: " .. tostring(none or why))

local _, ok2 = means("OK")
check(tostring(ok2):find("button/OK#2", 1, true) ~= nil,
      "two OK buttons were not told apart by #2: " .. tostring(ok2))

-- The kinds asked for decide between two of one word: typing goes to the
-- field, never the button beside it.
check(means("Find", { field = true }) == "field/Find", "typing into Find did not find the field")
check(means("Find", { button = true }) == "button/Find", "pressing Find did not find the button")

local missing, said = means("Launch")
check(missing == nil and tostring(said):find("no control called Launch", 1, true),
      "a label nothing has was not said so: " .. tostring(missing or said))

-- Windows: the application's name, a second window of it, a title that
-- ends in it - and nothing that merely starts with the same letters.
check(auto.names("Calculator", "Calculator"), "Calculator was not Calculator")
check(auto.names("calculator", "Calculator"), "the case of a name counted")
check(auto.names("Mail2", "Mail"), "a second Mail window was not Mail's")
check(auto.names("notes.md - Text Editor", "Text Editor"), "a document's window was not its editor's")
check(not auto.names("Mailbox", "Mail"), "Mailbox was taken for Mail")
check(not auto.names("Text Editor Help", "Text Editor"), "a window that only starts so was taken")
check(not auto.names("Mail.x", "Mail"), "a name with a dot after it was taken")

if #fails == 0 then
  print(("PASS: %d checks on automate.lua (an address said exactly, a label alone when one "
         .. "control has it, two refused with both named, the kind deciding a field from a "
         .. "button, a missing one said; an application's windows by name, number and "
         .. "title)"):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on automate.lua:"):format(#fails, checks))
for _, f in ipairs(fails) do print("  " .. f) end
os.exit(1)
