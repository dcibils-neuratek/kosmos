-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name Mandelbrot
-- kosmos: image build/mandelbrot.elf
--
-- Mandelbrot: a Lua and C app. Lua opens the window and decides what is
-- drawn; C - fractal.c - computes every pixel, which is the work that wants
-- every cycle. Build (F6) compiles fractal.c into build/mandelbrot.elf, and
-- this file runs inside that image, where use("mandelbrot.elf") is the C.

local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local fractal = use("mandelbrot.elf")

local W, H = 520, 340
local ITERATIONS = 200

local win, err = ui.window{ title = "Mandelbrot", w = W, h = H, x = 160, y = 110,
                            direct = true }

if not win or not win:surface() then
  print("mandelbrot: no window to draw in: " .. tostring(err))
  return
end

local hz = fs.read("/Devices/cpu").counter_hz
local started = sys.ticks()

fractal.fill(win:surface(), ITERATIONS)
win:commit{ x = 0, y = 0, w = W, h = H }

print(("mandelbrot: %d x %d pixels, %d iterations, computed in C in %d ms")
      :format(W, H, ITERATIONS, (sys.ticks() - started) * 1000 // hz))

-- A direct window answers its own events: here, only being closed.
while win.running do
  local reply = wmproto.poll(win.handle, 25)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then win:close() end
  end
end
