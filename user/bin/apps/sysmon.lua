-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pulse
-- kosmos: name System Monitor
-- kosmos: section system
-- kosmos: needs network
-- Every monitor Kosmos has, in one window: the whole machine at a glance,
-- and a page for each part of it (`docs/sysmonitor.md`, drawn in
-- `docs/sysmonitor.html`).
--
--   wm sysmon                  the Overview
--   wm sysmon:cpu              the CPU page
--   wm sysmon:--tell cpu       and a line a sample, for a suite
--
-- Diego, 22 November 2026: "i want to have 1 place to see all the system
-- complnents usage in a way i can see all at once, or specigic components
-- with specific graphs", and "i dont want to loose any of the metrics we
-- have now". **This was Monitor**, the processors in a window, and it is
-- the file, the name and the icon System Monitor keeps (decision 1): its
-- window is the CPU page now (`/Kosmos/Libraries/sysmon/cpu.lua`), with
-- each core's temperature added, and it opens on the Overview
-- (`sysmon/overview.lua`), which the Deskbar's meters open too. Step S3.
--
-- **The window is the thinnest layer** (*Kits supply, applications
-- orchestrate*): it counts nothing. Every number is the kernel's, a
-- driver's or a server's, every difference is `meters.lua`'s, every graph
-- the Series Kit's through `ui.history`, and what is asked when is
-- `sysmon/sample.lua`'s. This holds the sidebar, the header and the dots,
-- shows a page, and says what it shows with `--tell`.
--
-- **The sidebar always shows**, a row for each part of this machine with
-- a live number. A part whose page is not here yet - Memory, Processes,
-- the GPU, the drives, the network, the LLM server, until S4 to S8 move
-- each in - opens the window that shows it today, which stays until its
-- rows have been seen here (S9), so nothing is out of reach in between.
-- Heat and power opens the CPU page, whose card holds the package's heat
-- and power until its own page (S6).
--
-- **What it may do** (decision 7): `needs network`, for the network's
-- counts, which Network Monitor held alone. Nothing it reads is secret.
-- `needs processes` comes with End, in S4.
--
-- **How often it samples is chosen in the dots' menu** (`roadmap.md` 6p):
-- every half second, second or two seconds, for every page at once, and a
-- history's sixty columns are that many of them.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local theme = ui.theme
local L = ui.layout
local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local overview_page = use("/Kosmos/Libraries/sysmon/overview.lua")

local ARGS = tostring(args or "")
local TELL = ARGS:find("%-%-tell") ~= nil     -- a line a sample, for its suite

local START = "overview"

for word in ARGS:gmatch("%S+") do
  if word:sub(1, 1) ~= "-" then START = word:lower() break end
end

-- The counter's rate, for the window's clock and for every rate, read where
-- it is used as every piece of counter arithmetic here does
-- (`architecture.md` §5).
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

--
-- The three paces, and what sixty columns of each come to. A second to
-- start with, every time the window opens: how often one window looks is
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
local bits = false

local function pace_ticks(p) return counter_hz * p.half // 2 end

local S = sample.new(counter_hz, 60)

S.pace_ticks = pace_ticks(pace)

local CORES = S.cores
local WIDE = CORES > 16 and CORES <= 32
local SIDE = 200                              -- the sidebar, `docs/sysmonitor.html`'s 206 less its rule
local MW = WIDE and 1180 or 680               -- a page
local W = SIDE + MW

--------------------------------------------------------------------------
-- The pages, and the windows that show a part until its page is here.
--------------------------------------------------------------------------

local P = cpu_page.processor(CORES)
local function the_pace() return pace end
local function in_bits() return bits end

local pages = {
  cpu = cpu_page.new{ S = S, w = MW, pace = the_pace, processor = P },
  overview = overview_page.new{ S = S, w = MW, pace = the_pace, bits = in_bits, processor = P },
}

local ELSEWHERE = {
  memory    = "/Kosmos/Apps/procs.lua",
  processes = "/Kosmos/Apps/procs.lua",
  gpu       = "/Kosmos/Apps/gpumon.lua",
  disks     = "/Kosmos/Apps/diskmon.lua",
  network   = "/Kosmos/Apps/netmon.lua",
  llm       = "/Kosmos/Apps/llmmon.lua",
}

-- Heat and power has the CPU page's card until its own page (S6).
local STANDS_IN = { heat = "cpu" }

local showing = pages[START] and START or STANDS_IN[START] or "overview"

