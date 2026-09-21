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
-- Which rasterizer, declared here and decided once the arguments are read.
--
-- A `local` this early because every mode below closes over it - the
-- measuring ones as much as the window - and a name assigned further down
-- than the function that reads it is a global by accident.
--
-- The C one needs a surface to draw into and cannot make one itself, so
-- `film_for` is the other half of the decision: a surface when there is a
-- C rasterizer to use it, and nil when the core is going to allocate its
-- own Lua table as it always has.
--------------------------------------------------------------------------

local Native

local function film_for(w, h)
  if not Native then return nil end

  return gfx.surface{ w = w, h = h }
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
  local app = App.new({ width = width, height = height, quality = level,
                        fb = film_for(width, height) })

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
  --
  -- **This one only works on the Lua rasterizer, and says so rather than
  -- quietly measuring nothing.** It wraps the rasterizer's methods to time
  -- them, and the C one keeps its methods in a metatable that this cannot
  -- reach - so under `--compare`'s other half every phase would read zero
  -- and the table would look like a frame that costs nothing at all.
  --
  -- A silently empty profile is worse than a refusal, because somebody
  -- would believe it.
  --
  if Native then
    print("solar: --phases profiles the Lua rasterizer; add --lua")
    return
  end

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
                          belts = opts and opts.belts,
                          fb = film_for(width, height) })

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

--
-- **What a primitive costs, three ways**, which is the question the Game
-- Kit has to keep answering: the interpreter, C writing into a Lua array,
-- and C writing into a surface. The three numbers are the whole argument
-- for where a renderer's pixels should live.
--
local function bench(width, height, count)
  local ok, game = pcall(use_, "/kits/game")

  if not ok then print("solar: no /kits/game") return end

  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1
  local fb = {}

  for i = 1, width * height do fb[i] = 0 end

  local function ms(what)
    local began = sys.ticks()

    for _ = 1, count do what() end

    return (sys.ticks() - began) / hz * 1000 / count
  end

  local in_lua = ms(function()
    for i = 1, width * height do fb[i] = 0x102030 end
  end)

  local in_c = ms(function() game.clear(fb, width, height, 0x102030) end)
  local surface = gfx.surface{ w = width, h = height }
  local on_surface = ms(function()
    game.clear(surface, width, height, 0x102030)
  end)

  print(("solar: clear %dx%d, %d times each"):format(width, height, count))
  print(("solar:   Lua into a table      %8.2f ms"):format(in_lua))
  print(("solar:   C into that table     %8.2f ms  %.2fx"):format(in_c,
        (in_c > 0) and (in_lua / in_c) or 0))
  print(("solar:   C into a surface      %8.2f ms  %.2fx"):format(on_surface,
        (on_surface > 0) and (in_lua / on_surface) or 0))
  print("solar: bench done")
end

--------------------------------------------------------------------------

local wanted = args or ""

--------------------------------------------------------------------------
-- The Game Kit's rasterizer, under the core's own name.
--
-- **The core is not edited; its module is furnished.** `require` is this
-- program's (see the shim above), so seeding `loaded["solar.soft"]` before
-- anything asks for it means `solar/app.lua` picks up the C rasterizer at
-- the line where it says `local Soft = require "solar.soft"`, without a
-- byte of the core changing. `solar/soft.lua` on disk is untouched - the
-- same file that runs under stock `lua` and LOVE - and `--lua` runs it.
-- That is what the port brief means by an optional native fast path the
-- core falls back from.
--
-- **Why the whole rasterizer and not a few primitives.** The first attempt
-- here replaced `clear` and `line` with C ones that wrote into the core's
-- Lua table, and `solar --bench` said, clearing 960x540 ten times:
--
--     Lua into a table       24.81 ms
--     C into that table      43.81 ms   0.56x
--     C into a surface        1.32 ms  18.72x
--
-- C writing into a Lua *table* is slower than the interpreter writing into
-- it. `fb[i] = c` inside a Lua loop is one VM instruction reaching the
-- table's array part; the same store from C is `lua_pushinteger` and
-- `lua_rawseti`, the whole table API with its boxing and its barrier, once
-- per pixel. The interpreter was never the slow part - **the
-- representation was**, and a C loop over it pays that toll twice.
--
-- So the framebuffer had to become a surface, and the moment it does,
-- every function that indexes it has to come too: `sphere`, `ring` and
-- `sun` read and write `self.fb[i]` directly, inside their inner loops,
-- which is exactly why they are fast in Lua and exactly why this was all
-- or nothing. `user/lib/gamesoft.c` is the whole of it.
--
-- `--compare` is what says it draws the same picture: the same calls into
-- both rasterizers, then a pixel-by-pixel count of the disagreements.
--------------------------------------------------------------------------

