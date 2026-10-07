-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Counter
--
-- Lesson 1 of the IDE's tutorial, finished: a window, a label, and two
-- buttons - one counts, one starts again. Help ▸ Tutorial has the lesson.

local ui = use("/Kosmos/Libraries/ui.lua")

-- The window: a title, a size, and where it opens.
local win = ui.window{ title = "Counter", w = 360, h = 200, x = 180, y = 140 }
local count = 0

-- What it says, and the two things to press.
local label = ui.label{ x = 24, y = 34, w = 312, text = "Not pressed yet." }
local press = ui.button{ x = 24, y = 96, text = "Press me", go = true }
local reset = ui.button{ x = 150, y = 96, text = "Start again" }

-- The words, from the count.
local function show()
  if count == 0 then
    label.text = "Not pressed yet."
  else
    label.text = ("Pressed %d %s."):format(count, count == 1 and "time" or "times")
  end

  win.dirty = true
end

press.on_click = function()
  count = count + 1
  print("counter: pressed " .. count)
  show()
end

reset.on_click = function()
  count = 0
  print("counter: started again")
  show()
end

win:add(label)
win:add(press)
win:add(reset)
print("counter: a window with two buttons")
win:run()
