-- kosmos: application
-- kosmos: icon System_Kernel
-- kosmos: name Scheduler
-- kosmos: section preferences
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- How Kosmos shares its processors, shown so a person can see it.
--
--   wm scheduler
--
-- **As `docs/scheduler.html` draws it** (`docs/scheduler.md` G2; Diego, 11
-- October 2026: "it should be redesigned to better explain what it does,
-- how changing the scheduler affects performance and responsiveness, and
-- else in a visual way", and of the drawing: "Perfect"):
--
--   - **the modes as cards**, each saying what it is for, what it buys and
--     what it costs - Balanced (the priority policy), Fair (round robin)
--     and Game mode, which is the next step (G3) and drawn as such;
--   - **who ran**: a lane for each processor over the last two seconds,
--     a colour a band, from what each core records at every tick
--     (`sys.sched_lane`);
--   - **the bands as a ladder**, each with what lives in it;
--   - **a turn** - the quantum - with what a shorter or longer one trades;
--   - **the numbers that matter**: how often the processors switch, the
--     longest the keys waited, and a game's slowest frame once there is
--     game mode.
--
-- A choice takes effect at once on every core and is kept for every start
-- (`scheduling.lua`, G1).

local ui         = use("/Kosmos/Libraries/ui.lua")
local text       = use("/Kosmos/Libraries/text.lua")
local scheduling = use("/Kosmos/Libraries/scheduling.lua")
local theme      = ui.theme
local L          = ui.layout

local TELL = (tostring(args or "")):find("%-%-tell") ~= nil

local info = sys.scheduler()

if not info then
  print("scheduler: this machine does not report its scheduler")
  return
end

local cpu = fs.read("/Devices/cpu") or {}
local counter_hz = cpu.counter_hz or 62500000
local CORES = 1

-- As many processors as answer for themselves.
do
  local n = 0
  while sys.sched_lane(n + 1) do n = n + 1 end
  CORES = math.max(1, n)
end

-- The bands as the kernel numbers them (`kernel/sched.h`), and their
-- colours as the drawing has them - a sign, the same in every look.
local INPUT, AUDIO, DISPLAY, NORMAL, LOW = 5, 4, 3, 2, 1
local IDLE = 255
local BAND_INK = {
  [INPUT] = 0xffe85d75, [AUDIO] = 0xffb07cf0, [DISPLAY] = 0xff4f8cf0,
  [NORMAL] = 0xff4bb07a, [LOW] = 0xff3a7a5a, [0] = 0xff2f5c47,
}
local GAME_INK = 0xfff0b43c
local GOOD, COST = 0xff7fdc9d, 0xffe3a04f
local SELECT = 0xff2f6bdc

local W = 960
local PAD, GAP = 16, 12

--------------------------------------------------------------------------
-- The modes.
--------------------------------------------------------------------------

local MODES = {
  { key = "priority", title = "Balanced", tag = "priority",
    text = "The desktop first: keys, sound and the screen never wait. Everything else takes turns. Chosen, it applies at once and at every start.",
    good = "Feels instant", cost = "a busy band below can wait" },
  { key = "round-robin", title = "Fair", tag = "round-robin",
    text = "Every thread takes the same turns, whoever it is. For comparing, and for seeing why Balanced exists. Chosen, it applies at once and at every start.",
    good = "Nothing starves", cost = "the pointer waits behind a build" },
  { key = "game", title = "Game mode", tag = "next",
    text = "The game in front gets a band and a core of its own. The screen, sound and keys stay above it. On and off at once, no restart.",
    good = "Steady frames", cost = "other programs share one core fewer", later = true },
}

local RUNGS = {
  { band = INPUT, name = "Input", text = "keys and the pointer, read the moment they come" },
  { band = AUDIO, name = "Audio", text = "sound - Groove, Music - never late" },
  { band = DISPLAY, name = "Display", text = "the window manager and the window server" },
  { band = "game", name = "Game", text = "nobody - game mode is the next step" },
  { band = NORMAL, name = "Normal", text = "applications, servers, builds, downloads" },
  { band = LOW, name = "Low", text = "what can wait: indexing, thumbnails" },
}

local TURNS_MS = { 4, 12, 20, 40, 100 }

local function policy_name() return info.policies[info.policy] or "?" end

local function mode_of_policy()
  for i, m in ipairs(MODES) do
    if m.key == policy_name() then return i end
  end
end

--------------------------------------------------------------------------
-- What the processors did: sampled twice a second.
--------------------------------------------------------------------------

local lanes = {}
local before = nil                  -- { switches, at } from the last sample
local switches_per_second = nil
local keys_wait_ms = nil
local said = "read from the kernel, not remembered"

local function sample()
  local total, keys = 0, 0

  for c = 1, CORES do
    local lane = sys.sched_lane(c)

    if lane then
      lanes[c] = lane
      total = total + lane.switches
      keys = math.max(keys, lane.waits[INPUT + 1] or 0)
    end
  end

  local now = sys.ticks()

  if before and now > before.at then
    switches_per_second = math.floor((total - before.switches) * counter_hz / (now - before.at) + 0.5)
  end

  before = { switches = total, at = now }
  keys_wait_ms = keys > 0 and keys * 1000 / counter_hz or 0
end

local function refresh(words)
  info = sys.scheduler() or info
  said = words or said
end

local function choose_mode(i)
  local m = MODES[i]

  if m.later then
    refresh("game mode is the next step")
    print("scheduler: game mode is the next step")
    return
  end

  local index = scheduling.index_of(m.key, info)

  if not index then
    refresh(("no policy called %s here"):format(m.key))
    return
  end

  local ok, why = sys.set_policy(index)

  if ok then scheduling.keep{ policy = m.key } end

  refresh(ok and ("%s, on every core now and at every start"):format(m.title)
          or ("%s was refused: %s"):format(m.title, tostring(why)))
  print(("scheduler: chose %s - %s, the kernel says %s"):format(m.key, ok and "in force" or tostring(why),
        policy_name()))
end

local function choose_turn(ms)
  local ticks = math.max(1, (ms * info.tick_hz + 500) // 1000)
  local ok, why = sys.set_quantum(ticks)

  if ok then scheduling.keep{ quantum = ticks } end

  refresh(ok and ("a turn is now %d ms, and at every start"):format(ms)
          or ("a turn of %d ms was refused: %s"):format(ms, tostring(why)))
  print(("scheduler: a turn of %d ms - %s"):format(ms, ok and "in force" or tostring(why)))
end

--------------------------------------------------------------------------
-- The page: measured from the faces in force, not copied from the
-- drawing's pixels, so a larger look's type still fits.
--------------------------------------------------------------------------

local LBL = "label"
local LH = gfx.height(LBL)
local UH = gfx.height("ui")
local MODE_W = (W - 2 * PAD - 2 * GAP) // 3
local MODE_TEXT_W = MODE_W - 24

local mode_h = 0

for _, m in ipairs(MODES) do
  local lines = #ui.wrapped(m.text, MODE_TEXT_W, LBL)
  local verdict = #ui.wrapped("✓ " .. m.good .. "  – " .. m.cost, MODE_TEXT_W, LBL)

  mode_h = math.max(mode_h, 10 + UH + 4 + lines * LH + 4 + verdict * LH + 10)
end

-- A turn: its title, its note under it, the choices, the bar, and what the
-- two ends trade - on one line where they fit, else one under the other.
local TRADE_LEFT, TRADE_RIGHT = "shorter: snappier, more switching", "longer: more work done"
local function trade_lines(w)
  return gfx.measure(TRADE_LEFT, "label") + 16 + gfx.measure(TRADE_RIGHT, "label") <= w - 24 and 1 or 2
end
local TURN_NOTE = "how long a thread runs before an equal gets the processor"

local LEFT_W = (W - 2 * PAD - GAP) * 155 // 255
local RIGHT_W = W - 2 * PAD - GAP - LEFT_W
local TURN_NOTE_LINES = #ui.wrapped(TURN_NOTE, RIGHT_W - 24, "label")
local TURN_H = 10 + LH + TURN_NOTE_LINES * LH + 6 + UH + 4 + 10 + 6 + 6 + trade_lines(RIGHT_W) * LH + 10
local LANE_H = math.max(12, LH - 4)
local LANES_H = 10 + LH + 8 + CORES * (LANE_H + 6) + 6 + LH + 10
local TILE_H = 8 + gfx.height("title") + LH + 8
local LADDER_H = 10 + LH + 8 + #RUNGS * (LH + 8) + 6 + LH + 10

local BODY_TOP = GAP
local MODES_Y = BODY_TOP
local PANES_Y = MODES_Y + mode_h + GAP
local LEFT_H = LANES_H + GAP + TILE_H
local RIGHT_H = LADDER_H + GAP + TURN_H
local H = L.head + PANES_Y + math.max(LEFT_H, RIGHT_H) + PAD

local win, err = ui.window{ title = "Scheduler", w = W, h = H, x = 120, y = 70, header = true }

if not win then
  print("scheduler: " .. tostring(err))
  return
end

local function sub()
  local m = MODES[mode_of_policy() or 1]

  return ("how Kosmos shares its %d processor%s \u{b7} %s"):format(CORES, CORES == 1 and "" or "s",
         mode_of_policy() and m.title or policy_name())
end

local icon = ui.view{ x = 0, y = 0, w = 26, h = 26 }
function icon:draw(g) g:icon(0, 0, "System_Kernel.png", 26) end

local head = ui.header{ x = 0, y = 0, w = W, title = "Scheduler", sub = sub(), left = { icon }, title_bar = true }

win:add(head)

local page = ui.view{ x = 0, y = L.head, w = W, h = H - L.head }
local spots = {}                    -- what a click lands on, made as it is drawn

local function pane(g, x, y, w, h)
  g:fill_round(x, y, w, h, theme.sunken, 10)
  g:frame_round(x, y, w, h, theme.line_soft, 10)
end

local function pane_title(g, x, y, w, title, note)
  g:text(x + 12, y + 10, title, theme.text, nil, LBL)

  if note then
    g:text(x + w - 12 - gfx.measure(note, LBL), y + 10, note, theme.text_dim, nil, LBL)
  end
end

local function draw_modes(g)
  local using = mode_of_policy()

  for i, m in ipairs(MODES) do
    local x, y = PAD + (i - 1) * (MODE_W + GAP), MODES_Y
    local on = i == using
    local tag = on and "in use" or m.tag
    local tw = gfx.measure(tag, LBL) + 14

    g:fill_round(x, y, MODE_W, mode_h, on and theme.mix(theme.raised, SELECT, 150) or theme.raised, 11)
    g:frame_round(x, y, MODE_W, mode_h, on and SELECT or theme.line_soft, 11)
    if on then g:frame_round(x + 1, y + 1, MODE_W - 2, mode_h - 2, SELECT, 10) end

    g:text(x + 12, y + 10, m.title, m.later and theme.text_dim or theme.text, nil, "ui")
    g:fill_round(x + MODE_W - 12 - tw, y + 11, tw, LH, on and SELECT or theme.sunken, LH // 2)
    g:text(x + MODE_W - 12 - tw + 7, y + 11, tag, on and 0xffffffff or theme.text_dim, nil, LBL)

    local ty = y + 10 + UH + 4

    for _, line in ipairs(ui.wrapped(m.text, MODE_TEXT_W, LBL)) do
      g:text(x + 12, ty, line, m.later and theme.text_dim or theme.text, nil, LBL)
      ty = ty + LH
    end

    ty = ty + 4

    -- What it buys in green and what it costs in amber, wrapped together.
    local good = "\u{2713} " .. m.good
    local lines = ui.wrapped(good .. "  \u{2013} " .. m.cost, MODE_TEXT_W, LBL)

    for li, line in ipairs(lines) do
      if li == 1 then
        local gw = gfx.measure(good, LBL)
        g:text(x + 12, ty, good, GOOD, nil, LBL)
        g:text(x + 12 + gw, ty, line:sub(#good + 1), COST, nil, LBL)
      else
        g:text(x + 12, ty, line, COST, nil, LBL)
      end

      ty = ty + LH
    end

    spots[#spots + 1] = { x = x, y = y, w = MODE_W, h = mode_h, key = "mode " .. m.key,
                          act = function() choose_mode(i) end }
  end
end

local function lane_ink(band)
  if band == IDLE then return nil end

  return BAND_INK[band] or BAND_INK[NORMAL]
end

local function draw_lanes(g, x, y, w)
  pane(g, x, y, w, LANES_H)
  pane_title(g, x, y, w, "Who ran, the last two seconds", "a lane a core, a colour a band")

  local label_w = gfx.measure("Core " .. CORES, LBL) + 10
  local lx, lw = x + 12 + label_w, w - 24 - label_w
  local ly = y + 10 + LH + 8
  local N = 512

  for c = 1, CORES do
    local lane = lanes[c]

    g:text(x + 12, ly + (LANE_H - LH) // 2, "Core " .. c, theme.text_dim, nil, LBL)
    g:fill_round(lx, ly, lw, LANE_H, 0xff15181d, 3)

    if lane then
      -- Oldest at the left: tick (ticks - N) to tick (ticks - 1), a column
      -- of pixels a stretch of ticks, runs of one colour drawn as one.
      local start = lane.ticks - N
      local run_ink, run_x = nil, lx

      for px = 0, lw do
        local ink = nil

        if px < lw then
          local t = start + (px * N) // lw

          if t >= 0 then
            ink = lane_ink(lane.bands:byte(t % N + 1))
          end
        end

        if ink ~= run_ink or px == lw then
          if run_ink then g:fill(run_x, ly, lx + px - run_x, LANE_H, run_ink) end
          run_ink, run_x = ink, lx + px
        end
      end
    end

    ly = ly + LANE_H + 6
  end

  -- The legend.
  local gx = x + 12
  ly = ly + 6

  for _, b in ipairs({ { "input", BAND_INK[INPUT] }, { "audio", BAND_INK[AUDIO] }, { "display", BAND_INK[DISPLAY] },
                       { "game", GAME_INK }, { "normal", BAND_INK[NORMAL] }, { "low", BAND_INK[LOW] } }) do
    g:fill_round(gx, ly + (LH - 9) // 2, 9, 9, b[2], 2)
    g:text(gx + 13, ly, b[1], theme.text_dim, nil, LBL)
    gx = gx + 13 + gfx.measure(b[1], LBL) + 12
  end

  g:text(gx, ly, "empty = idle", theme.text_dim, nil, LBL)
end

local function draw_tiles(g, x, y, w)
  local tw = (w - 2 * 8) // 3
  local tiles = {
    { switches_per_second and text.grouped(switches_per_second) or "\u{2013}",
      "Switches a second" },
    { keys_wait_ms and (keys_wait_ms > 0 and ("%.1f ms"):format(keys_wait_ms) or "0 ms") or "\u{2013}",
      "Keys' longest wait" },
    { "\u{2013}", "A game's slowest frame, with game mode" },
  }

  for i, t in ipairs(tiles) do
    local tx = x + (i - 1) * (tw + 8)

    g:fill_round(tx, y, tw, TILE_H, theme.raised, 9)
    g:frame_round(tx, y, tw, TILE_H, theme.line_soft, 9)
    g:text(tx + 10, y + 8, t[1], theme.text, nil, "title")
    g:text(tx + 10, y + 8 + gfx.height("title"), ui.fitted(t[2], tw - 20, LBL), theme.text_dim, nil, LBL)
  end
end

local function draw_ladder(g, x, y, w)
  local fair = policy_name() == "round-robin"

  pane(g, x, y, w, LADDER_H)
  pane_title(g, x, y, w, "The bands")

  local ry = y + 10 + LH + 8
  local bw = 78

  for _, r in ipairs(RUNGS) do
    local ink = r.band == "game" and GAME_INK or BAND_INK[r.band]
    local dim = r.band == "game" or fair

    g:fill_round(x + 12, ry, bw, LH + 2, dim and theme.mix(theme.sunken, ink, 430) or ink, 5)
    g:text(x + 12 + (bw - gfx.measure(r.name, LBL)) // 2, ry + 1, r.name, dim and theme.text_dim or 0xff12151b, nil, LBL)
    g:text(x + 12 + bw + 10, ry + 1, ui.fitted(r.text, w - 24 - bw - 10, LBL), dim and theme.text_dim or theme.text, nil, LBL)
    ry = ry + LH + 8
  end

  g:text(x + 12, ry + 2, ui.fitted(fair and "Fair puts every thread in one queue: the bands are not asked"
         or "\u{2191} a higher band always runs first; inside a band, they take turns", w - 24, LBL),
         theme.text_dim, nil, LBL)
end

local function draw_turn(g, x, y, w)
  pane(g, x, y, w, TURN_H)
  pane_title(g, x, y, w, "A turn")

  local ny = y + 10 + LH

  for _, line in ipairs(ui.wrapped(TURN_NOTE, w - 24, LBL)) do
    g:text(x + 12, ny, line, theme.text_dim, nil, LBL)
    ny = ny + LH
  end

  local now_ms = info.quantum * 1000 // info.tick_hz
  local sx, sy = x + 12, ny + 6
  local cell_h = UH + 4

  for i, ms in ipairs(TURNS_MS) do
    local word = ("%d ms"):format(ms)
    local cw = gfx.measure(word, LBL) + 18
    local on = ms == now_ms

    g:fill(sx, sy, cw, cell_h, on and SELECT or theme.raised)
    g:frame_round(sx, sy, cw, cell_h, theme.line_soft, 0)
    g:text(sx + 9, sy + (cell_h - LH) // 2, word, on and 0xffffffff or theme.text, nil, LBL)
    spots[#spots + 1] = { x = sx, y = sy, w = cw, h = cell_h, key = "turn " .. ms,
                          act = function() choose_turn(ms) end }
    sx = sx + cw
  end

  if not (function() for _, ms in ipairs(TURNS_MS) do if ms == now_ms then return true end end end)() then
    g:text(sx + 10, sy + (cell_h - LH) // 2, ("now %d ms"):format(now_ms), theme.text_dim, nil, LBL)
  end

  -- What it trades: shorter is snappier and switches more, longer gets more
  -- work done; the marker where the turn in force sits between 4 and 100.
  local by = sy + cell_h + 10
  local bw = w - 24
  local steps = 24

  for i = 0, steps - 1 do
    local mix = (i * 1000) // (steps - 1)
    g:fill(x + 12 + (i * bw) // steps, by, bw // steps + 1, 6, theme.mix(0xff4f8cf0, 0xfff0b43c, mix))
  end

  local share = math.max(0, math.min(1, (math.log(math.max(now_ms, 4)) - math.log(4)) / (math.log(100) - math.log(4))))
  g:fill_round(x + 12 + math.floor(share * (bw - 4)), by - 3, 4, 12, 0xffffffff, 2)

  g:text(x + 12, by + 12, TRADE_LEFT, theme.text_dim, nil, LBL)

  if trade_lines(w) == 1 then
    g:text(x + w - 12 - gfx.measure(TRADE_RIGHT, LBL), by + 12, TRADE_RIGHT, theme.text_dim, nil, LBL)
  else
    g:text(x + w - 12 - gfx.measure(TRADE_RIGHT, LBL), by + 12 + LH, TRADE_RIGHT, theme.text_dim, nil, LBL)
  end
end

local told = false

function page:draw(g)
  spots = {}
  draw_modes(g)

  local lx, rx = PAD, PAD + LEFT_W + GAP

  draw_lanes(g, lx, PANES_Y, LEFT_W)
  draw_tiles(g, lx, PANES_Y + LANES_H + GAP, LEFT_W)
  draw_ladder(g, rx, PANES_Y, RIGHT_W)
  draw_turn(g, rx, PANES_Y + LADDER_H + GAP, RIGHT_W)

  if TELL and not told then
    told = true

    local where = {}
    for _, s in ipairs(spots) do
      where[#where + 1] = ("%s@%d,%d"):format(s.key, s.x + s.w // 2, L.head + s.y + s.h // 2)
    end

    print("scheduler: spots " .. table.concat(where, " | "))
  end
end

function page:mouse(action, x, y)
  return ui.click(self, action, x, y, function(px, py)
    for _, s in ipairs(spots) do
      if px >= s.x and px < s.x + s.w and py >= s.y and py < s.y + s.h then
        return s.key, s.act
      end
    end
  end)
end

win:add(page)

--
-- **Twice a second**: the lanes, the switches and the waits read again, and
-- the header saying what is in force.
--
local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }
local said_lanes = false

function clock:tick()
  sample()
  head.sub = said ~= "read from the kernel, not remembered" and said or sub()

  if TELL and not said_lanes and switches_per_second then
    said_lanes = true
    print(("scheduler: lanes %d, %d switches a second, keys waited %.2f ms, policy %s"):format(
          CORES, switches_per_second, keys_wait_ms or 0, policy_name()))
  end

  win:paint()
end

win:add(clock)

sample()
print(("scheduler: window at %d,%d, %d by %d, %d cores, policy %s"):format(
      win.origin_x or 0, win.origin_y or 0, W, H, CORES, policy_name()))

win:run()
