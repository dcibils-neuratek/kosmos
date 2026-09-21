-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: section demos
--
-- Solar System: the Kosmos host for the portable simulator.
--
--   wm solar
--   solar --check          the simulation alone, no window (bring-up step 1)
--
-- **The core is not ours and is not touched** (`/lib/solar`, and its
-- `README.kosmos.md`): a program that runs unmodified under stock `lua`, in
-- LÖVE, and here. This file is the *host*, and a host owes it four things -
-- a clock, somewhere to put a finished frame, an input queue, and a way to
-- read a file - and nothing else.
--
-- Written in the order the port brief asks for, one bring-up step at a
-- time, each verified in QEMU before the next: the simulation first with no
-- display at all, then what a frame costs, then a window.
--
local use_ = use

--------------------------------------------------------------------------
-- `require`, which this system does not have.
--
-- Kosmos loads a library with `use("/lib/x.lua")` - a file in this
-- process's namespace, with no package path, no search and no global module
-- table, and that is a deliberate position rather than an omission
-- (`init.lua`). The core asks for `require "solar.sim"` because it also
-- runs on interpreters where that is the only way.
--
-- So the host provides one, here, in eight lines: `solar.app` becomes
-- `/lib/solar/app.lua`, and `solar.fonts.f24` becomes
-- `/lib/solar/fonts/f24.lua`. It is a *global* because the core reads it as
-- one, and `use` hands a library this program's own environment - so the
-- files it loads see this `require` and can load their own dependencies.
--
-- Nothing else in Kosmos gains a `require`: this one lives in this
-- program's world and goes when it does.
--------------------------------------------------------------------------

local loaded = {}

function require(name)
  local already = loaded[name]

  if already ~= nil then return already end

  local path = "/lib/" .. tostring(name):gsub("%.", "/") .. ".lua"
  local ok, value = pcall(use_, path)

  if not ok then
    error("solar: " .. path .. " would not load: " .. tostring(value), 2)
  end

  loaded[name] = value

  return value
end

--------------------------------------------------------------------------
-- Bring-up step 1: the simulation, with nothing else.
--
-- The brief's first step, and it is a good one: `sim.lua` needs only
-- `math`, `string` and `table`, so this says whether the core loads and
-- whether float maths is sane on this machine *before* a single pixel is
-- involved. A wrong answer here is a wrong answer about the interpreter;
-- a wrong answer later could be about anything.
--------------------------------------------------------------------------