local function native_soft()
  local ok, game = pcall(use_, "/kits/game")

  if not ok or type(game) ~= "table" or type(game.soft) ~= "table" then
    return nil
  end

  return game.soft
end

--
-- The decision itself. `--lua` is the portable rasterizer, and it is worth
-- keeping reachable rather than being a thing you rebuild to get: it is
-- the reference the C is checked against by `--compare`, and it is what
-- proves the core still runs unmodified.
--
Native = not wanted:match("%-%-lua") and native_soft() or nil

if Native then
  loaded["solar.soft"] = Native
end

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

if wanted:match("%-%-bench") then
  local w, h = wanted:match("(%d+)x(%d+)")

  bench(tonumber(w) or 960, tonumber(h) or 540, number("%-%-frames", 10))
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

  --
  -- The film, and with the C rasterizer it is a surface rather than half a
  -- million Lua numbers. `Soft.new(w, h, fb)` has always taken a host
  -- buffer as its third argument - "anything indexable 1 .. w*h that
  -- stores numbers", says the core's own comment - so handing it one is
  -- the contract the port brief describes rather than a hole cut for us.
  --
  local film = film_for(width, height)

  if Native and not film then
    print("solar: no room for a " .. width .. "x" .. height .. " surface")
    return
  end

  local app = App.new({ width = width, height = height, quality = level,
                        autoQuality = auto,
                        assets = assets ~= "none" and assets or nil,
                        stars = stars, belts = belts,
                        fb = film,
                        read = read })

  local win = ui.window{ title = "Solar System", direct = true,
                         w = width * scale, h = height * scale, x = 80, y = 70 }

  if not win or not win:surface() then
    print("solar: no window")
    return
  end

  print(("solar: %dx%d at %dx, level %d, auto, %s rasterizer")
        :format(width, height, scale, level, Native and "C" or "Lua"))

  --
  -- Putting the frame on the window, and which call that is depends on
  -- where the frame lives.
  --
  -- A surface goes across with `blit` or `stretch` - a C loop reading
  -- pixels and writing pixels, with nothing in between. A Lua table has to
  -- go through `pixels`, which reads half a million table slots. Both are
  -- one call a frame, which is the thing that matters either way.
  --
  -- **`win:surface()` is asked every frame, and that is not sloppiness.**
  -- The window is double-buffered: `surface()` answers
  -- `region[region.draw_into]`, and `commit` flips `draw_into` to the other
  -- one. Holding the answer in a local means every frame after the first is
  -- drawn into the buffer that was just put on screen, while the one being
  -- shown is never written - which looks like a black window at a perfectly
  -- healthy forty-two frames a second, and is exactly what it did for one
  -- screenshot on 21 September.
  --
  local present

  if Native and scale == 1 then
    present = function(dst) dst:blit(film, 0, 0, width, height, 0, 0) end
  elseif Native then
    present = function(dst)
      dst:stretch(film, 0, 0, width, height, 0, 0, width * scale, height * scale)
    end
  else
    present = function(dst) dst:pixels(app.g.fb, width, height, scale) end
  end

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

    -- The whole frame, in one call into C, and the counter over it. The
    -- surface is this pass's back buffer, asked for now rather than kept.
    local dst = win:surface()

    present(dst)
    counter(dst, now)

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

