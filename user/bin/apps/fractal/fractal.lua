-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Mandelbrot
-- kosmos: name GPU Fractal
-- kosmos: section demos
-- The Mandelbrot set, computed by the GPU's execution units - a kernel
-- thread for sixteen pixels of a row, a lane a pixel - and the same picture
-- on the processor, the two times side by side (`docs/compute.md` C4,
-- drawn in `docs/compute.html`).
--
--   wm fractal
--
-- **The Compute Kit's example.** The kernel is text (`eukernels.lua`,
-- `mandelbrot`), assembled here by `eu.lua` and checked by the GPU's driver
-- before it is kept; the picture is a kit buffer the window shows as it is,
-- without a copy; each frame is one message - the corner of the view, a
-- pixel's size and the most iterations, as the kernel's constants - and a
-- ticket, asked on the window's own clock and never waited for. The
-- processor's half is `fractal.c`, the kernel's arithmetic step for step,
-- on one core.
--
-- Drag to move, scroll to zoom; the iterations rise as it zooms, so the
-- detail holds. Where there is no Intel GPU - QEMU - the GPU half says so
-- and the processor draws.

local ui = use("/Kosmos/Libraries/ui.lua")
local compute = use("/Kosmos/Kits/compute")
local fractal = use("/Kosmos/Kits/fractal")
local kernels = use("/Kosmos/Libraries/eukernels.lua")

local theme, L = ui.theme, ui.layout
local VW, VH = 800, 600                 -- the picture: fifty groups across, six hundred down
local SIDE, FOOT = 320, 30
local W, H = VW + SIDE, L.head + VH + FOOT
local GPU_C, CPU_C = 0xffb98cff, 0xff4f9fe6

local cpu = fs.read("/Devices/cpu") or {}
local counter_hz = cpu.counter_hz or 62500000

--------------------------------------------------------------------------
-- The view: a centre and a pixel's size. The iterations from how far in.
--------------------------------------------------------------------------

local START = { x = -0.6, y = 0.0, d = 3.2 / VW }
local view = { x = START.x, y = START.y, d = START.d }

local function iterations()
  local zoom = START.d / view.d

  return math.max(64, math.min(4096, math.floor(128 + 48 * math.log(math.max(1, zoom), 2))))
end

local function corner()
  return view.x - VW / 2 * view.d, view.y - VH / 2 * view.d
end

--------------------------------------------------------------------------
-- The GPU half: the kernel kept, the picture its buffer.
--------------------------------------------------------------------------

local gpu_why
local kernel, picture, gpu_surface

do
  local ok, why = compute.available()

  if ok then
    kernel, why = compute.kernel(kernels.mandelbrot)
  end

  if kernel then
    picture, why = compute.picture(VW, VH)
  end

  if picture then
    gpu_surface = picture:surface()
  else
    gpu_why = why
    print("fractal: gpu - " .. tostring(why) .. "; the processor draws")
  end
end

local cpu_surface = gfx.surface{ w = VW, h = VH }

cpu_surface:fill(0, 0, VW, VH, 0xff000000)

local mode = gpu_surface and "gpu" or "cpu"
local times = { gpu = nil, cpu = nil }    -- the last of each, in milliseconds
local ticket, ticket_view                -- the run in flight, and the view it is of
local wanted = { gpu = true, cpu = true } -- a frame to compute, by whom
local shown_view = { gpu = nil, cpu = nil }

ui.lend_picture("fractal:gpu", gpu_surface)
ui.lend_picture("fractal:cpu", cpu_surface)

local function view_words(v)
  return ("%.6f,%.6f 1 : %.0f"):format(v.x, v.y, START.d / v.d)
end