local function check()
  local Sim = require "solar.sim"
  local sim = Sim.new()

  -- One step, because `au` is worked out when a body is moved rather than
  -- when it is made: asking before that is asking the wrong question.
  sim:update(0)

  -- `calendar` answers five numbers - year, month, day, hour, minute - and
  -- not a sentence, so the sentence is made here.
  local y, mo, d, h, mi = Sim.calendar(sim.days)

  print(("solar: %s, %d bodies"):format(_VERSION, #sim.bodies))
  print(("solar: date %04d-%02d-%02d %02d:%02d UTC"):format(y, mo, d, h, mi))

  for _, b in ipairs(sim.bodies) do
    if b.id == "earth" or b.id == "saturn" or b.id == "jupiter" then
      print(("solar: %-8s %8.4f au"):format(b.id, b.au or -1))
    end
  end

  print("solar: check done")
end

--------------------------------------------------------------------------
-- Bring-up step 2: what a frame costs, before anything is drawn on screen.
--
-- The brief is firm about the order and it is right: the numbers decide the
-- render size and the graphics level, so they come before the window rather
-- than after somebody has been disappointed by one.
--
-- `tools/profile.lua` in the portable project does this with `os.clock`,
-- which Kosmos does not have - the libraries opened here are base,
-- coroutine, table, string, math and utf8. So the timing is the counter,
-- with its rate read from `/dev/cpu` beside the sum, which is this system's
-- rule about clocks. Everything else is the reference's method: warm up,
-- then time N frames of update and draw.
--
-- **No textures yet.** File reads are bring-up step 5, so bodies are
-- flat-shaded here and the sweep is a floor rather than the final answer;
-- the same sweep runs again with `read` once the assets are on the disk.
--------------------------------------------------------------------------

local function timer()
  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1

  return function() return sys.ticks() / hz end
end

local function frames_of(width, height, level, count, focus)
  local App = require "solar.app"
  local clock = timer()
  local app = App.new({ width = width, height = height, quality = level })

  if focus then
    for _, b in ipairs(app.sim.bodies) do
      if b.id == focus then app:setFocus(b) end
    end
  end

  -- The reference's warm-up: ninety updates so the camera has settled and
  -- every lookup table has been built, then one draw thrown away.
  for _ = 1, 90 do app:update(1 / 30) end

  app:draw()

  local began = clock()

  for _ = 1, count do
    app:update(1 / 60)
    app:draw()
  end

  return (clock() - began) / count * 1000, app
end

local function sweep(width, height, count, focus)
  print(("solar: sweep %dx%d, %d frames a level, focus %s, no textures")
        :format(width, height, count, focus or "sun"))

  for level = 1, 10 do
    local ms = frames_of(width, height, level, count, focus)

    print(("solar: level %2d  %8.1f ms  %6.2f fps")
          :format(level, ms, (ms > 0) and (1000 / ms) or 0))
  end

  print("solar: sweep done")
end

--
-- Where a frame goes, by phase: the same wrapping the reference does, on
-- the rasterizer's own methods. This is the table that says what would be
-- worth moving to C - which is the Game Kit's question, and one to answer
-- with numbers rather than with an opinion.
--
local function phases(width, height, level, count, focus)
  local Soft = require "solar.soft"
  local clock = timer()
  local spent, order = {}, {}

  local function watch(name, label)
    local f = Soft[name]

    order[#order + 1] = label

    Soft[name] = function(...)
      local began = clock()
      local a, b, c = f(...)

      spent[label] = (spent[label] or 0) + clock() - began

      return a, b, c
    end
  end

  watch("clear", "clear")
  watch("sphere", "spheres")
  watch("ring", "rings")
  watch("sun", "sun")
  watch("line", "lines")
  watch("text", "text")
  watch("rect", "panels")

  local ms = frames_of(width, height, level, count, focus)

  print(("solar: %dx%d level %d, %.1f ms a frame, focus %s")
        :format(width, height, level, ms, focus or "sun"))

  for _, label in ipairs(order) do
    local each = (spent[label] or 0) / count * 1000

    print(("solar: %-8s %8.2f ms  %5.1f%%"):format(label, each,
          (ms > 0) and (each / ms * 100) or 0))
  end

  print("solar: phases done")
end

--
-- **Attribution by removal**, which is the only honest way here.
--
-- The phase timings above wrap each call, and `sys.ticks()` is a *syscall* -
-- so wrapping `text` and `line`, which are called thousands of times a
-- frame, measures this profiler as much as it measures the renderer. Their
-- shares summed to 112%, which is the tell.
--
-- The reference profiler already knows this and says so: few heavy calls
-- are timed directly, many small ones are measured by taking them away and
-- looking at the difference. That is what this does - one run with
-- everything, then one run each without the stars, the belts and the
-- orbits - and the differences are real milliseconds with no measurement
-- inside them.
--
local function attribute(width, height, level, count, focus)
  local base = frames_of(width, height, level, count, focus)

  print(("solar: %dx%d level %d, focus %s, no textures")
        :format(width, height, level, focus or "sun"))
  print(("solar: everything        %8.1f ms"):format(base))

  local App = require "solar.app"
  local clock = timer()

  -- The same run, with one thing switched off. Written here rather than
  -- through `frames_of` because each needs its own option or key.
  local function without(label, opts, keys)
    local app = App.new({ width = width, height = height, quality = level,
                          stars = opts and opts.stars,
                          belts = opts and opts.belts })

    if focus then
      for _, b in ipairs(app.sim.bodies) do
        if b.id == focus then app:setFocus(b) end
      end
    end

    for _, key in ipairs(keys or {}) do app:key(key, false) end
    for _ = 1, 90 do app:update(1 / 30) end

    app:draw()

    local began = clock()

    for _ = 1, count do
      app:update(1 / 60)
      app:draw()
    end

    local ms = (clock() - began) / count * 1000

    print(("solar: without %-10s %8.1f ms   %6.1f ms of it")
          :format(label, ms, base - ms))

    return ms
  end

  without("orbits", nil, { "o" })
  without("stars", { stars = { 0, 0 } })
  without("belts", { belts = { 0, 0 } })
  without("all three", { stars = { 0, 0 }, belts = { 0, 0 } }, { "o" })

  print("solar: attribution done")
end

--------------------------------------------------------------------------

local wanted = args or ""

local function number(after, fallback)
  return tonumber(wanted:match(after .. "%s+(%d+)")) or fallback
end

if wanted:match("%-%-check") then
  check()
  return
end

if wanted:match("%-%-sweep") then
  local w, h = wanted:match("(%d+)x(%d+)")

  sweep(tonumber(w) or 960, tonumber(h) or 540, number("%-%-frames", 5),
        wanted:match("%-%-focus%s+(%a+)"))
  return
end

if wanted:match("%-%-attrib") then
  local w, h = wanted:match("(%d+)x(%d+)")

  attribute(tonumber(w) or 960, tonumber(h) or 540, number("%-%-level", 5),
            number("%-%-frames", 4), wanted:match("%-%-focus%s+(%a+)"))
  return
end

if wanted:match("%-%-phases") then
  local w, h = wanted:match("(%d+)x(%d+)")

  phases(tonumber(w) or 960, tonumber(h) or 540, number("%-%-level", 5),
         number("%-%-frames", 5), wanted:match("%-%-focus%s+(%a+)"))
  return
end

--------------------------------------------------------------------------
-- Bring-up step 3: a window, and the frame on it.
--
-- **The render size is not the window size.** Cost here is per pixel and
-- the interpreter is the renderer, so the film is drawn small and blown up
-- by a whole number in the one native call that presents it - `surface:pixels`,
-- which takes the whole frame at once. A call per pixel would be half a
-- million calls a frame; a call per frame is one.
--
-- The window draws its own pixels (`direct = true`), so the finished frame
-- is a `commit` and nothing is copied between processes (`gfx.md` 19.4) -
-- the same arrangement Doom, the Super Nintendo and the video player use.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- Bring-up step 5: the textures.
--
-- The core asks its host for `read(path)` and wants the whole file as a
-- string - which is what a texture is here, read once at startup and kept
-- for the life of the program (`string.byte` per texel, the README says).
-- Eleven and a half megabytes of them, which the heap takes because
-- `malloc` grows it by asking the kernel for another arena.
--
-- Bytes come through a region rather than through `fs.read` - the ordinary
-- read answers Lua *values*, and a PPM is not one - so this is the same
-- path the media kit uses for a film: `read_into` fills a region, and
-- `region_read` makes the one string the core is going to hold anyway.
--
-- A texture that is missing is not an error: the body is drawn flat-shaded
-- in its base colour, which is how the port came up before the disk had
-- anything on it.
--------------------------------------------------------------------------

local BIGGEST = 2 * 1024 * 1024         -- earth.ppm is 1.5 MB; none is larger

local function reader()
  local page = sys.memory(BIGGEST // 4096)

  if not page then
    print("solar: no room for a read buffer, so no textures")
    return nil
  end

  return function(path)
    local size = (fs.getattr(path) or {}).size

    if not size or size == 0 then return nil end

    local pieces, at = {}, 0

    while at < size do
      local want = size - at

      if want > BIGGEST then want = BIGGEST end

      local got = fs.read_into(path, page, at, want)

      if not got or got == 0 then break end

      pieces[#pieces + 1] = sys.region_read(page, 0, got)
      at = at + got
    end

    if at == 0 then return nil end

    return table.concat(pieces)
  end
end

--
-- **The HUD is authored at 540 rows** and multiplied up from there - 1x
-- under 900, 2x under 1700 - so a render *below* 540 draws a HUD sized for
-- a screen twice as tall as the one it is on. At 480x270 it swallows the
-- picture, which is what Diego saw: "the hud is too big and cant see the
-- planets".
--
-- So the default is 540 rows, and the speed has to come from somewhere
-- else. The measurements say where: at the overview the orbits are 31% of
-- a frame, the belts 11% and the stars 5%, so the sky and the rings of
-- rubble are the cheap thing to spend. `stars` and `belts` are options the
-- core already takes, which is why this is a host decision rather than a
-- change to the core.
--
local function run(width, height, scale, level, assets, auto, stars, belts,
                   fps_wanted)
  local App = require "solar.app"
  local ui = use_("/lib/ui.lua")
  local wmproto = use_("/lib/wmproto.lua")

  local read = (assets ~= "none") and reader() or nil

  local app = App.new({ width = width, height = height, quality = level,
                        autoQuality = auto,
                        assets = assets ~= "none" and assets or nil,
                        stars = stars, belts = belts,
                        read = read })

  local win = ui.window{ title = "Solar System", direct = true,
                         w = width * scale, h = height * scale, x = 80, y = 70 }

  if not win or not win:surface() then
    print("solar: no window")
    return
  end

  print(("solar: %dx%d at %dx, level %d, auto")
        :format(width, height, scale, level))

  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1
  local last = sys.ticks()

  --
  -- **The frame rate, on the picture** - Diego asked for it, and a number
  -- printed to a console nobody is looking at is not a frame counter.
  --
  -- Drawn by the *host*, onto the surface, after the frame has been
  -- presented: `surface:text` is C and the letters land on pixels that are
  -- already there. The alternative - drawing it into the core's Lua
  -- framebuffer - would be a per-pixel loop in the interpreter for the one
  -- thing on screen that exists to say how slow the interpreter is.
  --
  -- It also means the core is untouched, which is the whole arrangement
  -- here: the HUD is the film's, this is the projectionist's.
  --
  local shown_fps, shown_ms = 0, 0
  local showing_fps = fps_wanted

  local function counter(s_, when)
    if not showing_fps then return end

    local said = ("%.1f fps  %.0f ms"):format(shown_fps, shown_ms)
    local wide = gfx.measure(said) + 12
    local tall = gfx.height() + 6
    local x = width * scale - wide - 8

    s_:fill(x, 8, wide, tall, 0xcc101820)
    s_:fill(x, 8, wide, 1, 0xff3b6ea5)
    s_:text(x + 6, 11, said, (shown_fps >= 20) and 0xff8fe08f
                             or (shown_fps >= 8) and 0xffe0d48f or 0xffe08f8f)
  end

  --
  -- What Kosmos calls a key and what the core calls one. The core asks for
  -- names - "space", "left", "tab" - and a `rawkey` carries the scancode
  -- the keyboard sent, so the mapping lives here, in the host, which is
  -- exactly what a host is for.
  --
  local KEYS = {
    [57] = "space", [105] = "left", [106] = "right", [103] = "up",
    [108] = "down", [15] = "tab", [1] = "escape", [14] = "backspace",
    [53] = "/", [51] = ",", [52] = ".", [26] = "[", [27] = "]",
    [12] = "-", [13] = "=",
    [11] = "0", [2] = "1", [3] = "2", [4] = "3", [5] = "4",
    [6] = "5", [7] = "6", [8] = "7", [9] = "8", [10] = "9",
    [30] = "a", [48] = "b", [46] = "c", [32] = "d", [18] = "e",
    [33] = "f", [34] = "g", [35] = "h", [23] = "i", [36] = "j",
    [37] = "k", [38] = "l", [50] = "m", [49] = "n", [24] = "o",
    [25] = "p", [16] = "q", [19] = "r", [31] = "s", [20] = "t",
    [22] = "u", [47] = "v", [17] = "w", [45] = "x", [21] = "y",
    [44] = "z",
  }

  local shift = false
  local frames, since, said = 0, sys.ticks(), sys.ticks()

  while win.running and not app.quit do
    local now = sys.ticks()
    local dt = (now - last) / hz

    last = now

    app:update(dt)
    app:draw()

    -- The whole frame, in one call into C, and the counter over it.
    win:surface():pixels(app.g.fb, width, height, scale)
    counter(win:surface(), now)

    if not win:commit{ x = 0, y = 0, w = width * scale, h = height * scale } then
      break
    end

    frames = frames + 1

    -- What it is really managing, every two seconds, because a port's first
    -- question is always "how fast is it now".
    --
    -- Twice a second for the number on screen, every two seconds for the
    -- line in the log: one is read while it moves and wants to be current,
    -- the other is read afterwards and wants to be quiet.
    --
    if now - since >= hz // 2 then
      shown_fps = frames / ((now - since) / hz)
      shown_ms = (shown_fps > 0) and (1000 / shown_fps) or 0
      frames, since = 0, now

      if now - said >= 2 * hz then
        said = now
        print(("solar: %.1f frames a second, level %d")
              :format(shown_fps, app.quality or level))
      end
    end

    local reply = wmproto.poll(win.handle, 0)

    if not reply then break end

    for _, ev in ipairs(reply.events or {}) do
      if win:direct_event(ev) then
        -- The window manager's own chrome answered it.
      elseif ev.type == "close" then
        win:close()
      elseif ev.type == "rawkey" then
        if ev.code == 42 or ev.code == 54 then
          shift = ev.down
        elseif ev.down and ev.code == 63 then
          -- F5: the counter is the host's, so its key is the host's too,
          -- and it is one the core does not use.
          showing_fps = not showing_fps
        elseif ev.down and KEYS[ev.code] then
          app:key(KEYS[ev.code], shift)
        end
      elseif ev.type == "mouse" and not ev.menu then
        local x, y = ev.x // scale, ev.y // scale

        if ev.action == "press" then
          app:pointerDown(x, y)
        elseif ev.action == "release" then
          app:pointerUp(x, y)
        elseif ev.action == "move" then
          app:pointerMove(x, y, 0, 0)
        end
      end
    end
  end

  win:close()
  print("solar: closed")
end

--
-- The window is what this program is, so it is what happens unless one of
-- the measuring modes above was asked for by name. An unknown flag lands
-- here rather than in a help text, which is the right way round: `solar
-- --level 8` should open a window at level 8, not explain itself.
--
do
  local w, h = wanted:match("(%d+)x(%d+)")

  --
  -- Defaults for a machine that is emulating every instruction: the HUD's
  -- own 960x540, drawn once rather than doubled, with a quarter of the
  -- stars and a fifth of the belt. `--stars` and `--belts` put them back
  -- for a machine that can afford them, and the ThinkPad is the one to
  -- ask about that rather than QEMU.
  --
  local sky = number("%-%-stars", 500)
  local rubble = number("%-%-belts", 300)

  run(tonumber(w) or 960, tonumber(h) or 540, number("%-%-scale", 1),
      number("%-%-level", 2),
      wanted:match("%-%-assets%s+(%S+)") or "/home/solar/",
      not wanted:match("%-%-hold"),
      { sky, math.floor(sky * 1.3) },
      { rubble, math.floor(rubble * 0.7) },
      not wanted:match("%-%-nofps"))
  return
end

print("solar: bring-up step 2. What exists so far:")
print("  solar --check                       the simulation, no window")
print("  solar --sweep [WxH] [--frames N]    frame time at every level")
print("  solar --phases [WxH] [--level N]    where a frame goes")
print("  solar --attrib [WxH] [--level N]    the same, by removal")
print("  solar [WxH] [--scale N] [--level N] [--assets DIR|none]")
