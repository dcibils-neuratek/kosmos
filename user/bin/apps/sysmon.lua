-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pulse
-- kosmos: name System Monitor
-- kosmos: section system
-- The processors, in a window: what each is doing now, and for the last
-- minute.
--
-- **As `docs/monitor.html` draws it**, agreed 11 October 2026. Diego: "cpu
-- monitor app needs a refresh", "Put the cpu name and model in the app not
-- in the title as it can't be shown entirely", "each core bar is
-- unnecessary", "Just leave the graphs with its history, cpu speed, kernel
-- and user colors and core number", "Show the cpu name and model in a nice
-- way", "Bars are a 'show cores bars' option". So:
--
--   - **the processor in a card of its own**, its maker small above its
--     model large, how many processors and the clock it is rated at under
--     them, and on the right how fast the machine is running now;
--   - **a graph a core**, its number, its speed and its share above it, the
--     last minute in it - the kernel's time in red under the thread's own in
--     green, as 24 September asked: "historical graph data ... and the
--     kernel and user space times is great in red and green";
--   - **each core's bar**, what this window was before, behind "Show each
--     core's bar" in the dots' menu, off every time a Monitor opens.
--
-- **How many there are decides the shape** - "what if the processor has 16
-- cores?" - and the window is never taller than the screen: four across at
-- 84 tall up to eight, at 60 from nine to sixteen, eight across in a window
-- twice as wide to thirty-two, and past that one graph for the machine.
--
-- **How often it samples is chosen in the dots' menu** (`roadmap.md` 6p):
-- every half second, second or two seconds, and the history's sixty columns
-- are that many of them.
--
-- **Busy is ticks; the split is counters; the speed is the core's own.**
-- `sys.cpuload` says a core's busy and idle in scheduler ticks, its busy time
-- split into user and kernel in the counter's units, and the two totals the
-- core keeps of how fast it ran (`pulse.speeds`) - which QEMU's processor
-- does not keep, so there the card says the rated clock and the graphs only
-- their share. **A percentage or a speed is the difference between two
-- readings.**

local ui = use("/Kosmos/Libraries/ui.lua")
-- The kit's palette, not a copy: only the one `ui.lua` holds is the one it
-- changes when the desktop changes look.
local theme = ui.theme
local pulse = use("/Kosmos/Libraries/pulse.lua")
local L = ui.layout