print(("sysmon: processor %s | %s | %s"):format(P.maker, P.model, P.facts))
print("sysmon: features " .. P.feature_words)
print(("sysmon: chip %s | %s, %d square"):format(P.lid, P.part, pages.cpu.chip_size))

if not S.gpu_lib then print("sysmon: no GPU tile: " .. tostring(S.gpu_why or "no GPU library")) end

--------------------------------------------------------------------------
-- The sidebar: a row a part, each with its number now.
--------------------------------------------------------------------------

local function rate(b) return files.rate(b, bits) end

local function side_items()
  local cpu = S.cpu
  local t = S.totals
  local items = {
    { id = "overview", name = "Overview" },
    { gap = true },
    { heading = true, name = "The machine" },
    { id = "cpu", name = "CPU", note = cpu.taken[1] and ("%d%%"):format(S:busy()) or nil },
    { id = "memory", name = "Memory", note = t.used_mb and ("%.1f GB"):format(t.used_mb / 1024) or nil },
    { id = "processes", name = "Processes", note = S.alive > 0 and tostring(S.alive) or nil },
  }

  if S.gpu_lib then
    items[#items + 1] = { id = "gpu", name = "GPU",
                          note = ("%d%%"):format(math.floor((S.gpu.busy or 0) * 100 + 0.5)) }
  end

  if #S.drives > 0 then
    items[#items + 1] = { id = "disks", name = "Disks" }

    for _, d in ipairs(S.drives) do
      local m = S.drive[d.key]

      items[#items + 1] = { id = "drive:" .. d.key, name = d.name, indent = true,
                            note = m and not m.silent and rate(m.rin + m.rout) or nil }
    end
  end

  if S.card then
    items[#items + 1] = { id = "network", name = "Network", note = rate(S.net.rin + S.net.rout) }
  end

  if S.llm_now then
    local s = S.llm_now

    items[#items + 1] = { id = "llm", name = "LLM",
                          note = s.state == "answering" and S.llm_last
                                 and ("%.1f t/s"):format(S.llm_last) or "idle" }
  end

  local heat = S.hot.heat

  items[#items + 1] = { gap = true }
  items[#items + 1] = { heading = true, name = "Across them" }
  items[#items + 1] = { id = "heat", name = "Heat and power",
                        note = heat and heat.temp_c and ("%d °C"):format(heat.temp_c) or nil }
  return items
end

local side = ui.sidebar{ x = 0, y = L.head + 2, w = SIDE - 1, h = 400, items = side_items(),
                         selected = showing, follow = { "left", "top", "bottom" } }

--
-- **Chosen by the pointer only, until every row is a page** (S9): the
-- arrows choose as they land, and a row whose page is not here yet opens
-- another window - so an arrow held down the list would open five.
--
side.focusable = false

-- How tall the sidebar's rows are, for the window's height.
local function side_need()
  local last = side.items[#side.items]
  local y, h = side:row_of(last.id)

  return (side.y - L.head) + (y or 0) + (h or 0) + L.page_foot
end

local function height()
  return math.max(side_need(), pages[showing].h)
end

local win, err = ui.window{ title = "System Monitor", w = W, h = L.head + height(), x = 90, y = 130,
                            header = true }

if not win then
  print("sysmon: " .. tostring(err))
  return
end

local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = L.head + height(),
                             follow = { "left", "top", "bottom" } }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, "panel")
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
  g:text(L.head_in, (L.head - 1 - gfx.height("title")) // 2, "System Monitor",
         theme.text, nil, "title")
end

side.h = height() - 2

--------------------------------------------------------------------------
-- The header: the page's name, what the two colours mean, and the dots.
--------------------------------------------------------------------------

local legend = ui.view{ w = 2 * (10 + 6) + 14 + gfx.measure("User")
                             + gfx.measure("Kernel"), h = 26 }

function legend:draw(g)
  local fh = gfx.height()
  local y = (self.h - 10) // 2
  local ty = (self.h - fh) // 2
  local x = 0

  for _, part in ipairs({ { cpu_page.USER_COL, "User" }, { cpu_page.KERNEL_COL, "Kernel" } }) do
    g:fill_round(x, y, 10, 10, part[1], 3)
    x = x + 10 + 6
    g:text(x, ty, part[2], theme.text_dim, theme.raised, "ui")
    x = x + gfx.measure(part[2]) + 14
  end
end

-- Declared here and defined with the sampling, which is what they change.
local set_pace, set_bars, set_per_core, set_bits, show
local paced_at = sys.ticks()

local more = ui.iconbutton{ icon = "more" }

-- Rebuilt on every press, so each mark is on what is in use.
more.on_click = function()
  local items = {}

  for _, p in ipairs(PACES) do
    items[#items + 1] = { text = p.menu, mark = (p == pace),
                          on_choose = function() set_pace(p) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Show each core's bar", mark = pages.cpu.show_bars,
                        on_choose = function() set_bars(not pages.cpu.show_bars) end }

  if pages.cpu.machine then
    items[#items + 1] = { text = "Show a graph a core", mark = pages.cpu.per_core,
                          on_choose = function() set_per_core(not pages.cpu.per_core) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Rates in bits a second", mark = bits,
                        on_choose = function() set_bits(not bits) end }

  local mx = more:in_window()
  local m = win:open_menu(win.origin_x + mx, win.origin_y + L.head, items)

  -- Where the menu is, and how many samples the pace in use has taken in
  -- how long by the counter - which is what `tools/run_sysmon.py` holds
  -- to the pace, since the words above the history could say a second
  -- while the clock kept another.
  if m then
    print(("sysmon: more menu at %d,%d, %d wide, rows of %d; %d samples in %.1f s")
          :format(m.x, m.y, m.w, m.row, S.samples, (sys.ticks() - paced_at) / counter_hz))
  end
end

local header = ui.header{ x = SIDE, y = 0, w = MW, title = pages[showing].title,
                          right = { legend, more }, title_bar = true }

--------------------------------------------------------------------------
-- The page shown, as one view; on the Overview a click on a tile opens its
-- part, on the release over the tile it was pressed on.
--------------------------------------------------------------------------

local body = ui.view{ x = SIDE, y = L.head, w = MW, h = pages[showing].h }
local painted = { ticks = 0, kb = 0 }
local open_part                       -- forward: a tile and the sidebar call it

-- Where everything a suite presses is, in the window: once at the first
-- paint, and the tiles again whenever they are laid out again.
local told_tiles = nil

local function say_where()
  local mx, my = more:in_window()

  print(("sysmon: window at %d,%d; more at %d,%d"):format(
        win.origin_x or 0, win.origin_y or 0, mx + more.w // 2, my + more.h // 2))

  for _, it in ipairs(side.items) do
    if it.id and not it.heading then
      local y, h = side:row_of(it.id)

      print(("sysmon: sidebar %s at %d,%d"):format(it.id, side.x + side.w // 2, side.y + y + h // 2))
    end
  end
end

local function say_tiles()
  if showing ~= "overview" then return end

  local key = {}

  for _, t in ipairs(pages.overview.tiles) do
    key[#key + 1] = ("%s at %d,%d %dx%d"):format(t.id, body.x + t.x, body.y + t.y, t.w, t.h)
  end

  key = table.concat(key, "; ")

  if key ~= told_tiles then
    told_tiles = key
    print("sysmon: tiles " .. key)
  end
end

function body:draw(g)
  if not self.told and more.x then
    say_where()
    self.told = true
  end

  say_tiles()

  -- What a paint costs, by the counter and the collector, for `--tell`.
  local t0, kb = sys.ticks(), collectgarbage("count")

  pages[showing]:draw(g)

  painted.ticks, painted.kb = sys.ticks() - t0, collectgarbage("count") - kb
end

function body:mouse(action, x, y)
  if showing ~= "overview" then return false end

  return ui.click(self, action, x, y, function(px, py)
    local t = pages.overview:tile_at(px, py)

    if t then return t.id, function() open_part(t.opens, "tile " .. t.id) end end

    return nil
  end)
end

--------------------------------------------------------------------------
-- What the window says with `--tell`: every row of the inventory each page
-- shows, and what a sample cost (`sysmonitor.md`, *How it is tested* and
-- *The busiest path*).
--------------------------------------------------------------------------

local function ms(ticks) return ticks * 1000 / counter_hz end

local function tell_rows()
  for _, name in ipairs({ "overview", "cpu" }) do
    for _, r in ipairs(pages[name]:rows()) do
      print(("sysmon: row %d %s: %s = %s"):format(r[1], name, r[2], r[3]))
    end
  end

  print("sysmon: core temperatures " .. pages.cpu:temperatures())

  local slow = {}

  for _, src in ipairs(S.sources) do
    if src.slow then slow[#slow + 1] = src.name end
  end

  print("sysmon: slow to answer " .. (#slow > 0 and table.concat(slow, ", ") or "none"))
end

local function tell_cost()
  local c = S.cost

  if not c then return end

  -- A count that fell is a collection that ran inside the span: said so,
  -- rather than as an allocation below nothing.
  local function kb(n) return n >= 0 and ("%.1f KB"):format(n) or "a collection" end

  print(("sysmon: cost of a sample: asking %.2f ms, differences %.2f ms, painting %.2f ms; "
         .. "%d messages, %d calls; allocated %s sampling, %s painting")
        :format(ms(c.asking), ms(c.taking), ms(painted.ticks), c.messages, c.calls,
                kb(c.kb), kb(painted.kb)))
end

--------------------------------------------------------------------------
-- The sampling, on the window kit's own clock set to the pace: a sample a
-- tick for every page. A tick repaints the window by itself
-- (`window:run`), so nothing here asks for it.
--------------------------------------------------------------------------

local relayout                        -- forward

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

local function same_rows(a, b)
  if #a ~= #b then return false end

  for i = 1, #a do
    if a[i].id ~= b[i].id or a[i].gap ~= b[i].gap then return false end
  end

  return true
end

function sampler:tick()
  S:take()

  -- The sidebar's numbers, and its rows when a part came or went.
  local items = side_items()
  local moved = not same_rows(items, side.items)

  side.items = items

  if pages.overview:stale() then
    pages.overview:arrange()
    moved = true
  end

  if moved then relayout("the parts changed") say_where() end

  if TELL and (S.samples == 3 or S.samples % 10 == 0) then
    tell_rows()
    tell_cost()
  end
end

--------------------------------------------------------------------------
-- The window grown or shrunk to what is shown, and painted at once.
--------------------------------------------------------------------------

function relayout(why)
  local page = pages[showing]

  page:measure()
  body.h = page.h

  local ok, w, h = win:resize(W, L.head + height())   -- as it was opened

  -- The page's own height and the sidebar's beside the window's: the
  -- window is the taller of the two, so on a machine of few cores the
  -- sidebar decides it and the page alone grows (`x86-sysmon`).
  print(("sysmon: %s, %s, its page %d, the sidebar %d"):format(why,
        ok and ("%dx%d"):format(w, h) or "not resized", page.h, side_need()))
  win:paint()
end

-- A page shown: the header's name, the sidebar's choice and the page, all
-- in the same paint as the click that asked for them.
function show(id, why)
  showing = id
  side.selected = id
  header.title = pages[id].title
  told_tiles = nil
  relayout(why or ("page " .. id))

  local first = id == "cpu" and pages.cpu:heading() or ("tiles " .. pages.overview:names())

  print(("sysmon: page %s shown: %s"):format(id, first))
end

--
-- A part opened, from the sidebar or a tile: its page, the page standing
-- in for it, or the window that shows it until its page is here - and
-- then the sidebar's choice stays where it was, since this window did not
-- change page.
--
function open_part(id, from)
  id = tostring(id or "")

  if id:match("^drive:") then id = "disks" end

  local page = pages[id] and id or STANDS_IN[id]

  if page then
    show(page, from)
    return
  end

  side.selected = showing

  local program = ELSEWHERE[id]

  if program then
    fs.send("/Running/wm", { type = "launch", program = program })
    print(("sysmon: %s opens %s, until its page is here"):format(id, program))
  end

  win:paint()
end

side.on_select = function(_, id) open_part(id, "sidebar " .. id) end

function set_bars(on)
  pages.cpu.show_bars = on
  if showing ~= "cpu" then show("cpu") end
  relayout(on and "bars shown" or "bars hidden")
end

function set_per_core(on)
  pages.cpu.per_core = on
  if showing ~= "cpu" then show("cpu") end
  relayout(on and "a graph a core" or "one graph")
end

function set_bits(on)
  bits = on
  print("sysmon: rates in " .. (on and "bits" or "bytes") .. " a second")
  win:paint()
end

--
-- A pace chosen: the window's clock set to it, and every history begun
-- again, since columns taken at one pace drawn beside columns at another
-- would be a time axis that lies. The words change at once; the first
-- column arrives a pace later, from a reading taken now.
--
function set_pace(p)
  pace = p
  win.tick_every = pace_ticks(p)
  win.last_tick = sys.ticks()
  S.pace_ticks = win.tick_every
  S:restart()
  paced_at = win.last_tick

  print(("sysmon: %s, %s"):format(p.menu:lower(), p.heading:lower()))
  win:paint()
end

win:add(side_ground)
win:add(side)
win:add(header)
win:add(body)
win:add(sampler)

print(("sysmon: page %s shown: %s"):format(showing,
      showing == "cpu" and pages.cpu:heading() or ("tiles " .. pages.overview:names())))

-- After the sampler, which is what makes the kit read a clock at all.
win.tick_every = pace_ticks(pace)
win:run()
