-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: section demos
-- System Benchmark. What this machine can do, as one number and its parts.
--
--   wm sysbench
--
-- The window. Everything it measures lives in `/lib/bench.lua`, which
-- `score` uses as well - one engine, two faces, so a board with a serial
-- cable and no display gets the same numbers as a desktop does.
--
-- **It stays usable while it runs**, and that is not a nicety. Each
-- measurement is a coroutine resumed one slice at a time, so the window
-- redraws between slices, the rows fill in as answers arrive, and the
-- window can be dragged while the processor test is running. An
-- application that freezes the desktop for two minutes is the exact
-- failure this whole system exists to avoid, and a benchmark that did it
-- would be a poor advertisement for the design it is measuring.

local ui    = use("/lib/ui.lua")
local theme = ui.theme
local bench = use("/lib/bench.lua")

local W, H  = 620, 700
local ROW   = 20

-- How often the window is allowed to repaint while a measurement is
-- running, in seconds.
--
-- This is not a frame rate, it is a correction. Repainting six hundred by
-- seven hundred pixels ten times a second and having the compositor blit
-- them is real work on the same single core the benchmark is trying to
-- measure, and it showed: the same machine scored around 100 from `score`
-- on the serial line and around 55 here. Twice a second is enough to
-- watch, and puts the two within a few percent of each other.
--
-- It cannot be removed entirely. A benchmark with a window is measuring a
-- machine that has a window, and that honestly is the number a desktop
-- gets. `score` is the one to quote when comparing two machines, and it
-- says so on the last line here.
local REDRAW = 0.5

-- `direct` because this window draws its own pixels rather than sending
-- the compositor a list of widgets. Without it there is no shared region,
-- `win:surface()` answers nil, and the first frame dies indexing it -
-- which is exactly what happened, and left a correctly-titled window with
-- nothing inside it.
local win, err = ui.window{ title = "System Benchmark", w = W, h = H,
                            x = 120, y = 20, direct = true }

if not win then
  print("sysbench: " .. tostring(err))
  return
end

local TESTS   = bench.TESTS
local results = {}
local index   = 1
local current = bench.measure(TESTS[1])
local score   = nil
local saved   = false
local started = bench.now()
local last_drawn = 0

--------------------------------------------------------------------------
-- Finishing.
--
-- The report is written to the disk as well as drawn, and that is the
-- point of a score rather than a flourish: an application's `print` does
-- not reach the serial line, because the window manager owns the console
-- while it runs. A number that exists only in this window is a number
-- nobody can copy, keep, or compare against another machine.
--------------------------------------------------------------------------

local function finish()
  score = bench.total(results)

  local text = table.concat(bench.report(results), "\n") .. "\n"

  saved = fs.write("/home/sysbench.txt", text) and true or false

  bench.cleanup()
end

--------------------------------------------------------------------------
-- How a rate is written down.
--
-- Three significant figures and a prefix, because "48425462.0" is a number
-- nobody can compare at a glance and "48.4 M" is.
--------------------------------------------------------------------------

local function rate(n)
  if n >= 1e9 then return ("%.2f G"):format(n / 1e9) end
  if n >= 1e6 then return ("%.2f M"):format(n / 1e6) end
  if n >= 1e3 then return ("%.1f k"):format(n / 1e3) end

  return ("%.1f"):format(n)
end

local function draw()
  local s = win:surface()

  -- No buffer to draw into this time round. It happens when this loop
  -- gets ahead of the compositor, which it does constantly here because
  -- it polls without waiting. Skipping the frame is right: the
  -- measurement is the work, and a dropped frame costs nothing.
  if not s then return end

  s:fill(0, 0, W, H, theme.window)

  local y = 12

  s:text(16, y, "System Benchmark", theme.text)
  y = y + ROW

  local cpu = bench.cpu

  s:text(16, y, ("%s %s, %d core%s"):format(cpu.implementer or "?",
                                            cpu.part or "?",
                                            cpu.cores or 1,
                                            (cpu.cores or 1) == 1 and ""
                                                                  or "s"),
         theme.text_dim)
  y = y + ROW + 4

  local shown = nil

  for i, t in ipairs(TESTS) do
    if t.group ~= shown then
      shown = t.group
      y = y + 4
      s:text(16, y, t.group, theme.text)

      local g = bench.group_score(results, t.group)

      if g then
        s:text(W - 84, y, ("%.0f"):format(g), theme.text)
      end

      y = y + ROW
    end

    local r      = results[i]
    local colour = theme.text_dim

    if i == index and not score then
      colour = theme.text
      s:fill(8, y - 2, 4, ROW - 4, theme.accent)
    end

    s:text(32, y, t.name, colour)

    if r then
      if r.skipped then
        s:text(290, y, "not on this machine", colour)
      else
        s:text(290, y, rate(r.rate) .. " " .. t.unit, colour)

        if r.score then
          s:text(W - 84, y, ("%.0f"):format(r.score), colour)
        end
      end
    end

    y = y + ROW
  end

  y = H - 62

  s:fill(16, y, W - 32, 6, theme.line)
  s:fill(16, y, math.floor((W - 32) * (index - 1) / #TESTS), 6, theme.accent)

  y = y + 18

  if score then
    s:text(16, y, ("Kosmos Mark: %.0f"):format(score), theme.text)
    s:text(200, y, ("%d measurements, %.0f seconds")
                   :format(#TESTS, bench.now() - started), theme.text_dim)
    s:text(200, y + 18, saved and "saved to /home/sysbench.txt"
                        or "not saved: there is no disk", theme.text_dim)
  else
    s:text(16, y, ("%d of %d"):format(index, #TESTS), theme.text)
    s:text(200, y, "measuring; the window still moves", theme.text_dim)
    s:text(200, y + 18, "`score` measures the same, without a desktop",
           theme.text_dim)
  end

  win:commit{ x = 0, y = 0, w = W, h = H }
end



--------------------------------------------------------------------------
-- One slice of measurement, one frame, repeat.
--------------------------------------------------------------------------

while win.running do
  if current then
    local ok, value, why = coroutine.resume(current)

    if not ok then
      results[index] = { skipped = true, why = tostring(value) }
      current = nil
    elseif coroutine.status(current) == "dead" then
      if type(value) == "number" then
        bench.record(results, index, value)
      else
        results[index] = { skipped = true, why = tostring(why) }
      end

      current = nil
    end

    if not current then
      -- A failed setup takes the rest of its group with it. There is no
      -- point timing a read of a file that could not be written, and the
      -- four failures that would follow read as four faults rather than
      -- the one missing disk they actually are.
      if results[index] and results[index].skipped then
        local group = TESTS[index].group

        while TESTS[index + 1] and TESTS[index + 1].group == group do
          index = index + 1
          results[index] = { skipped = true, why = "the group before it" }
        end
      end

      index = index + 1

      if TESTS[index] then
        current = bench.measure(TESTS[index])
      else
        finish()
      end
    end
  end

  -- Drawn on a clock rather than every slice, for the reason above
  -- REDRAW: the painting competes with the thing being measured.
  if score or (bench.now() - last_drawn) >= REDRAW then
    last_drawn = bench.now()
    draw()
  end

  -- No wait at all while there is measuring left: this application is
  -- deliberately trying to use the whole processor, and a poll that slept
  -- would be timing the sleep. Once it has a score it waits like anything
  -- else, because an idle desktop should be idle.
  local reply = fs.send("/app/wm", { type = "poll", window = win.handle,
                                     wait = score and 1 or 0 })

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "key" and ev.code == 3 then
      win:close()
    end
  end
end
