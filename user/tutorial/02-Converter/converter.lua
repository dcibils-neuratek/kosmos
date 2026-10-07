-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Converter
--
-- Lesson 2 of the IDE's tutorial, finished: a number, a choice of what to
-- turn it into, and the answer as you type. Swap turns the question round.

local ui = use("/Kosmos/Libraries/ui.lua")

local win = ui.window{ title = "Converter", w = 440, h = 230, x = 200, y = 150 }

-- Each conversion: its name in the list, the way back, and the sum.
local CONVERSIONS = {
  { "c-f",   "Celsius to Fahrenheit",  "f-c",   function(n) return n * 9 / 5 + 32 end, "F" },
  { "f-c",   "Fahrenheit to Celsius",  "c-f",   function(n) return (n - 32) * 5 / 9 end, "C" },
  { "km-mi", "Kilometres to miles",    "mi-km", function(n) return n / 1.609344 end, "miles" },
  { "mi-km", "Miles to kilometres",    "km-mi", function(n) return n * 1.609344 end, "km" },
  { "kg-lb", "Kilograms to pounds",    "lb-kg", function(n) return n / 0.45359237 end, "lb" },
  { "lb-kg", "Pounds to kilograms",    "kg-lb", function(n) return n * 0.45359237 end, "kg" },
}

local by = {}
local choices = {}

for _, c in ipairs(CONVERSIONS) do
  by[c[1]] = c
  choices[#choices + 1] = { c[1], c[2] }
end

local number = ui.field{ x = 24, y = 30, w = 140, text = "20", hint = "a number" }
local which = ui.dropdown{ x = 180, y = 28, choices = choices, value = "c-f" }
local answer = ui.label{ x = 24, y = 92, w = 392, text = "" }
local swap = ui.button{ x = 24, y = 150, text = "Swap" }

-- The answer, worked out again from what is there now.
local function work_out()
  local n = tonumber(number.text)
  local c = by[which.value]

  if not n then
    answer.text = "Type a number."
  else
    answer.text = ("%s is %.2f %s"):format(number.text, c[4](n), c[5])
  end

  win.dirty = true
end

number.on_change = function() work_out() end
which.on_change = function() work_out() end

-- Swap: the answer becomes the number, and the conversion turns round.
swap.on_click = function()
  local n = tonumber(number.text)
  local c = by[which.value]

  if n then
    number.text = ("%.2f"):format(c[4](n))
    number.caret = #number.text + 1
  end

  which.value = c[3]
  print("converter: swapped to " .. which.value)
  work_out()
end

win:add(number)
win:add(which)
win:add(answer)
win:add(swap)
work_out()
print("converter: " .. answer.text)
win:run()
