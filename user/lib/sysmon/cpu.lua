-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's CPU page: Monitor's window, moved in whole, with each
-- core's temperature beside its speed (`docs/sysmonitor.md` S3; inventory
-- rows 1-18).
--
--   local page = use("/Kosmos/Libraries/sysmon/cpu.lua").new{
--     S = S, w = 680, pace = function() return pace end }
--   page:measure()            -- page.h, after a change to what is shown
--   page:draw(g)              -- in the page's own coordinates
--
-- **As `docs/monitor.html` draws it**, agreed 11 October 2026. Diego: "cpu
-- monitor app needs a refresh", "Put the cpu name and model in the app not
-- in the title as it can't be shown entirely", "each core bar is
-- unnecessary", "Just leave the graphs with its history, cpu speed, kernel
-- and user colors and core number", "Show the cpu name and model in a nice
-- way", "Bars are a 'show cores bars' option". So:
--
--   - **the processor in a card of its own**, its model large, how many
--     processors and the clock it is rated at under it, its features under
--     those, and on the right how fast the machine is running now;
--   - **a graph a core**, its number, its speed, its temperature and its
--     share above it, the last minute in it - the kernel's time in red
--     under the thread's own in green, as 24 September asked: "historical
--     graph data ... and the kernel and user space times is great in red and
--     green";
--   - **each core's bar**, what Monitor was before, behind "Show each core's
--     bar" in the dots' menu, off every time the window opens.
--
-- **How many there are decides the shape** - "what if the processor has 16
-- cores?" - and the page is never taller than the screen: four across at
-- 84 tall up to eight, at 60 from nine to sixteen, eight across in a window
-- twice as wide to thirty-two, and past that one graph for the machine.
--
-- **Each core's temperature, where a reading exists** (S3): Nebula reads
-- `IA32_THERM_STATUS` on each core's tick, and `sys.cpuload()[c].temp_c`
-- carries it. QEMU's processor gives none, so there a graph says its speed
-- and share as before and nothing that looks like a reading.
--
-- **And under the cores' graphs, since S6**: the package's temperature
-- against TjMax and its power against PL1 and PL2 over the history window
-- - limits Nebula has read since the heat arrived and nothing had drawn -
-- with "held" marked where it was held (`M.package_graphs`, which the Heat
-- and power page draws too); and **who ran**, the Scheduler's lanes,
-- read-only, by the kit's `ui.lanes`, with switches a second, the keys'
-- longest wait and which application has game mode. "Change in Scheduler"
-- opens the Scheduler, which stays the place to change them (rows 96-99).
--
-- **Nothing here asks anything.** The page reads what the window's sample
-- (`sysmon/sample.lua`) last heard: the processor's meter, the heat's, the
-- histories. A repaint is drawing and nothing else.

