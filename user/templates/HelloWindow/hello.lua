-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Hello Window
--
-- Hello Window: a Lua app - a window, a label and a button that counts.
-- Nothing to build: Run runs it. Change the words, press F5, and look.

local ui = use("/Kosmos/Libraries/ui.lua")

local win = ui.window{ title = "Hello Window", w = 360, h = 190, x = 180, y = 140 }
local count = 0

local label = ui.label{ x = 24, y = 34, w = 312, text = "Not pressed yet." }
local button = ui.button{ x = 24, y = 92, text = "Press me", go = true }

button.on_click = function()
  count = count + 1
  label.text = ("Pressed %d %s."):format(count, count == 1 and "time" or "times")
  print("hello: pressed " .. count)
  win.dirty = true
end

win:add(label)
win:add(button)
print("hello: a window with a button")
win:run()
