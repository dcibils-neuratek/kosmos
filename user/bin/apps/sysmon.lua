-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pulse
-- kosmos: section system
-- The processors, in a window: what each is doing now, and for the last
-- minute.
--
-- **As `docs/apps.html` draws it** (`roadmap.md` 5zx). Diego, 24 September,
-- with macOS's CPU History beside it: "the monitor app needds some
-- historical graph data like we have on mac os", "its good to have the
-- current usage but historical graph data is also required and also havint
-- the kernel and user space times is great in red and green".
--
-- So: a row a core, its bar the kernel's time in red under the thread's own
-- in green; and under the card a panel a core on a dark ground, a column a
-- second, newest at the right, the same two colours stacked the same way.
--
-- **Busy is ticks; the split is counters.** `sys.cpuload` says a core's
-- busy and idle in scheduler ticks, and its busy time split into user and
-- kernel in the counter's units, measured at each crossing between a
-- thread's code and the kernel (`kernel/thread.c`, `time_cross`). The share
-- of busy that was the kernel's is the ratio of the two counters, and the
-- two clocks never meet in one sum.
--
-- The care every reader of these counters needs once: **a percentage is
-- the difference between two readings.** Totals since boot, per core; the
-- subtractions below are the whole of what makes them a meter.
--
-- The processor meters this replaced were BeOS's Pulse (`/lib/pulse.lua`),
-- which `cores` still draws.

local ui = use("/lib/ui.lua")
-- The kit's palette, not a copy: only the one `ui.lua` holds is the one it
-- changes when the desktop changes look.
local theme = ui.theme
local pulse = use("/lib/pulse.lua")
local L = ui.layout

