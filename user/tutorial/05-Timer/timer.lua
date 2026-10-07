-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Kitchen Timer
--
-- Lesson 5 of the IDE's tutorial, finished: minutes counted down, measured
-- on the counter rather than counted in seconds, and when they are up a
-- notification and a sound. Started with a number - `timer.lua:5` - it
-- starts counting at once.

local ui     = use("/Kosmos/Libraries/ui.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
local files  = use("/Kosmos/Libraries/files.lua")

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

local win = ui.window{ title = "Kitchen Timer", w = 380, h = 250, x = 260, y = 160 }

-- The digits in the heading's face at 56 pixels: asked for after the
-- window is open, because that is when the desktop says what its faces are.
local shown   = ui.label{ x = 24, y = 20, w = 332, text = "05:00", role = ui.sized("heading", 56) }
local minutes = ui.field{ x = 24, y = 130, w = 90, text = "5", hint = "minutes" }
local start   = ui.button{ x = 128, y = 128, text = "Start" }
local reset   = ui.button{ x = 214, y = 128, text = "Reset" }
local said    = ui.label{ x = 24, y = 196, w = 332, text = "" }

-- When it rings, on the counter; nil while it is not counting.
local deadline = nil
local began = 0

local function show(seconds)
  shown.text = ("%02d:%02d"):format(seconds // 60, seconds % 60)
  win.dirty = true
end

local function ring()
  deadline = nil
  local took = (sys.ticks() - began) // counter_hz

  show(0)
  said.text = "Time's up."
  notify.post{ title = "Kitchen Timer", body = minutes.text .. " minutes are up", alert = true }
  fs.send("/Running/wm", { type = "launch", program = "beep", args = "880 600" })
  print(("timer: rang after %d s"):format(took))
end

start.on_click = function()
  local m = tonumber(minutes.text)

  if not m or m <= 0 then
    said.text = "Type how many minutes."
    win.dirty = true
    return
  end

  began = sys.ticks()
  deadline = began + math.floor(m * 60 * counter_hz)
  said.text = "Counting."
  show(math.ceil(m * 60))
end

reset.on_click = function()
  deadline = nil
  said.text = ""
  show(math.ceil((tonumber(minutes.text) or 0) * 60))
end

-- Once a second, the time left worked out again from the counter.
local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function clock:tick()
  if not deadline then return end

  local left = deadline - sys.ticks()

  if left <= 0 then
    ring()
  else
    show((left + counter_hz - 1) // counter_hz)
  end
end

win:add(shown)
win:add(minutes)
win:add(start)
win:add(reset)
win:add(said)
win:add(clock)

-- A number it was started with: those minutes, counting already.
local given = files.words(args)[1]

if given then
  minutes.text = given
  minutes.caret = #given + 1
  start.on_click()
end

win:run()