local load0 = sys.cpuload() or {}
local CORES = math.max(1, #load0)

-- The counter's rate, for the window's clock and for the speeds, read where
-- it is used as every piece of counter arithmetic here does
-- (`architecture.md` §5).
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

--
-- The three paces, and what sixty columns of each come to. A second to
-- start with, every time a Monitor opens: how often one window looks is
-- that window's to say, as a Terminal's text size is (`settings.lua`).
--
local PACES = {
  { half = 1, menu = "Update every 0.5 s", heading = "The last 30 seconds",
    foot = "A column every half second; the newest at the right." },
  { half = 2, menu = "Update every 1 s",   heading = "The last minute",
    foot = "A column a second; the newest at the right." },
  { half = 4, menu = "Update every 2 s",   heading = "The last 2 minutes",
    foot = "A column every two seconds; the newest at the right." },
}
local pace = PACES[2]

--
-- The drawing's measures, from the core count (`docs/monitor.html`,
-- "Sixteen cores").
--
local WIDE     = CORES > 16 and CORES <= 32
local MACHINE  = CORES > 32           -- one graph for all, a graph a core on asking
local W        = WIDE and 1180 or 620
local ACROSS   = WIDE and 8 or 4
local GRAPH_H  = (CORES <= 8) and 84 or 60
local GRAPH_TOP = 22                  -- a graph's line of words above it
local GAP_X, GAP_Y = 12, 12
local COLUMNS  = 60                   -- a column a sample
local DIE      = 64                   -- the chip with its maker's mark (13 October, "B for the chip")
local CHIP_FACE = ui.sized("title", 21)   -- the drawing's 18 against its 12, at Kosmos's larger "ui"
-- Its model, its facts and its features' line, and 13 above and below: 83
-- on the M700, from the 116 the drawing had before it was condensed.
-- And at least the chip and 13 above and below it, which B's chip makes 91.
local CHIP_H   = math.max(26 + gfx.height(CHIP_FACE) + gfx.height("ui")
                 * (#pulse.processor_features() > 0 and 2 or 1), DIE + 27)
local BAR_ROW  = 40                   -- a row of the bars, when they are shown
local BAR_SPLIT = CORES > 8           -- the bars in two columns

-- Fixed colours, as the drawing has them: red and green are a sign here,
-- the same in every look, and a graph's ground is dark in all of them
-- because that is what makes a column of colour readable at a glance.
local USER_BAR    = 0xff2fa84f
local KERNEL_BAR  = 0xffe5484d
local USER_COL    = 0xff34c759
local KERNEL_COL  = 0xffff453a
local GRAPH_GROUND = 0xff16181d

-- What the window shows, which the menu changes.
local show_bars = false
local per_core = not MACHINE

--
-- What each core did since the last sample, for its line and its bar, and in
-- each of the last sixty, for its graph: `{ user, kernel }` in per cent.
-- And how fast each ran, in MHz, or nil where nobody can say.
--
local now, history, speeds, heat = {}, {}, nil, nil
local all_now, all_history = { 0, 0 }, {}

for c = 1, CORES do
  now[c] = { 0, 0 }
  history[c] = {}
end

--------------------------------------------------------------------------
-- Where everything goes, for the window's height and for the drawing, from
-- one function: the bars shown or not change only what this says.
--------------------------------------------------------------------------

local function graphs_shown()
  return per_core and CORES or 1
end

local function measure()
  local inner = W - 2 * L.page_side
  local m = { inner = inner }
  local y = L.page_top

  m.chip = y
  y = y + CHIP_H + L.between

  if show_bars then
    local rows = BAR_SPLIT and (CORES + 1) // 2 or CORES

    m.bars, m.bar_rows = y, rows
    y = y + rows * BAR_ROW + (rows - 1) + 2 + L.between
  end

  m.heading = y
  y = y + L.group + 8

  local n = graphs_shown()
  local across = (n == 1) and 1 or ACROSS
  local down = (n + across - 1) // across

  m.graphs, m.across, m.down = y, across, down
  m.graph_w = (inner - (across - 1) * GAP_X) // across
  m.graph_h = (n == 1) and 120 or GRAPH_H
  y = y + down * (GRAPH_TOP + m.graph_h) + (down - 1) * GAP_Y

  m.foot = y + 10
  m.h = m.foot + gfx.height() + L.page_foot
  return m
end

local M = measure()

local win, err = ui.window{ title = "Monitor", w = W, h = L.head + M.h, x = 90, y = 130,
                            header = true }

if not win then
  print("sysmon: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The header: the window's name, what the two colours mean, and the dots.
-- The processor is not here any more; it has a card of its own.
--------------------------------------------------------------------------

local legend = ui.view{ w = 2 * (10 + 6) + 14 + gfx.measure("User")
                             + gfx.measure("Kernel"), h = 26 }

function legend:draw(g)
  local fh = gfx.height()
  local y = (self.h - 10) // 2
  local ty = (self.h - fh) // 2
  local x = 0

  for _, part in ipairs({ { USER_COL, "User" }, { KERNEL_COL, "Kernel" } }) do
    g:fill_round(x, y, 10, 10, part[1], 3)
    x = x + 10 + 6
    g:text(x, ty, part[2], theme.text_dim, theme.raised, "ui")
    x = x + gfx.measure(part[2]) + 14
  end
end

-- Declared here and defined with the sampling, which is what they change.
local set_pace, set_bars, set_per_core
local samples, paced_at = 0, sys.ticks()

local more = ui.iconbutton{ icon = "more" }

-- Rebuilt on every press, so each mark is on what is in use.
more.on_click = function()
  local items = {}

  for _, p in ipairs(PACES) do
    items[#items + 1] = { text = p.menu, mark = (p == pace),
                          on_choose = function() set_pace(p) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Show each core's bar", mark = show_bars,
                        on_choose = function() set_bars(not show_bars) end }

  if MACHINE then
    items[#items + 1] = { text = "Show a graph a core", mark = per_core,
                          on_choose = function() set_per_core(not per_core) end }
  end

  local m = win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)

  -- Where the menu is, and how many samples the pace in use has taken in
  -- how long by the counter - which is what `tools/run_sysapps.py` holds
  -- to the pace, since the words above the history could say a second
  -- while the clock kept another.
  if m then
    print(("sysmon: more menu at %d,%d, %d wide, rows of %d; %d samples in %.1f s")
          :format(m.x, m.y, m.w, m.row, samples,
                  (sys.ticks() - paced_at) / counter_hz))
  end
end

win:add(ui.header{ x = 0, y = 0, w = W, title = "Monitor",
                   right = { legend, more },
                   title_bar = true })

--------------------------------------------------------------------------
-- The processor, said once and whole.
--------------------------------------------------------------------------

local MAKER, MODEL, RATED = pulse.processor()
local FACTS = ("%d %s"):format(CORES, CORES == 1 and "processor" or "processors")

if RATED then FACTS = FACTS .. " \u{b7} rated " .. RATED end

print(("sysmon: processor %s | %s | %s"):format(MAKER, MODEL, FACTS))

--
-- **What it can do, in one line under the facts** (`docs/monitor.html`,
-- "The processor's features"; Diego, 13 October: "It's a almost subtitle
-- kind of thing"): `pulse.features`, read once - a processor's features do
-- not change - its virtualization green when it is on.
--
local FEATURES = {}

for _, f in ipairs(pulse.processor_features()) do
  FEATURES[#FEATURES + 1] = { f[1], f[2] and theme.good or nil }
end

do
  local words = {}

  for _, f in ipairs(FEATURES) do words[#words + 1] = f[1] end

  print("sysmon: features " .. table.concat(words, " \u{b7} "))
end

-- The chip, with its maker's mark and its part on the lid (`ui.processor_chip`):
-- "Core i7-6700" is "i7-6700" there, "Cortex-A76" is "A76".
-- A maker with no name here - QEMU's processor says only its model - is
-- the model's first word on the lid, and nothing under it.
local ON_LID = (MAKER or "") ~= "" and MAKER or ((MODEL or ""):match("^(%S+)") or "")
local PART = (MAKER or "") ~= "" and ((MODEL or ""):match("(%S+)$") or ""):gsub("^Cortex%-", "") or ""

local function die(g, x, y)
  ui.processor_chip(g, x, y, DIE, ON_LID, PART)
end

print(("sysmon: chip %s | %s, %d square"):format(ON_LID, PART, DIE))

-- How fast the machine runs now: the mean of the cores that said, in GHz.
local function machine_speed()
  if not speeds then return nil end

  local sum, n = 0, 0

  for c = 1, CORES do
    if speeds[c] then sum, n = sum + speeds[c], n + 1 end
  end

  return n > 0 and (sum / n / 1000) or nil
end

-- The model and the speed, as the drawing has them: the title's face at 21,
-- asked for once - the card condensed (Diego, 13 October: "25% less tall
-- and more condensed"), the maker beside the model rather than above it.
local BIG = CHIP_FACE

local function draw_chip(g, x, y, w)
  local ghz = machine_speed()
  local big = ghz and ("%.2f GHz"):format(ghz) or RATED or ""
  local note = ghz and "now, across all cores" or (RATED and "rated" or "")

  -- How hot and how much power, where the processor says: "62 °C · 28 W",
  -- and why it is held when it is, in place of the note.
  if heat then
    local parts = { heat.temp_c and ("%d °C"):format(heat.temp_c) or nil }

    if heat.watts then parts[#parts + 1] = ("%.0f W"):format(heat.watts) end
    if heat.held then parts[#parts + 1] = heat.held == "hot" and "held: too hot" or "held: power limit" end
    if #parts > 0 then note = table.concat(parts, " · ") end
  end

  -- The kit's card (`ui.meter_card`), with the die as its picture.
  ui.meter_card(g, x, y, w, CHIP_H, {
    picture = die, size = DIE, model_face = BIG, inline = true, line = FEATURES,
    -- The maker is on the chip now, so not beside the model as well.
    maker = nil, model = MODEL, facts = FACTS,
    now = big ~= "" and { { big, theme.text, BIG }, { note, theme.text_dim, "ui" } } or nil,
  })
end

--------------------------------------------------------------------------
-- Each core's bar, when asked for: what this window was before.
--------------------------------------------------------------------------

local function speed_text(c)
  return speeds and speeds[c] and ("%.2f GHz"):format(speeds[c] / 1000) or nil
end

local function draw_bars(g, x, y, w)
  local rows = M.bar_rows
  local h = rows * BAR_ROW + (rows - 1) + 2

  g:fill_round(x, y, w, h, theme.raised, L.card_r)
  g:frame_round(x, y, w, h, theme.line_soft, L.card_r)

  local cols = BAR_SPLIT and 2 or 1
  local cw = w // cols
  local lh, nh = gfx.height("label"), gfx.height()
  local name_w = gfx.measure("100% user \u{b7} 100% kernel") + L.row_in
  local ghz_w = gfx.measure("0.00 GHz") + 8
  local pct_w = gfx.measure("100%")

  for c = 1, CORES do
    local col = (c - 1) // rows
    local r = (c - 1) % rows
    local rx = x + col * cw
    local ry = y + 1 + r * (BAR_ROW + 1)
    local u, k = now[c][1], now[c][2]

    if r > 0 then g:fill(rx + 1, ry - 1, cw - 2, 1, theme.line_soft) end
    if col > 0 and r == 0 then g:fill(rx, y + 1, 1, h - 2, theme.line_soft) end

    local top = ry + (BAR_ROW - (lh + nh)) // 2

    g:text(rx + L.row_in, top, "Core " .. c, theme.text, theme.raised, "label")
    g:text(rx + L.row_in, top + lh, ("%d%% user \u{b7} %d%% kernel"):format(u, k),
           theme.text_dim, theme.raised, "ui")

    -- The bar: the kernel's part first, from the left, and the thread's
    -- own after it - `docs/apps.html`'s `.bar i.k` and `.bar i.u`.
    local bx = rx + L.row_in + name_w
    local bw = cw - (bx - rx) - ghz_w - pct_w - 3 * L.row_in
    local by = ry + (BAR_ROW - 6) // 2
    local kw = (bw * k) // 100
    local uw = (bw * u) // 100

    g:fill_round(bx, by, bw, 6, theme.track, 3)
    if kw + uw > 0 then g:fill_round(bx, by, math.max(kw + uw, 6), 6, USER_BAR, 3) end
    if kw > 0 then g:fill_round(bx, by, math.max(kw, 6), 6, KERNEL_BAR, 3) end

    local ghz = speed_text(c)
    local pct = ("%d%%"):format(u + k)
    local ty = ry + (BAR_ROW - nh) // 2

    if ghz then
      g:text(rx + cw - L.row_in - pct_w - L.row_in - gfx.measure(ghz), ty, ghz,
             theme.text_dim, theme.raised, "ui")
    end

    g:text(rx + cw - L.row_in - gfx.measure(pct), ty, pct, theme.text, theme.raised, "ui")
  end
end

--------------------------------------------------------------------------
-- The graphs: a core each, or the machine's when there are too many.
--------------------------------------------------------------------------

--
-- `words` is a list, longest first: the first that fits beside the name is
-- drawn, and the name always wins. On the M700, four graphs across, "3.41
-- GHz · 100%" ran over "Core 1" - measured in the default face, drawn in
-- the interface's - which QEMU's processor, saying no speed, never showed.
--
local function draw_graph(g, px, py, pw, ph, name, words, h)
  g:text(px + 2, py + (GRAPH_TOP - 4 - gfx.height("label")) // 2, name,
         theme.text, theme.window, "label")

  local room = pw - 4 - gfx.measure(name, "label") - 10

  for _, w in ipairs(words or {}) do
    local ww = gfx.measure(w, "ui")

    if ww <= room then
      g:text(px + pw - 2 - ww, py + (GRAPH_TOP - 4 - gfx.height("ui")) // 2,
             w, theme.text_dim, theme.window, "ui")
      break
    end
  end

  -- The kit's graph of a minute (`ui.history`): kernel time at the floor,
  -- user time on top of it.
  ui.history.draw(g, px, py + GRAPH_TOP, pw, ph, h, {
    mode = "stacked", columns = COLUMNS, ground = GRAPH_GROUND,
    colours = { KERNEL_COL, USER_COL },
  })
end

local function busy_now()
  local sum = 0

  for c = 1, CORES do sum = sum + now[c][1] + now[c][2] end

  return sum // CORES
end

--------------------------------------------------------------------------
-- The page, as one view laid out by `measure`.
--------------------------------------------------------------------------

local body = ui.view{ x = 0, y = L.head, w = W, h = M.h }

function body:draw(g)
  -- Where the window and its dots are, for whoever drives Monitor from
  -- outside: once, at the first paint, when the header has placed them.
  if not self.told and more.x then
    print(("sysmon: window at %d,%d; more at %d,%d"):format(
          win.origin_x or 0, win.origin_y or 0,
          more.x + more.w // 2, more.y + more.h // 2))
    self.told = true
  end

  local x = L.page_side

  draw_chip(g, x, M.chip, M.inner)

  if show_bars then draw_bars(g, x, M.bars, M.inner) end

  local right = ("%d %s \u{b7} %d%% busy"):format(CORES, CORES == 1 and "processor" or "processors",
                                                  busy_now())

  g:text(x + 3, M.heading + (L.group - gfx.height("heading")) // 2, pace.heading,
         theme.text, theme.window, "heading")
  g:text(x + M.inner - gfx.measure(right), M.heading + (L.group - gfx.height()) // 2, right,
         theme.text_dim, theme.window, "ui")

  if per_core then
    for c = 1, CORES do
      local col = (c - 1) % M.across
      local row = (c - 1) // M.across
      local px = x + col * (M.graph_w + GAP_X)
      local py = M.graphs + row * (GRAPH_TOP + M.graph_h + GAP_Y)
      local mhz = speeds and speeds[c]
      local pct = ("%d%%"):format(now[c][1] + now[c][2])
      local words = mhz and {
        ("%.2f GHz \u{b7} %s"):format(mhz / 1000, pct),
        ("%.1f GHz \u{b7} %s"):format(mhz / 1000, pct),
        ("%.1f GHz"):format(mhz / 1000),
        pct,
      } or { pct }

      draw_graph(g, px, py, M.graph_w, M.graph_h, "Core " .. c, words, history[c])
    end
  else
    local ghz = machine_speed()

    draw_graph(g, x, M.graphs, M.inner, M.graph_h, "Every core",
               { (ghz and ("%.2f GHz \u{b7} "):format(ghz) or "") .. busy_now() .. "%",
                 busy_now() .. "%" }, all_history)
  end

  g:text(x + 3, M.foot, pace.foot, theme.text_dim, theme.window, "ui")
end

win:add(body)

--------------------------------------------------------------------------
-- The sampling, on the window kit's own clock set to the pace: a sample a
-- tick, for the lines, the bars and a column of each graph. A tick repaints
-- the window by itself (`window:run`), so nothing here asks for it.
--------------------------------------------------------------------------

--
-- User and kernel, in per cent, between two readings of one core.
--
-- Busy from the ticks; how much of busy was the kernel's from the counters,
-- which are measured where the ticks can only sample (`percpu.h`).
--
local function split(a, b)
  local di, db = b.idle - a.idle, b.busy - a.busy

  if di + db <= 0 then return 0, 0 end

  local busy = (db * 100) // (di + db)
  local du = (b.user_counter or 0) - (a.user_counter or 0)
  local dk = (b.kernel_counter or 0) - (a.kernel_counter or 0)

  if du + dk <= 0 then return busy, 0 end

  local k = (busy * dk + (du + dk) // 2) // (du + dk)

  return busy - k, k
end

local last = load0

-- The heat beside the speed (`pulse.heat`): two readings of `sys.info()` a
-- sample apart, on the counter's clock.
local info_last, info_at = sys.info(), sys.ticks()

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local load = sys.cpuload()

  if not load then return end

  local su, sk = 0, 0

  for c = 1, CORES do
    if load[c] and last[c] then
      local u, k = split(last[c], load[c])
      local h = history[c]

      now[c][1], now[c][2] = u, k
      su, sk = su + u, sk + k
      ui.history.push(h, { k, u }, COLUMNS)
    end
  end

  all_now = { sk // CORES, su // CORES }
  ui.history.push(all_history, all_now, COLUMNS)

  speeds = pulse.speeds(last, load, counter_hz)
  last = load

  local info, at = sys.info(), sys.ticks()

  heat = pulse.heat(info_last, info, counter_hz > 0 and (at - info_at) / counter_hz or nil, load)
  info_last, info_at = info, at
  samples = samples + 1
end

win:add(sampler)

--
-- The window grown or shrunk to what is shown, and everything placed again
-- by `measure`.
--
local function relayout(why)
  M = measure()
  body.h = M.h

  local ok, w, h = win:resize(W, M.h)

  print(("sysmon: %s, %s"):format(why, ok and ("%dx%d"):format(w, h) or "not resized"))
  win:paint()
end

function set_bars(on)
  show_bars = on
  relayout(on and "bars shown" or "bars hidden")
end

function set_per_core(on)
  per_core = on
  relayout(on and "a graph a core" or "one graph")
end

--
-- A pace chosen: the window's clock set to it, and the history begun
-- again, since columns taken at one pace drawn beside columns at another
-- would be a time axis that lies. The words change at once; the first
-- column arrives a pace later, from a reading taken now.
--
function set_pace(p)
  pace = p
  win.tick_every = counter_hz * p.half // 2
  win.last_tick = sys.ticks()
  last = sys.cpuload() or last
  samples, paced_at = 0, win.last_tick

  for c = 1, CORES do history[c] = {} end
  all_history = {}

  print(("sysmon: %s, %s"):format(p.menu:lower(), p.heading:lower()))
end

-- After the sampler, which is what makes the kit read a clock at all.
win.tick_every = counter_hz * pace.half // 2
win:run()