--------------------------------------------------------------------------
-- `--compare`: the two rasterizers, the same calls, and the pixels counted.
--
-- **This is the whole argument for the C being allowed to exist.** It is
-- an optional fast path under a renderer that also runs elsewhere, so the
-- only thing that makes it legitimate is drawing the picture the portable
-- one draws. Not approximately: the same `floor` in the same three places,
-- the same coverage on an anti-aliased line, the same texel out of the
-- same equirectangular lookup.
--
-- So the check is direct rather than clever. Every primitive is called on
-- both with identical arguments, and then every pixel is compared. There
-- is no tolerance and no sampling: a single channel out by one is a
-- failure, because a rounding difference that shows up on one pixel today
-- is the one that shows up on a hundred thousand when a planet fills the
-- screen.
--
-- It reports the first disagreement it finds with both colours, because
-- "17 pixels differ" tells you nothing and "at 412,88 the Lua says 3f4a5e
-- and the C says 3f4a5d" tells you which `floor` to go and look at.
--
-- Textures are not read here and that is deliberate: this has to run on a
-- machine with nothing on its disk, and the flat-shaded path exercises
-- every line of the shading except the three that index a string. Those
-- are covered by the picture on screen, which is where a wrong texel is
-- extremely obvious.
--------------------------------------------------------------------------