-- The GPU's frame: one message, answered at once with a ticket.
local function run_gpu()
  local x0, y0 = corner()
  local t, why, code = compute.run{
    kernel = kernel, simd = 16, grid = { VW // 16, VH },
    buffers = { picture },
    constants = string.pack("<ffffi4", x0, view.d, y0, view.d, iterations()),
  }

  if t then
    ticket, ticket_view = t, { x = view.x, y = view.y, d = view.d, n = iterations() }
    wanted.gpu = false
  elseif code ~= 12 then                  -- busy: the next tick tries again
    print("fractal: the run was refused: " .. tostring(why))
    wanted.gpu = false
  end
end

-- The processor's frame: the same picture, in C, here and now.
local function run_cpu()
  local x0, y0 = corner()
  local n = iterations()
  local t0 = sys.ticks()

  fractal.draw(cpu_surface, x0, view.d, y0, view.d, n)
  times.cpu = (sys.ticks() - t0) * 1000 / counter_hz
  shown_view.cpu = { x = view.x, y = view.y, d = view.d, n = n }
  wanted.cpu = false
  print(("fractal: processor %.1f ms at %s, %d iterations, %dx%d")
        :format(times.cpu, view_words(view), n, VW, VH))
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local win = ui.window{ title = "GPU Fractal", w = W, h = H, x = 80, y = 50, header = true }

if not win then
  print("fractal: no window")
  return
end

local showing_kernel = false
local seg, kernel_button, side, picture_view, foot, code_view

local function changed()
  wanted.gpu, wanted.cpu = gpu_surface ~= nil, true
  win:paint()
end

seg = ui.segments{ items = { "GPU", "Processor" }, on = mode == "gpu" and 1 or 2,
                   on_change = function(_, i)
                     if i == 1 and not gpu_surface then
                       seg.on = 2
                       print("fractal: no GPU to switch to - " .. tostring(gpu_why))
                     else
                       mode = i == 1 and "gpu" or "cpu"
                       print("fractal: showing the " .. (mode == "gpu" and "GPU's" or "processor's") .. " picture")
                     end
                     win:paint()
                   end }

kernel_button = ui.button{ text = "Kernel", on_click = function()
  showing_kernel = not showing_kernel
  code_view.hidden = not showing_kernel
  kernel_button.text = showing_kernel and "Times" or "Kernel"
  print("fractal: " .. (showing_kernel and "the kernel shown" or "the times shown"))
  win:paint()
end }

local reset = ui.button{ text = "Reset", on_click = function()
  view.x, view.y, view.d = START.x, START.y, START.d
  changed()
end }

win:add(ui.header{ x = 0, y = 0, w = W, title = "GPU Fractal",
                   sub = ("Mandelbrot \u{b7} %d \u{d7} %d"):format(VW, VH),
                   right = { seg, kernel_button, reset }, title_bar = true })

-- The picture, whoever computed it, and how long that took.
picture_view = ui.view{ x = 0, y = L.head, w = VW, h = VH }

function picture_view:draw(g)
  local name = mode == "gpu" and "fractal:gpu" or "fractal:cpu"

  g:fill(0, 0, self.w, self.h, 0xff000000)
  g:lent(0, 0, VW, VH, name, VW, VH)

  local ms = times[mode]
  local big = ui.sized("title", 18)
  local line = ms and ("%.1f ms"):format(ms) or "\u{2014}"
  local note = mode == "gpu" and "a frame, on the GPU's execution units"
               or "a frame, on one of the processor's cores"
  local bw = math.max(gfx.measure(line, big), gfx.measure(note, "label")) + 24
  local bh = gfx.height(big) + gfx.height("label") + 14

  g:fill_round(12, 12, bw, bh, 0xd0141820, 9)
  g:frame_round(12, 12, bw, bh, theme.line_soft, 9)
  g:text(24, 18, line, mode == "gpu" and GPU_C or CPU_C, nil, big)
  g:text(24, 18 + gfx.height(big), note, theme.text_dim, nil, "label")

  local hint = "drag to move \u{b7} scroll to zoom"
  local hw = gfx.measure(hint, "label") + 18

  g:fill_round(self.w - hw - 12, self.h - gfx.height("label") - 18, hw, gfx.height("label") + 6, 0xb0141820, 6)
  g:text(self.w - hw - 3, self.h - gfx.height("label") - 15, hint, theme.text_dim, nil, "label")
end

function picture_view:mouse(action, x, y)
  if action == "press" then
    self.grab = { x = x, y = y, vx = view.x, vy = view.y }
    return true
  end

  if not self.grab then return false end

  if action == "move" or action == "release" then
    view.x = self.grab.vx - (x - self.grab.x) * view.d
    view.y = self.grab.vy - (y - self.grab.y) * view.d
    -- The GPU follows the drag; the processor, too slow to, when it ends.
    wanted.gpu = gpu_surface ~= nil

    if action == "release" then
      self.grab = nil
      wanted.cpu = true
    end

    win:paint()
  end

  return true
end

function picture_view:wheel(n)
  view.d = view.d * (0.8 ^ n)
  changed()
  return true
end

win:add(picture_view)

-- The side: the two times, and the run - or the kernel itself.
side = ui.view{ x = VW, y = L.head, w = SIDE, h = VH }

local function heading(g, y, text)
  g:text(16, y, text:upper(), theme.text_dim, theme.raised, "label")
  return y + gfx.height("label") + 6
end

function side:draw(g)
  -- Where the controls are on the screen, said once, for the suite's
  -- clicks (`tools/run_fractal.py`), as This Machine says its choices.
  if not self.placed and win.origin_x then
    self.placed = true
    for _, c in ipairs { { "GPU | Processor", seg }, { "Kernel", kernel_button }, { "the picture", picture_view } } do
      print(("fractal: %s at %d,%d %dx%d"):format(c[1], win.origin_x + c[2].x, win.origin_y + c[2].y,
                                                 c[2].w, c[2].h))
    end
  end

  g:fill(0, 0, self.w, self.h, theme.raised)
  g:fill(0, 0, 1, self.h, theme.line_soft)

  local y = heading(g, 14, "This frame")
  local most = math.max(times.gpu or 0, times.cpu or 0, 0.001)

  for _, row in ipairs { { "GPU", times.gpu, GPU_C, gpu_surface and "" or "no Intel GPU here" },
                         { "Processor", times.cpu, CPU_C, "" } } do
    local label, ms, colour, none = row[1], row[2], row[3], row[4]
    local tw = self.w - 16 - 84 - 70

    g:text(16, y, label, theme.text, theme.raised, "ui")
    g:fill_round(100, y + 4, tw, 10, theme.window, 5)

    if ms then
      g:fill_round(100, y + 4, math.max(4, math.floor(tw * ms / most)), 10, colour, 5)
    end

    local value = ms and (ms < 10 and ("%.1f ms"):format(ms) or ("%.0f ms"):format(ms)) or (none ~= "" and "none" or "\u{2014}")

    g:text(self.w - 16 - gfx.measure(value, "ui"), y, value, theme.text_dim, theme.raised, "ui")
    y = y + gfx.height("ui") + 8
  end

  if times.gpu and times.cpu and times.gpu > 0 then
    g:text(16, y, ("%.0f times as fast - the last processor frame"):format(times.cpu / times.gpu),
           theme.text, theme.raised, "label")
  elseif not gpu_surface then
    g:text(16, y, ui.fitted("GPU: " .. tostring(gpu_why), self.w - 32, "label"), theme.text_dim, theme.raised, "label")
  end

  y = y + gfx.height("label") + 18

  if showing_kernel then
    heading(g, y, ("The kernel \u{b7} %d instructions"):format(kernel and kernel.instructions or 0))
    return
  end

  y = heading(g, y, "The run")

  local n = iterations()
  local facts = {
    { "Grid", ("%d \u{d7} %d groups, 16 pixels each"):format(VW // 16, VH) },
    { "Threads", ("%d, sixteen lanes each"):format(VW // 16 * VH) },
    { "Iterations", ("up to %d"):format(n) },
    { "Buffer", ("%.1f MB, shared, shown as is"):format(VW * VH * 4 / 1048576) },
    { "Each frame", "one message, one paint" },
  }

  for _, f in ipairs(facts) do
    g:text(16, y, f[1], theme.text_dim, theme.raised, "ui")
    g:text(108, y, ui.fitted(f[2], self.w - 124, "ui"), theme.text, theme.raised, "ui")
    y = y + gfx.height("ui") + 4
  end
end

win:add(side)

-- The kernel, as the program hands it over: read-only, monospace.
code_view = ui.editor{ x = VW + 12, y = L.head + 170, w = SIDE - 24, h = VH - 182,
                       text = kernels.mandelbrot, gutter = false, read_only = true }
code_view.hidden = true
win:add(code_view)

foot = ui.view{ x = 0, y = L.head + VH, w = W, h = FOOT }

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.raised)
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local y = (self.h - gfx.height("label")) // 2

  g:text(16, y, ("Centre %.6f, %.6f      Scale 1 : %.0f"):format(view.x, view.y, START.d / view.d),
         theme.text_dim, theme.raised, "label")

  local right = gpu_surface and "Kosmos Compute Kit \u{b7} the GPU's execution units"
                or "Kosmos Compute Kit \u{b7} no Intel GPU here"

  g:text(self.w - 16 - gfx.measure(right, "label"), y, right, theme.text_dim, theme.raised, "label")
end

win:add(foot)

--------------------------------------------------------------------------
-- The clock: a ticket asked, a frame given - never waited for.
--------------------------------------------------------------------------

local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }
local held_once = false

-- The two pictures held to each other, the first time both are of one
-- view: the GPU's against the processor's, pixel for pixel.
local function hold()
  local a, b = shown_view.gpu, shown_view.cpu

  if held_once or not (a and b) or a.x ~= b.x or a.y ~= b.y or a.d ~= b.d then return end

  held_once = true
  print(("fractal: the GPU's picture and the processor's differ in %d of %d pixels")
        :format(fractal.differ(gpu_surface, cpu_surface), VW * VH))
end

function clock:tick()
  if ticket and ticket:done() then
    local ns = ticket:time()

    times.gpu = ns and ns / 1e6 or times.gpu
    shown_view.gpu = ticket_view
    ticket = nil
    print(("fractal: gpu %.2f ms at %s, %d iterations, %dx%d")
          :format(times.gpu or 0, view_words(shown_view.gpu), shown_view.gpu.n, VW, VH))
    hold()
    win:paint()
  end

  if not ticket and wanted.gpu and gpu_surface then
    run_gpu()
  end

  -- The processor's frame once the GPU's is given, so a drag stays the GPU's.
  if wanted.cpu and not ticket then
    run_cpu()
    hold()
    win:paint()
  end
end

win:add(clock)
win.tick_every = counter_hz // 60

print(("fractal: open, %dx%d, %s"):format(VW, VH, gpu_surface and "on the GPU and the processor"
                                          or "on the processor only"))
win:run()

if kernel then kernel:forget() end
if picture then picture:free() end
