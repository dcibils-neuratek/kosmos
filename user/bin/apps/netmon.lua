-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Network
-- kosmos: name Network Monitor
-- kosmos: section system
-- kosmos: needs network
-- What goes in and out of this machine, now and over the last minute.
--
--   wm netmon
--
-- Diego, 12 October 2026: "i realizsed we need network monitor app as well
-- to see network input and output". Drawn in `docs/netmonitor.html` and
-- agreed ("mockup is perfect! go ahead and build it"); `docs/netmonitor.md`
-- is what it is made of, and this was step N3.
--
-- **Its body is System Monitor's Network page now** (`docs/sysmonitor.md`
-- S5): `/Kosmos/Libraries/sysmon/network.lua` draws the card, the minute,
-- the totals and the connections, here and there - one body, not two that
-- look alike. This window is the header, the dots and a sample that asks
-- only the network (`sysmon/sample.lua`, its parts named). S9 retires it,
-- and `netmon` opens System Monitor on its Network page (decision 3).
--
-- **It counts nothing itself.** The network server counts every frame and
-- every connection's bytes (`NET_OP_STATS`, `fs.net_stats`), and the
-- sample asks once a pace and takes the difference between two readings.

local ui = use("/Kosmos/Libraries/ui.lua")
local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
local net_page = use("/Kosmos/Libraries/sysmon/network.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- Monitor's three paces, and what sixty columns of each come to.
local PACES = {
  { half = 1, menu = "Update every 0.5 s", heading = "The last 30 seconds",
    foot = "A column every half second; the newest at the right." },
  { half = 2, menu = "Update every 1 s",   heading = "The last minute",
    foot = "A column a second; the newest at the right." },
  { half = 4, menu = "Update every 2 s",   heading = "The last 2 minutes",
    foot = "A column every two seconds; the newest at the right." },
}
local pace = PACES[2]

-- `--tell`: a line a sample, for `tools/run_netmon.py` to read the rates
-- off. Not without it: a line a second for as long as the window is open
-- filled the M700's log on 12 October.
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil
local bits = false                      -- rates in bits a second, from the menu

local function pace_ticks(p) return counter_hz * p.half // 2 end

local S = sample.new(counter_hz, 60, { network = true })

S.pace_ticks = pace_ticks(pace)
S:want("connections", true)

local page = net_page.new{ S = S, pace = function() return pace end, bits = function() return bits end }

print(("netmon: card %s | %s | %s"):format(page.maker, page.model, page.facts))

local win, err = ui.window{ title = "Network Monitor", w = page.w, h = L.head + page.h,
                            x = 110, y = 110, header = true }

if not win then
  print("netmon: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The header: the name, the two colours, the dots.
--------------------------------------------------------------------------

local legend = ui.view{ w = 2 * (10 + 6) + 14 + gfx.measure("In") + gfx.measure("Out"), h = 26 }

function legend:draw(g)
  local y, ty, x = (self.h - 10) // 2, (self.h - gfx.height()) // 2, 0

  for _, part in ipairs({ { theme.io_in, "In" }, { theme.io_out, "Out" } }) do
    g:fill_round(x, y, 10, 10, part[1], 3)
    x = x + 10 + 6
    g:text(x, ty, part[2], theme.text_dim, theme.raised, "ui")
    x = x + gfx.measure(part[2]) + 14
  end
end

local set_pace
local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  local items = {}

  for _, p in ipairs(PACES) do
    items[#items + 1] = { text = p.menu, mark = (p == pace),
                          on_choose = function() set_pace(p) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Bytes a second", mark = not bits,
                        on_choose = function() bits = false print("netmon: bytes") end }
  items[#items + 1] = { text = "Bits a second", mark = bits,
                        on_choose = function() bits = true print("netmon: bits") end }

  local m = win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)

  if m then
    print(("netmon: more menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
  end
end

win:add(ui.header{ x = 0, y = 0, w = page.w, title = "Network Monitor",
                   right = { legend, more }, title_bar = true })

local body = ui.view{ x = 0, y = L.head, w = page.w, h = page.h }

function body:draw(g)
  if not self.told and more.x then
    local gx, gy, gw, gh = page:graph_box()

    print(("netmon: window at %d,%d; more at %d,%d; graph at %d,%d, %dx%d"):format(
          win.origin_x or 0, win.origin_y or 0, more.x + more.w // 2, more.y + more.h // 2,
          gx, L.head + gy, gw, gh))
    self.told = true
  end

  page:draw(g)
end

win:add(body)

--------------------------------------------------------------------------
-- The sampling, on the window kit's clock set to the pace.
--------------------------------------------------------------------------

local function take()
  S:take()

  if TELL and S.net_s then print("netmon: " .. page:tell_line()) end
end

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick() take() end

win:add(sampler)

set_pace = function(p)
  pace = p
  win.tick_every = pace_ticks(p)
  win.last_tick = sys.ticks()
  S.pace_ticks = win.tick_every

  -- Begun again, from a reading taken now: columns at one pace beside
  -- columns at another would be a time axis that lies.
  S:restart()
  take()
  print(("netmon: %s, %s"):format(p.menu:lower(), p.heading:lower()))
end

take()
win.tick_every = pace_ticks(pace)
win:run()