local ui = use("/Kosmos/Libraries/ui.lua")
local pulse = use("/Kosmos/Libraries/pulse.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

-- Fixed colours, as the drawing has them: red and green are a sign here,
-- the same in every look, and a graph's ground is dark in all of them
-- because that is what makes a column of colour readable at a glance.
M.USER_BAR     = 0xff2fa84f
M.KERNEL_BAR   = 0xffe5484d
M.USER_COL     = 0xff34c759
M.KERNEL_COL   = 0xffff453a
M.GRAPH_GROUND = 0xff16181d
M.HEAT_COL     = 0xffff8a5c         -- a temperature, the drawing's `--heat`
M.WATT_COL     = 0xffffc04d         -- a power, its `--watt`
M.HELD_COL     = 0xffe3a04f         -- where it was held

-- The words for what this machine does not measure, in a measurement's
-- place and never as a nought (`sysmonitor.md`, *Sensors*).
M.UNMEASURED = "not measured on this machine"

--
-- **The processor, said once and whole**: its maker, model and rated clock
-- (`pulse.processor`), its features (`pulse.processor_features`, read once
-- - a processor's features do not change - its virtualization green when
-- it is on), and what goes on its chip's lid: "Core i7-6700" is "i7-6700"
-- there, "Cortex-A76" is "A76". A maker with no name here - QEMU's
-- processor says only its model - is the model's first word on the lid,
-- and nothing under it. The Overview's tile draws the same chip.
--
function M.processor(cores)
  local maker, model, rated = pulse.processor()
  local facts = ("%d %s"):format(cores, cores == 1 and "processor" or "processors")

  if rated then facts = facts .. " \u{b7} rated " .. rated end

  local features, words = {}, {}

  for _, f in ipairs(pulse.processor_features()) do
    features[#features + 1] = { f[1], f[2] and theme.good or nil }
    words[#words + 1] = f[1]
  end

  local lid = (maker or "") ~= "" and maker or ((model or ""):match("^(%S+)") or "")
  local part = (maker or "") ~= "" and ((model or ""):match("(%S+)$") or ""):gsub("^Cortex%-", "") or ""

  return { maker = maker, model = model, rated = rated, facts = facts,
           features = features, feature_words = table.concat(words, " \u{b7} "),
           lid = lid, part = part }
end

--
-- **The package's heat and power in words**, as the CPU page's graphs and
-- the Heat and power page say them, each with its limit where Nebula read
-- one: "44 °C · hottest core 46 °C · slows at 100 °C", "12.4 W · PL1 65 W
-- sustained · PL2 81 W a burst · never held". A machine that keeps none
-- says so in the reading's place.
--
function M.temperature_words(S)
  local heat = S.hot.heat

  if not (heat and heat.temp_c) then return M.UNMEASURED end

  local parts = { ("%d °C"):format(heat.temp_c) }
  local hottest = S:hottest()

  if hottest then parts[#parts + 1] = ("hottest core %d °C"):format(hottest) end
  if heat.limit_c then parts[#parts + 1] = ("slows at %d °C"):format(heat.limit_c) end

  return table.concat(parts, " \u{b7} ")
end

-- How many of the history's columns were held.
function M.held_columns(S)
  local n = 0

  for _, col in ipairs(S.held_h) do
    if col[1] then n = n + 1 end
  end

  return n
end

function M.held_words(S)
  local heat = S.hot.heat

  if not (heat and heat.limit_c) then return M.UNMEASURED end
  if heat.held == "hot" then return "held now: too hot" end
  if heat.held == "power" then return "held now: power limit" end

  local n = M.held_columns(S)

  return n > 0 and ("held in %d of %d columns"):format(n, #S.held_h) or "never held"
end

function M.power_words(S)
  local heat = S.hot.heat

  if not (heat and heat.watts) then return M.UNMEASURED end

  local parts = { ("%.1f W"):format(heat.watts) }

  if heat.limit_w then parts[#parts + 1] = ("PL1 %.0f W sustained"):format(heat.limit_w) end
  if heat.burst_w then parts[#parts + 1] = ("PL2 %.0f W a burst"):format(heat.burst_w) end
  parts[#parts + 1] = M.held_words(S)

  return table.concat(parts, " \u{b7} ")
end

-- A power graph's top: PL2 where it was read, else PL1, else the next
-- five watts above the most it holds.
local function watts_top(S)
  local heat = S.hot.heat
  local most = ui.history.most(S.watts_h)

  if heat and heat.burst_w and heat.burst_w >= most then return heat.burst_w end
  if heat and heat.limit_w and heat.limit_w >= most then return heat.limit_w end

  return math.max(5, math.ceil(most / 5) * 5)
end

M.PACKAGE_TOP = 22                      -- a package graph's line of words above it
M.HELD_H = 5                            -- the held marks under the power's graph

--
-- **The package's two graphs side by side**, `w` wide and `h` tall each
-- with its words above it and the held marks under the power's: its
-- temperature to TjMax (to 100 °C where none was read), and its power to
-- PL2 with a line at PL1. Drawn by the CPU page and the Heat and power
-- page; the height the pair takes is `M.PACKAGE_TOP + h + 3 + M.HELD_H`.
--
function M.package_graphs(g, x, y, w, h, S)
  local heat = S.hot.heat
  local gap = 12
  local gw = (w - gap) // 2
  local columns = S.columns

  local function name_line(px, title, words)
    local room = gw - 4 - gfx.measure(title, "label") - 10

    g:text(px + 2, y + (M.PACKAGE_TOP - 4 - gfx.height("label")) // 2, title, theme.text, theme.window, "label")
    g:text(px + gw - 2 - gfx.measure(ui.fitted(words, room, "ui"), "ui"),
           y + (M.PACKAGE_TOP - 4 - gfx.height("ui")) // 2,
           ui.fitted(words, room, "ui"), theme.text_dim, theme.window, "ui")
  end

  local gy = y + M.PACKAGE_TOP

  name_line(x, "The package's temperature", M.temperature_words(S))
  ui.history.draw(g, x, gy, gw, h, S.heat_h, {
    mode = "line", columns = columns, ground = M.GRAPH_GROUND, radius = 6,
    most = heat and heat.limit_c or 100, colours = { M.HEAT_COL } })

  local px = x + gw + gap
  local top = watts_top(S)

  name_line(px, "The package's power", M.power_words(S))
  ui.history.draw(g, px, gy, gw, h, S.watts_h, {
    mode = "line", columns = columns, ground = M.GRAPH_GROUND, radius = 6,
    most = top, colours = { M.WATT_COL } })

  -- PL1, a line across the graph where it lies under the top.
  if heat and heat.limit_w and heat.limit_w < top then
    local inner = h - 4
    local ly = gy + 2 + inner - math.floor(inner * heat.limit_w / top + 0.5)

    g:fill(px + 2, ly, gw - 4, 1, theme.line_soft)
  end

  -- Held, a mark a column it was held in.
  ui.history.draw(g, px, gy + h + 3, gw, M.HELD_H, S.held_h, {
    mode = "stacked", columns = columns, ground = M.GRAPH_GROUND, radius = 2,
    colours = { M.HELD_COL } })
end

function M.new(spec)
  local S = spec.S
  local CORES = S.cores
  local W = spec.w
  local pace = spec.pace
  local P = spec.processor or M.processor(CORES)

  local WIDE     = CORES > 16 and CORES <= 32
  local MACHINE  = CORES > 32           -- one graph for all, a graph a core on asking
  local ACROSS   = WIDE and 8 or 4
  local GRAPH_H  = (CORES <= 8) and 84 or 60
  local GRAPH_TOP = 22                  -- a graph's line of words above it
  local GAP_X, GAP_Y = 12, 12
  local DIE      = 64                   -- the chip with its maker's mark (13 October, "B for the chip")
  local CHIP_FACE = ui.sized("title", 21)
  -- Its model, its facts and its features' line, and 13 above and below,
  -- and at least the chip and 13 above and below it.
  local CHIP_H   = math.max(26 + gfx.height(CHIP_FACE) + gfx.height("ui")
                   * (#P.features > 0 and 2 or 1), DIE + 27)
  local BAR_ROW  = 40                   -- a row of the bars, when they are shown
  local BAR_SPLIT = CORES > 8           -- the bars in two columns
  local PACKAGE_H = 72                  -- each of the package's two graphs
  local LANE_H   = CORES > 16 and 6 or 12   -- who ran: a lane a core, as the drawing has them
  local LANE_GAP = CORES > 16 and 2 or 3
  local INKS     = ui.lanes.inks((sys.scheduler() or {}).band_names)
  local MINI_H   = 8 + gfx.height("title") + gfx.height("label") + 8
  local SCHEDULER = "/Kosmos/Apps/scheduler.lua"

  local page = { name = "cpu", title = "CPU", w = W, show_bars = false, per_core = not MACHINE,
                 machine = MACHINE, processor = P, chip_size = DIE }

  local function graphs_shown()
    return page.per_core and CORES or 1
  end

  -- Where everything goes, from one function: the bars shown or not change
  -- only what this says.
  function page:measure()
    local inner = self.w - 2 * L.page_side
    local m = { inner = inner }
    local y = L.page_top

    m.chip = y
    y = y + CHIP_H + L.between

    if self.show_bars then
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

    -- The package's two graphs under the cores' (S6).
    y = y + L.between
    m.package = y
    y = y + M.PACKAGE_TOP + PACKAGE_H + 3 + M.HELD_H

    -- Who ran: the lanes at the left, the three numbers stacked at the
    -- right, as the drawing has them.
    y = y + L.between
    m.who = y
    y = y + L.group + 8
    m.lanes = y
    m.lanes_w = (inner - GAP_X) * 8 // 13
    m.mini_x = L.page_side + m.lanes_w + GAP_X
    m.mini_w = inner - m.lanes_w - GAP_X
    y = y + math.max(ui.lanes.height(CORES, { lane_h = LANE_H, gap = LANE_GAP }), 3 * MINI_H + 2 * 6)

    m.foot = y + 10
    m.h = m.foot + gfx.height() + L.page_foot
    self.m, self.h = m, m.h
    return m
  end

  local function die(g, x, y)
    ui.processor_chip(g, x, y, DIE, P.lid, P.part)
  end

  -- The heat and power line, where the processor says: "62 °C · 28 W",
  -- and why it is held when it is.
  local function heat_words()
    local heat = S.hot.heat

    if not heat then return nil end

    local parts = { heat.temp_c and ("%d °C"):format(heat.temp_c) or nil }

    if heat.watts then parts[#parts + 1] = ("%.0f W"):format(heat.watts) end
    if heat.held then parts[#parts + 1] = heat.held == "hot" and "held: too hot" or "held: power limit" end

    return #parts > 0 and table.concat(parts, " \u{b7} ") or nil
  end

  local function draw_chip(g, x, y, w)
    local ghz = S:speed()
    local big = ghz and ("%.2f GHz"):format(ghz) or P.rated or ""
    local note = heat_words() or (ghz and "now, across all cores" or (P.rated and "rated" or ""))

    -- The kit's card (`ui.meter_card`), with the die as its picture. The
    -- maker is on the chip, so not beside the model as well.
    ui.meter_card(g, x, y, w, CHIP_H, {
      picture = die, size = DIE, model_face = CHIP_FACE, inline = true, line = P.features,
      maker = nil, model = P.model, facts = P.facts,
      now = big ~= "" and { { big, theme.text, CHIP_FACE }, { note, theme.text_dim, "ui" } } or nil,
    })
  end

  -- A core's speed and temperature, in words, or nil for each it has not.
  local function speed_of(c)
    local speeds = S.cpu.speeds

    return speeds and speeds[c] or nil
  end

  local function draw_bars(g, x, y, w)
    local m = page.m
    local rows = m.bar_rows
    local h = rows * BAR_ROW + (rows - 1) + 2
    local cpu = S.cpu

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
      local u, k = cpu.user[c], cpu.kernel[c]

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
      if kw + uw > 0 then g:fill_round(bx, by, math.max(kw + uw, 6), 6, M.USER_BAR, 3) end
      if kw > 0 then g:fill_round(bx, by, math.max(kw, 6), 6, M.KERNEL_BAR, 3) end

      local mhz = speed_of(c)
      local ghz = mhz and ("%.2f GHz"):format(mhz / 1000) or nil
      local pct = ("%d%%"):format(u + k)
      local ty = ry + (BAR_ROW - nh) // 2

      if ghz then
        g:text(rx + cw - L.row_in - pct_w - L.row_in - gfx.measure(ghz), ty, ghz,
               theme.text_dim, theme.raised, "ui")
      end

      g:text(rx + cw - L.row_in - gfx.measure(pct), ty, pct, theme.text, theme.raised, "ui")
    end
  end

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
      mode = "stacked", columns = S.columns, ground = M.GRAPH_GROUND,
      colours = { M.KERNEL_COL, M.USER_COL },
    })
  end

  --
  -- A core's words above its graph, longest first: its speed, its
  -- temperature and its share; then each shortened; then its share alone.
  --
  function page:core_words(c)
    local mhz, temp = speed_of(c), S.temps[c]
    local pct = ("%d%%"):format(S.cpu.user[c] + S.cpu.kernel[c])
    local hot = temp and ("%d °C"):format(temp) or nil
    local out = {}

    local function add(...)
      local parts = {}

      for i = 1, select("#", ...) do
        local p = select(i, ...)

        if p then parts[#parts + 1] = p end
      end

      out[#out + 1] = table.concat(parts, " \u{b7} ")
    end

    if mhz then add(("%.2f GHz"):format(mhz / 1000), hot, pct) end
    if mhz then add(("%.1f GHz"):format(mhz / 1000), hot, pct) end
    if hot then add(hot, pct) end
    if mhz then add(("%.1f GHz"):format(mhz / 1000)) end

    out[#out + 1] = pct
    return out
  end

  -- "8 processors · 23% busy": the heading over the graphs (row 16).
  function page:heading()
    return ("%d %s \u{b7} %d%% busy"):format(CORES, CORES == 1 and "processor" or "processors",
                                             S:busy())
  end

  function page:draw(g)
    local m = self.m or self:measure()
    local x = L.page_side

    draw_chip(g, x, m.chip, m.inner)

    if self.show_bars then draw_bars(g, x, m.bars, m.inner) end

    local right = self:heading()
    local p = pace()

    g:text(x + 3, m.heading + (L.group - gfx.height("heading")) // 2, p.heading,
           theme.text, theme.window, "heading")
    g:text(x + m.inner - gfx.measure(right), m.heading + (L.group - gfx.height()) // 2, right,
           theme.text_dim, theme.window, "ui")

    if self.per_core then
      for c = 1, CORES do
        local col = (c - 1) % m.across
        local row = (c - 1) // m.across
        local px = x + col * (m.graph_w + GAP_X)
        local py = m.graphs + row * (GRAPH_TOP + m.graph_h + GAP_Y)

        draw_graph(g, px, py, m.graph_w, m.graph_h, "Core " .. c, self:core_words(c), S.core[c])
      end
    else
      local ghz = S:speed()

      draw_graph(g, x, m.graphs, m.inner, m.graph_h, "Every core",
                 { (ghz and ("%.2f GHz \u{b7} "):format(ghz) or "") .. S:busy() .. "%",
                   S:busy() .. "%" }, S.all)
    end

    M.package_graphs(g, x, m.package, m.inner, PACKAGE_H, S)
    self:draw_who(g, x, m)

    g:text(x + 3, m.foot, p.foot, theme.text_dim, theme.window, "ui")
  end

  --
  -- **Who ran**, read-only: the kit's lanes, and switches a second, the
  -- keys' longest wait and game mode as the Scheduler's tiles say them.
  -- "Change in Scheduler ›" at the heading's right opens the Scheduler.
  --
  local LINK = "Change in Scheduler \u{203a}"

  function page:who_words()
    local who, game = S.who_ran, S.game
    local game_words = not game and "asked while this page shows"
                       or game.pid and ("on for %s"):format(tostring(game.title or game.pid)) or "off"

    return {
      switches = who.switches and tostring(who.switches) or "not counted yet",
      keys = who.keys_ms and ("%.2f ms"):format(who.keys_ms) or "not counted yet",
      game = game_words,
    }
  end

  function page:draw_who(g, x, m)
    local note = "a lane a core, a colour a band \u{b7} "
    local hy = m.who + (L.group - gfx.height("heading")) // 2
    local ny = m.who + (L.group - gfx.height()) // 2
    local lw = gfx.measure(LINK)
    local lx = x + m.inner - lw

    g:text(x + 3, hy, "Who ran, the last two seconds", theme.text, theme.window, "heading")
    g:text(lx - gfx.measure(note), ny, note, theme.text_dim, theme.window, "ui")
    g:text(lx, ny, LINK, theme.accent, theme.window, "ui")
    m.link = { x = lx, y = m.who, w = lw, h = L.group }

    ui.lanes.draw(g, x, m.lanes, m.lanes_w, S.who_ran.lanes, {
      cores = CORES, inks = INKS, lane_h = LANE_H, gap = LANE_GAP, background = theme.window })

    local words = self:who_words()
    local my = m.lanes

    for _, t in ipairs({ { words.switches, "switches a second" }, { words.keys, "the keys' longest wait" },
                         { words.game, "game mode" } }) do
      g:fill_round(m.mini_x, my, m.mini_w, MINI_H, theme.raised, 9)
      g:frame_round(m.mini_x, my, m.mini_w, MINI_H, theme.line_soft, 9)
      g:text(m.mini_x + 10, my + 8, ui.fitted(t[1], m.mini_w - 20, "title"), theme.text, theme.raised, "title")
      g:text(m.mini_x + 10, my + 8 + gfx.height("title"), ui.fitted(t[2], m.mini_w - 20, "label"),
             theme.text_dim, theme.raised, "label")
      my = my + MINI_H + 6
    end
  end

  -- A click on "Change in Scheduler ›": the Scheduler opened, on the
  -- release over the link it was pressed on.
  function page:mouse(action, px, py)
    return ui.click(self, action, px, py, function(cx, cy)
      local r = self.m and self.m.link

      if r and cx >= r.x and cx < r.x + r.w and cy >= r.y and cy < r.y + r.h then
        return "scheduler", function()
          fs.send("/Running/wm", { type = "launch", program = SCHEDULER })
          print("sysmon: cpu opens the Scheduler")
        end
      end

      return nil
    end)
  end

  --
  -- **What this page shows, a row of `docs/sysmonitor.md`'s inventory
  -- each**: its number, its words and its value now, which `--tell` prints
  -- and `tools/run_sysmon.py` holds to the document - a row missing here is
  -- a metric lost (*Every metric today*, rows 1-18).
  --
  local function lanes_read()
    local n = 0

    for c = 1, CORES do
      if S.who_ran.lanes[c] then n = n + 1 end
    end

    return n
  end

  local function lane_ticks()
    local lane = S.who_ran.lanes[1]

    return lane and #lane.bands or 0
  end

  function page:rows()
    local cpu, heat = S.cpu, S.hot.heat
    local busy, split, speeds = {}, {}, {}

    for c = 1, CORES do
      busy[#busy + 1] = ("%d:%d%%"):format(c, cpu.user[c] + cpu.kernel[c])
      split[#split + 1] = ("%d:%d/%d"):format(c, cpu.user[c], cpu.kernel[c])
      speeds[#speeds + 1] = speed_of(c) and ("%d:%.2f GHz"):format(c, speed_of(c) / 1000) or nil
    end

    local ghz = S:speed()
    local p = pace()
    local filled = #(S.core[1] or {})

    return {
      { 1, "The chip", ("%s | %s, %d square"):format(P.lid, P.part, DIE) },
      { 2, "The model", P.model ~= "" and P.model or "-" },
      { 3, "How many processors", ("%d %s"):format(CORES, CORES == 1 and "processor" or "processors") },
      { 4, "The rated clock", P.rated and ("rated " .. P.rated) or "not said by this processor" },
      { 5, "The features", P.feature_words ~= "" and P.feature_words or "none said" },
      { 6, "How fast it runs now", ghz and ("%.2f GHz, now, across all cores"):format(ghz) or M.UNMEASURED },
      { 7, "The package's temperature", heat and heat.temp_c and ("%d °C"):format(heat.temp_c) or M.UNMEASURED },
      { 8, "The package's power", heat and heat.watts and ("%.1f W"):format(heat.watts) or M.UNMEASURED },
      { 9, "Why it is held", heat and heat.limit_c
                              and (heat.held == "hot" and "held: too hot"
                                   or heat.held == "power" and "held: power limit" or "not held")
                              or M.UNMEASURED },
      { 10, "Each core busy", table.concat(busy, " ") },
      { 11, "Each core user/kernel", table.concat(split, " ") },
      { 12, "Each core's speed", #speeds > 0 and table.concat(speeds, " ") or M.UNMEASURED },
      { 13, "A graph a core", ("%d %s of %d columns, %d taken, kernel under user"):format(
              graphs_shown(), graphs_shown() == 1 and "graph" or "graphs", S.columns, filled) },
      { 14, "Show each core's bar", self.show_bars and "on" or "off" },
      { 15, "Past 32 cores", MACHINE and (self.per_core and "a graph a core, on asking" or "one graph")
                              or ("a graph a core, %d cores"):format(CORES) },
      { 16, "The heading", self:heading() },
      { 17, "The pace", p.menu:lower() },
      { 18, "The words over the history", p.heading .. " | " .. p.foot },
      { 96, "Who ran, the last two seconds", ("%d lanes, %d read, %d ticks each"):format(
              CORES, lanes_read(), lane_ticks()) },
      { 97, "Switches a second", self:who_words().switches },
      { 98, "The keys' longest wait", self:who_words().keys },
      { 99, "Game mode", self:who_words().game },
    }
  end

  -- Each core's temperature, for `--tell`: what S3 added beside the speed.
  function page:temperatures()
    local out = {}

    for c = 1, CORES do
      if S.temps[c] then out[#out + 1] = ("%d:%d °C"):format(c, S.temps[c]) end
    end

    return #out > 0 and table.concat(out, " ") or M.UNMEASURED
  end

  page:measure()
  return page
end

return M