local function compare(width, height)
  local Lua = use_("/lib/solar/soft.lua")
  local C = native_soft()

  if not C then
    print("solar: no /kits/game, so there is nothing to compare against")
    print("solar: FAIL")
    return
  end

  local surface = gfx.surface{ w = width, h = height }

  if not surface then
    print("solar: no room for the comparison surface")
    print("solar: FAIL")
    return
  end

  local a = Lua.new(width, height)
  local b = C.new(width, height, surface)

  --
  -- One script, run twice. A table of calls rather than two copies of the
  -- same code, because two copies is how a check ends up checking that the
  -- Lua agrees with itself.
  --
  local AXES = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }
  local TILT = { { 0.94, -0.34, 0 }, { 0.34, 0.94, 0 }, { 0, 0, 1 } }

  local script = {
    { "clear", 0x101828 },

    -- Lines at every slope, including the ones that clip on each edge and
    -- the degenerate ones, because the major-axis choice and the
    -- Liang-Barsky parameters are where a port goes wrong.
    { "line", 10, 10, 300, 200, 255, 128, 64, 1 },
    { "line", 300, 200, 10, 10, 255, 128, 64, 0.5 },
    { "line", -50, 30, 400, 31, 90, 200, 255, 0.8 },
    { "line", 20, -80, 21, 400, 200, 255, 90, 0.8 },
    { "line", -200, -200, 900, 800, 255, 255, 255, 0.3 },
    { "line", 100, 100, 100, 100, 255, 0, 0, 1 },
    { "lineFast", 5, 300, 500, 320, 120, 255, 180, 0.9 },
    { "lineFast", -400, 250, 400, 255, 255, 220, 120, 0.4 },

    -- Points of all three sizes, on and off the edge.
    { "point", 40, 40, 1, 255, 255, 255, 1 },
    { "point", 60, 40, 2, 255, 240, 200, 0.7 },
    { "point", 80, 40, 3, 200, 220, 255, 0.55 },
    { "point", 0, 0, 3, 255, 255, 255, 1 },

    { "rect", 200, 60, 120, 70, 30, 40, 60, 1 },
    { "rect", 230, 80, 120, 70, 200, 60, 40, 0.35 },
    { "rect", -20, 200, 80, 60, 60, 200, 90, 0.5 },
    { "frame", 205, 65, 110, 60, 220, 220, 220, 0.9 },
    { "circle", 150, 250, 55, 255, 200, 90, 0.8 },
    { "circle", 30, 30, 90, 90, 160, 255, 0.4 },

    -- The three expensive ones, at step 1 and at step 2, with and without
    -- an atmosphere, and with the ring shadow on - which is the only
    -- caller of the ring LUT from inside a sphere.
    { "sun", { sx = 120, sy = 420, sr = 26, ax = AXES[1], ay = AXES[2],
               az = AXES[3], pad = 5, step = 1 } },
    { "sun", { sx = 300, sy = 430, sr = 18, ax = TILT[1], ay = TILT[2],
               az = TILT[3], pad = 3, step = 2 } },

    { "sphere", { sx = 420, sy = 150, sr = 70, ax = AXES[1], ay = AXES[2],
                  az = AXES[3], L = { 0.6, 0.3, -0.74 },
                  color = { 180, 140, 110 }, step = 1 } },
    { "sphere", { sx = 560, sy = 300, sr = 48, ax = TILT[1], ay = TILT[2],
                  az = TILT[3], L = { -0.5, 0.2, -0.84 },
                  color = { 90, 130, 200 }, atm = 0.8,
                  atmColor = { 120, 170, 255 }, step = 1 } },
    { "sphere", { sx = 200, sy = 380, sr = 40, ax = AXES[1], ay = AXES[2],
                  az = AXES[3], L = { 0.2, 0.8, -0.56 },
                  color = { 210, 190, 140 }, ringShadow = true, step = 1 } },
    { "sphere", { sx = 640, sy = 120, sr = 55, ax = TILT[1], ay = TILT[2],
                  az = TILT[3], L = { 0.7, -0.2, -0.68 },
                  color = { 160, 160, 160 }, atm = 0.5, step = 2 } },

    { "ring", { sx = 200, sy = 380, sr = 40, N = { 0.1, 0.86, 0.5 },
                L = { 0.2, 0.8, -0.56 }, step = 1 } },
    { "ring", { sx = 640, sy = 400, sr = 34, N = { -0.3, 0.7, 0.65 },
                L = { 0.7, -0.2, -0.68 }, step = 2, shadow = false } },
    { "ring", { sx = 400, sy = 300, sr = 30, N = { 0.9, 0.1, 0.01 },
                L = { 0, 0, -1 }, step = 1 } },          -- edge-on: nothing
  }

  -- Text last, over everything, so a glyph blends rather than merely
  -- covering - which is the half of `S:text` that could differ.
  local Font = require "solar.font"
  local font = Font.get(12)

  script[#script + 1] = { "text", font, "Comparison 0123 ~!@#", 24, 470,
                          230, 230, 240, 0.85 }
  script[#script + 1] = { "text", font, "right edge", 700, 490,
                          255, 180, 90, 0.6, "right" }

  for _, call in ipairs(script) do
    local name = call[1]

    a[name](a, table.unpack(call, 2))
    b[name](b, table.unpack(call, 2))
  end

  --
  -- The comparison. `surface:get` is one call per pixel, which is slow and
  -- is fine: this runs once and it is a check rather than a frame.
  --
  -- **What is also counted is how much of the picture is not the
  -- background**, and that is not decoration. Two rasterizers that drew
  -- nothing at all agree on every pixel, so "0 differ" on its own is a
  -- sentence that a broken script and a correct port both produce. The
  -- coverage is what tells them apart, and it is checked rather than
  -- printed for somebody to notice.
  --
  local differ, drawn, first = 0, 0, nil
  local BACKGROUND = 0x101828

  for y = 0, height - 1 do
    for x = 0, width - 1 do
      local want = a.fb[y * width + x + 1]
      local got = surface:get(x, y) & 0x00ffffff

      if want ~= BACKGROUND then drawn = drawn + 1 end

      if want ~= got then
        differ = differ + 1

        if not first then
          first = ("at %d,%d the Lua says %06x and the C says %06x")
                  :format(x, y, want, got)
        end
      end
    end
  end

  print(("solar: compared %d pixels of %dx%d, %d drawn on, %d differ")
        :format(width * height, width, height, drawn, differ))

  -- A tenth of the frame. The script above covers about a fifth, so this
  -- is well clear of it and still catches a script that stopped drawing.
  if drawn < width * height // 10 then
    print("solar: the script drew almost nothing, so agreeing means nothing")
    print("solar: FAIL")

    return
  end

  if first then
    print("solar:   " .. first)
    print("solar: FAIL")

    return
  end

  print("solar: the C rasterizer draws the Lua rasterizer's picture")
  print("solar: PASS")
end

if wanted:match("%-%-compare") then
  local w, h = wanted:match("(%d+)x(%d+)")

  compare(tonumber(w) or 720, tonumber(h) or 512)

  return
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