local load0 = sys.cpuload() or {}
local CORES = math.max(1, #load0)

--
-- The drawing's measures. A row is the kit's least, 48, for two lines of
-- words; the history is two panels across, 84 tall, with 10 between rows of
-- them and 12 between the two in a row.
--
-- **The name's column is as wide as the widest note it can hold**, not the
-- drawing's 118: that was 12-point words, and at the machine's 18 the note
-- ran under the start of the bar.
--
local W        = 560
local ROW      = L.row_min
local NAME_W   = gfx.measure("100% user \u{b7} 100% kernel") + L.row_in
local PCT_W    = 40
local COLUMNS  = 60          -- a minute, a column a second
local PANEL_H  = 84
local PANEL_GAP_X = 12
local PANEL_GAP_Y = 10
local PANEL_NAME  = 18
local ACROSS   = (CORES <= 4) and 2 or 4
local DOWN     = (CORES + ACROSS - 1) // ACROSS

-- Fixed colours, as the drawing has them: red and green are a sign here,
-- the same in every look, and the history's ground is dark in all of them
-- because that is what makes a column of colour readable at a glance.
local USER_BAR    = 0xff2fa84f
local KERNEL_BAR  = 0xffe5484d
local USER_COL    = 0xff34c759
local KERNEL_COL  = 0xffff453a
local PANEL_GROUND = 0xff16181d

local card_h = CORES * ROW + (CORES - 1) + 2
local hist_top = L.head + L.page_top + card_h + L.between
local hist_h = DOWN * (PANEL_NAME + PANEL_H) + (DOWN - 1) * PANEL_GAP_Y
local H = hist_top + L.to_card + hist_h + 30 + L.page_foot

local win, err = ui.window{ title = "Monitor", w = W, h = H, x = 90, y = 130 }

if not win then
  print("sysmon: " .. tostring(err))
  return
end

--
-- What each core did in the last half second, for its row, and in each of
-- the last sixty seconds, for its panel: `{ user, kernel }` in per cent.
--
local now = {}
local history = {}

for c = 1, CORES do
  now[c] = { 0, 0 }
  history[c] = {}
end

--------------------------------------------------------------------------
-- The header: the subject, the processor, and what the two colours mean.
--------------------------------------------------------------------------

local ident = pulse.identity()
local processor = table.concat({ ident[2] or "", ident[3] or "" }, " ")
                  :match("^%s*(.-)%s*$")
local sub = ("%d %s"):format(CORES, CORES == 1 and "core" or "cores")

if processor ~= "" then sub = sub .. " \u{b7} " .. processor end

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

win:add(ui.header{ x = 0, y = 0, w = W, title = "Monitor", sub = sub,
                   right = { legend } })

--------------------------------------------------------------------------
-- The card: a row a core.
--------------------------------------------------------------------------

local rows = ui.view{ x = L.page_side, y = L.head + L.page_top,
                      w = W - 2 * L.page_side, h = card_h }

function rows:draw(g)
  local w = self.w

  g:fill_round(0, 0, w, self.h, theme.raised, L.card_r)
  g:frame_round(0, 0, w, self.h, theme.line_soft, L.card_r)

  local lh, nh = gfx.height("label"), gfx.height()
  local words = L.line + L.note

  for c = 1, CORES do
    local y = 1 + (c - 1) * (ROW + 1)
    local u, k = now[c][1], now[c][2]

    if c > 1 then g:fill(1, y - 1, w - 2, 1, theme.line_soft) end

    local top = y + (ROW - words) // 2

    g:text(L.row_in, top + (L.line - lh) // 2, "Core " .. c,
           theme.text, theme.raised, "label")
    g:text(L.row_in, top + L.line + (L.note - nh) // 2,
           ("%d%% user \u{b7} %d%% kernel"):format(u, k),
           theme.text_dim, theme.raised, "ui")

    -- The bar: the kernel's part first, from the left, and the thread's
    -- own after it - `docs/apps.html`'s `.bar i.k` and `.bar i.u`.
    local bx = L.row_in + NAME_W
    local bw = w - bx - L.row_in - PCT_W - L.row_in
    local by = y + (ROW - 8) // 2
    local kw = (bw * k) // 100
    local uw = (bw * u) // 100

    g:fill_round(bx, by, bw, 8, theme.track, 4)

    if kw + uw > 0 then
      g:fill_round(bx, by, math.max(kw + uw, 8), 8, USER_BAR, 4)
    end

    if kw > 0 then
      g:fill_round(bx, by, math.max(kw, 8), 8, KERNEL_BAR, 4)
    end

    local pct = ("%d%%"):format(u + k)

    g:text(w - L.row_in - gfx.measure(pct), y + (ROW - nh) // 2, pct,
           theme.text_dim, theme.raised, "ui")
  end
end

win:add(rows)

win:add(ui.label{ x = L.page_side + 3,
                  y = hist_top + (L.group - gfx.height("heading")) // 2,
                  w = W - 2 * L.page_side, text = "The last minute",
                  role = "heading" })

--------------------------------------------------------------------------
-- The history: a panel a core, a column a second.
--------------------------------------------------------------------------

local panels = ui.view{ x = L.page_side, y = hist_top + L.to_card,
                        w = W - 2 * L.page_side, h = hist_h }

function panels:draw(g)
  local pw = (self.w - (ACROSS - 1) * PANEL_GAP_X) // ACROSS
  local nh = gfx.height()

  for c = 1, CORES do
    local col = (c - 1) % ACROSS
    local row = (c - 1) // ACROSS
    local px = col * (pw + PANEL_GAP_X)
    local py = row * (PANEL_NAME + PANEL_H + PANEL_GAP_Y)

    g:text(px, py + (PANEL_NAME - 4 - nh) // 2, "Core " .. c,
           theme.text_dim, theme.window, "ui")

    local top = py + PANEL_NAME

    g:fill_round(px, top, pw, PANEL_H, PANEL_GROUND, 8)

    -- The newest at the right edge; each column a sixtieth of the panel,
    -- and a pixel of ground between two when there is room for one.
    local h = history[c]
    local inner = PANEL_H - 4

    for i = 1, #h do
      local slot = COLUMNS - #h + i - 1
      local x0 = px + (slot * pw) // COLUMNS
      local x1 = px + ((slot + 1) * pw) // COLUMNS
      local cw = math.max(1, x1 - x0 - 1)
      local kh = (inner * h[i][2]) // 100
      local uh = (inner * h[i][1]) // 100
      local base = top + PANEL_H - 2

      if kh > 0 then g:fill(x0, base - kh, cw, kh, KERNEL_COL) end
      if uh > 0 then g:fill(x0, base - kh - uh, cw, uh, USER_COL) end
    end
  end
end

win:add(panels)

win:add(ui.label{ x = L.page_side + 3, y = hist_top + L.to_card + hist_h + 10,
                  w = W - 2 * L.page_side,
                  text = "A column a second; the newest at the right.",
                  color = "text_dim", role = "ui" })

--------------------------------------------------------------------------
-- The sampling, on the window kit's own clock: twice a second for the rows,
-- and every second tick a column for the history. A tick repaints the
-- window by itself (`window:run`), so nothing here asks for it.
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
local second = load0
local halves = 0

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local load = sys.cpuload()

  if not load then return end

  for c = 1, CORES do
    if load[c] and last[c] then
      now[c][1], now[c][2] = split(last[c], load[c])
    end
  end

  last = load
  halves = halves + 1

  if halves >= 2 then
    halves = 0

    for c = 1, CORES do
      if load[c] and second[c] then
        local h = history[c]

        h[#h + 1] = { split(second[c], load[c]) }

        if #h > COLUMNS then table.remove(h, 1) end
      end
    end

    second = load
  end
end

win:add(sampler)
win:run()
