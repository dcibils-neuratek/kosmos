-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Device_Harddisk
-- kosmos: name Disk Monitor
-- kosmos: section system
-- What each drive is reading and writing, now and over the last minute, and
-- which programs are doing it.
--
--   wm diskmon
--
-- Diego, 12 October 2026: "we will need to do another app called Disk
-- Monitor to moniutor input ouptput like we do for network to show all
-- devices and its input output bandwtudh, procesees using io, etc". Drawn in
-- `docs/diskmonitor.html` and agreed; `docs/diskmonitor.md` is what it is
-- made of, and this was step D3.
--
-- **Its body is System Monitor's Disks page now** (`docs/sysmonitor.md`
-- S5): `/Kosmos/Libraries/sysmon/disks.lua` draws the list of drives, the
-- chosen one's card, its minute, its totals and its programs, here and
-- there - one body, not two that look alike. This window is the header,
-- the dots and a sample that asks only the drives (`sysmon/sample.lua`,
-- its parts named). S9 retires it, and `diskmon` opens System Monitor on
-- its Disks page (decision 3).
--
-- **It counts nothing itself**: each drive's server does (D1, D2), and the
-- sample reads `.device` under each drive's path, and the chosen one's
-- `.programs`, once a pace and takes differences.

local ui = use("/Kosmos/Libraries/ui.lua")
local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
local disks_page = use("/Kosmos/Libraries/sysmon/disks.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil   -- a line a sample, for its suite

local PACES = {
  { half = 1, menu = "Update every 0.5 s", heading = "The last 30 seconds",
    foot = "A column every half second; the newest at the right." },
  { half = 2, menu = "Update every 1 s",   heading = "The last minute",
    foot = "A column a second; the newest at the right." },
  { half = 4, menu = "Update every 2 s",   heading = "The last 2 minutes",
    foot = "A column every two seconds; the newest at the right." },
}
local pace = PACES[2]
local bits = false

local function pace_ticks(p) return counter_hz * p.half // 2 end

local S = sample.new(counter_hz, 60, { drives = true })

S.pace_ticks = pace_ticks(pace)
S:want("programs", "chosen")

local page = disks_page.new{ S = S, pace = function() return pace end, bits = function() return bits end,
                             prefix = "diskmon:" }

local win, err = ui.window{ title = "Disk Monitor", w = page.w, h = L.head + page.h,
                            x = 90, y = 90, header = true }

if not win then
  print("diskmon: " .. tostring(err))
  return
end

local legend = ui.view{ w = 2 * (10 + 6) + 14 + gfx.measure("Reading") + gfx.measure("Writing"), h = 26 }

function legend:draw(g)
  local y, ty, x = (self.h - 10) // 2, (self.h - gfx.height()) // 2, 0

  for _, part in ipairs({ { theme.io_in, "Reading" }, { theme.io_out, "Writing" } }) do
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
    items[#items + 1] = { text = p.menu, mark = (p == pace), on_choose = function() set_pace(p) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Bytes a second", mark = not bits,
                        on_choose = function() bits = false print("diskmon: bytes") end }
  items[#items + 1] = { text = "Bits a second", mark = bits,
                        on_choose = function() bits = true print("diskmon: bits") end }

  local m = win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)

  if m then
    print(("diskmon: more menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
  end
end

win:add(ui.header{ x = 0, y = 0, w = page.w, title = "Disk Monitor",
                   right = { legend, more }, title_bar = true })

local body = ui.view{ x = 0, y = L.head, w = page.w, h = page.h }

function body:draw(g)
  if not self.told and more.x then
    local gx, gy, gw, gh = page:graph_box()

    print(("diskmon: window at %d,%d; more at %d,%d; graph at %d,%d, %dx%d; list at %d,%d, rows of %d"):format(
          win.origin_x or 0, win.origin_y or 0, more.x + more.w // 2, more.y + more.h // 2,
          gx, L.head + gy, gw, gh, 0, L.head + disks_page.LIST_TOP, disks_page.DEV_H))
    self.told = true
  end

  page:draw(g)
end

function body:mouse(action, x, y) return page:mouse(action, x, y) end

win:add(body)

local function take()
  S:take()

  -- A stick came or went: the list as long as the drives.
  if page:stale() then
    page:measure()
    body.h = page.h
    win:resize(page.w, L.head + page.h)
  end

  local line = TELL and page:tell_line()

  if line then print("diskmon: " .. line) end
end

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick() take() end

win:add(sampler)

set_pace = function(p)
  pace = p
  win.tick_every = pace_ticks(p)
  win.last_tick = sys.ticks()
  S.pace_ticks = win.tick_every
  S:restart()
  take()
  print(("diskmon: %s, %s"):format(p.menu:lower(), p.heading:lower()))
end

take()
win.tick_every = pace_ticks(pace)
win:run()
