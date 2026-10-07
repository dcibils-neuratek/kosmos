-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name What's Running
--
-- Lesson 4 of the IDE's tutorial, finished: the machine asked what it is
-- doing - its processes and what each holds, its memory, how long it has
-- been up, and the names answering in /Running - once a second, on a clock
-- of its own.

local ui = use("/Kosmos/Libraries/ui.lua")

-- How fast the counter counts, read once: it differs from machine to
-- machine, so a time is worked out from it rather than assumed.
local cpu = fs.read("/Devices/cpu") or {}
local counter_hz = cpu.counter_hz or 62500000

local win = ui.window{ title = "What's Running", w = 420, h = 360, x = 240, y = 130 }

local machine = ui.label{ x = 16, y = 14, w = 388, text = "" }
local memory  = ui.label{ x = 16, y = 42, w = 388, text = "" }
local named   = ui.label{ x = 16, y = 70, w = 388, text = "" }
local names   = ui.list{ x = 16, y = 104, w = 388, h = 240, items = {} }

local looks = 0

-- Everything asked again: the kernel for its processes, /Devices for the
-- memory, and /Running for the names that answer there.
local function look()
  local alive = {}

  for _, p in ipairs(sys.processes() or {}) do
    if not p.exited then alive[#alive + 1] = p end
  end

  -- The largest first: what each holds, in pages of 4 KB.
  table.sort(alive, function(a, b) return (a.held or a.pages or 0) > (b.held or b.pages or 0) end)

  local rows = {}

  for _, p in ipairs(alive) do
    rows[#rows + 1] = ("%s - %d KB"):format(p.name, (p.held or p.pages or 0) * 4)
  end

  names.items = rows

  local up = sys.ticks() // counter_hz
  machine.text = ("%d processes, up %d:%02d"):format(#alive, up // 60, up % 60)

  local m = fs.read("/Devices/memory")

  if m then
    memory.text = ("%d of %d MB in use"):format(m.total_mb - m.free_mb, m.total_mb)
  end

  local running = fs.list("/Running") or {}
  table.sort(running)
  named.text = "Answering by name: " .. table.concat(running, ", ")
  win.dirty = true

  looks = looks + 1

  if looks == 1 then print("running: " .. rows[1] .. " the largest") end
  if looks == 3 then print("running: looked 3 times") end
end

-- A view with a `tick` is called once a second by the window, between
-- everything else it does.
local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function clock:tick() look() end

win:add(machine)
win:add(memory)
win:add(named)
win:add(names)
win:add(clock)
look()
win:run()
