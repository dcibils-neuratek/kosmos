-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's Processes page: Process Viewer's table, its band and
-- End, and the chosen process's minute in a strip under the table
-- (`docs/sysmonitor.md` S4; inventory rows 19-41).
--
--   local page = use("/Kosmos/Libraries/sysmon/processes.lua").new{
--     S = S, pace = function() return pace end, cpu = fs.read("/Devices/cpu"),
--     window = function() return win end }
--   page.view                 -- the page, a view the window shows
--   page:tick(shown)          -- on the window's clock, after `S:take()`
--   page:end_selected()       -- End, from the header
--   page:summary()            -- the header's line
--   page:rows()               -- the inventory's rows 19-41, for `--tell`
--
-- **The table is Process Viewer's own** - `proctable.lua`, which both
-- draw through: every column, sorting by any heading either way with ties
-- by id and empty cells last, columns shown, hidden, dragged, fitted and
-- kept, the selection following its process, End. What is here is the
-- page around it: the four pools in a band, the strip, and where the rows
-- come from. Process Viewer goes in S9 and `procs` opens this page then
-- (decision 3).
--
-- **Added: the chosen process in a strip under the table** - its share of
-- the machine and its memory over the minute. The minute is the sample's
-- (`S:follow`): kept for every process from the moment it is seen, so a
-- process chosen now shows the minute before it was chosen. **And since
-- S5 what it holds elsewhere**, under the two graphs: the drives it reads
-- and writes (each drive's `.programs`, asked of every drive while this
-- page shows), its connections (`NET_OP_STATS`'s, by the process the
-- stack named) and its GPU buffers (`GPU_OP_STATS`'s holders) - four lists
-- in four windows until now, joined here by the process's id. Its LLM
-- answers join them in S8.
--
-- **Nothing here asks anything in a repaint.** The rows are built on the
-- window's clock from what the sample heard - `sys.processes()`,
-- `/Devices/kernel` and `/Devices/memory` always, the window manager's
-- windows while this page shows (`S:want`) - and what each file declares
-- is read once, on the first tick. A heading sorts the rows already here.
--
-- **End is the one thing this page does**, and `needs processes` is what
-- lets it reach every process (decision 7).

local ui = use("/Kosmos/Libraries/ui.lua")
local proctable = use("/Kosmos/Libraries/proctable.lua")
local files = use("/Kosmos/Libraries/files.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

-- What the page is as wide as: the table Process Viewer's 1080, and the
-- page's margins either side - at the other pages' 680 its eleven columns
-- would scroll sideways a column at a time.
M.WIDTH = 1080 + 2 * 26

local BAND_H, BAND_GAP = 35, 14         -- a pool's line, 6, and its bar's 8
local VISIBLE = 12                      -- the table's rows, under its headings
local STRIP_GRAPH = 56                  -- the strip's two graphs
local SHARE_COL = cpu_page.USER_COL
local MEMORY_COL = 0xff4fb3ff           -- the Overview's memory, the same in every look

local memory_of = proctable.memory

--
-- **What the chosen process holds elsewhere**, from what the sample last
-- heard: the drives whose programs list it, its connections and its GPU
-- buffers, each a list of words and a total. Nothing asked here.
--
function M.holdings(S, id)
  local drives, conns, bufs = {}, {}, {}
  local cin, cout, bytes = 0, 0, 0

  for _, d in ipairs(S.drives or {}) do
    local p = S.programs and S.programs[d.key]

    for _, r in ipairs(p and p.list or {}) do
      if r.process == id then
        drives[#drives + 1] = ("%s %s/%s"):format(d.path, files.rate(r.rin), files.rate(r.rout))
      end
    end
  end

  for _, c in ipairs(S.conns or {}) do
    if c.process == id then
      conns[#conns + 1] = c.to
      cin, cout = cin + c.rin, cout + c.rout
    end
  end

  local now = S.gpu_lib and S.gpu.now

  for _, b in ipairs(now and now.buffers or {}) do
    if b.holder == id then
      bufs[#bufs + 1] = b
      bytes = bytes + b.bytes
    end
  end

  return {
    drives = #drives > 0 and table.concat(drives, ", ") or "none",
    connections = #conns > 0 and ("%d, %s in, %s out (%s)"):format(#conns, files.rate(cin), files.rate(cout),
                                                                  table.concat(conns, ", ")) or "none",
    buffers = #bufs > 0 and ("%d, %s"):format(#bufs, files.size(bytes))
              or (S.gpu_lib and "none" or "no GPU here"),
  }
end

function M.new(spec)
  local S = spec.S
  local pace = spec.pace
  local W = spec.w or M.WIDTH
  local ROW = ui.metrics.row
  local privilege, KERNEL_LEVEL = proctable.privilege(spec.cpu)
  local BANDS = (sys.scheduler() or {}).band_names or {}

  local page = { name = "processes", title = "Processes", w = W,
                 built = nil, said = nil, said_at = 0, ended = nil }

  -- What each file declares, read once on the first tick: nil until then.
  local declared = nil

  local view = ui.view{ x = 0, y = 0, w = W, h = 100 }

  --
  -- **The table**: the shared one, framed on a page of cards, its
  -- arrangement kept where Process Viewer keeps it so the one table has
  -- one arrangement whichever window shows it. The strip follows what it
  -- chooses, and says so when the choice is said.
  --
  local list = proctable.new{
    prefix = "sysmon: processes", x = L.page_side, y = 0, w = W - 2 * L.page_side,
    h = 1 + ROW * (VISIBLE + 1), window = spec.window, cpu = spec.cpu, prefs = "procs",
    framed = true,
    on_choose = function(id, said)
      S:follow(id)
      if said then print("sysmon: strip " .. page:strip_words()) end
    end,
  }

  page.view, page.table = view, list
  view:add(list.view)

  -- The rows, from what the sample heard: built a sample at a time, never
  -- in a paint. The sample's list is a new table each time it is read.
  function page:refresh()
    if self.built == S.proc_rows then return end

    self.built = S.proc_rows

    list:set(proctable.rows(S.proc_rows, { kernel = S.kernel, video = S.video, declared = declared,
                                           bands = BANDS, kernel_level = KERNEL_LEVEL,
                                           kernel_threads = S.procs.kernel_threads }))

    -- An End seen through: the process gone from the list, or ended in it.
    local e = self.ended

    if e and not e.gone then
      local alive = false

      for _, r in ipairs(list.list) do
        if r.id == e.id and not r.exited then alive = true end
      end

      if not alive then
        e.gone = true
        print(("sysmon: process %d gone"):format(e.id))
      end
    end
  end

  function page:tick(shown)
    if not declared then declared = proctable.declared() end
    if shown then self:refresh() end
  end

  -- The header's line: what the machine holds, or what the last End did,
  -- for four seconds (rows 19 and 20); `plain` for the first alone.
  function page:summary(plain)
    local hz = S.hz

    if self.said and sys.ticks() - self.said_at > 4 * hz then self.said = nil end

    if self.said and not plain then return self.said end

    return proctable.summary(S.procs.count, S.procs.threads, S.procs.kernel_threads,
                             S.totals.spaces, sys.ticks() // hz)
  end

  -- End: the chosen process, never Nebula's row.
  function page:end_selected()
    local r = list:chosen()

    if not r or r.synthetic then return end

    local text, done = proctable.finish(r)

    if done then self.ended = { id = r.id, name = r.name } end

    self.said, self.said_at = text, sys.ticks()
    print("sysmon: processes: " .. text)
  end

  --------------------------------------------------------------------------
  -- Where everything goes.
  --------------------------------------------------------------------------

  function page:measure()
    local m = {}
    local y = L.page_top

    m.band = y
    y = y + BAND_H + BAND_GAP

    m.table = y
    list.view.y = y
    y = y + list.view.h + BAND_GAP

    m.strip = y
    m.strip_h = 12 + gfx.height("label") + gfx.height("ui") + 8 + STRIP_GRAPH + 10
                + gfx.height("label") + gfx.height("ui") + 12
    y = y + m.strip_h

    m.foot = y + 10
    self.h = m.foot + gfx.height() + L.page_foot
    view.w, view.h = self.w, self.h
    list.view.w = self.w - 2 * L.page_side
    self.m = m
    return m
  end

  function page:chosen()
    return list:chosen()
  end

  local function draw_strip(g, x, y, w, h)
    local r = list:chosen()
    local strip = S.strip
    local lh, uh = gfx.height("label"), gfx.height("ui")

    g:fill_round(x, y, w, h, theme.raised, L.card_r)
    g:frame_round(x, y, w, h, theme.line_soft, L.card_r)

    if not r then
      g:text(x + 14, y + 12, "Choose a process to see its minute", theme.text_dim, theme.raised, "ui")
      return
    end

    local what = ("%s \u{b7} %d \u{b7} %s"):format(r.name, r.id, r.kind or "")
    local now = r.exited and "gone" or ("%d%% \u{b7} %s"):format(r.pct or 0, memory_of(r))

    g:text(x + 14, y + 12, pace().heading:upper(), theme.text_dim, theme.raised, "label")
    g:text(x + 14, y + 12 + lh, ui.fitted(what, w // 2, "ui"), theme.text, theme.raised, "ui")
    g:text(x + w - 14 - gfx.measure(now, "ui"), y + 12 + lh, now, theme.text_dim, theme.raised, "ui")

    local gy = y + 12 + lh + uh + 8
    local gw = (w - 28 - 12) // 2

    ui.history.draw(g, x + 14, gy, gw, STRIP_GRAPH, strip.share,
                    { mode = "stacked", columns = S.columns, ground = cpu_page.GRAPH_GROUND,
                      colours = { SHARE_COL }, radius = 6 })
    ui.history.draw(g, x + 14 + gw + 12, gy, gw, STRIP_GRAPH, strip.memory,
                    { mode = "line", columns = S.columns, ground = cpu_page.GRAPH_GROUND,
                      most = math.max(1, ui.history.most(strip.memory)),
                      colours = { MEMORY_COL }, radius = 6 })

    -- What it holds elsewhere: a column each, its name small above it.
    local held = M.holdings(S, r.id)
    local hy = gy + STRIP_GRAPH + 10
    local cw = (w - 28 - 2 * 12) // 3

    for i, part in ipairs({ { "DRIVES", held.drives }, { "CONNECTIONS", held.connections },
                            { "GPU BUFFERS", held.buffers } }) do
      local hx = x + 14 + (i - 1) * (cw + 12)

      g:text(hx, hy, part[1], theme.text_dim, theme.raised, "label")
      g:text(hx, hy + lh, ui.fitted(part[2], cw, "ui"), theme.text, theme.raised, "ui")
    end
  end

  function view:draw(g)
    local m = page.m or page:measure()
    local x = L.page_side
    local inner = page.w - 2 * L.page_side
    local t = S.totals
    local gap = 14
    local each = (inner - gap * 3) // 4
    local step = each + gap

    -- The four pools, as Process Viewer's band has them (rows 21-24).
    proctable.level(g, x, m.band, each, BAND_H, "Memory", t.used_mb, t.total_mb,
                    t.used_mb and ("%d of %d MB"):format(t.used_mb, t.total_mb) or "-")
    proctable.level(g, x + step, m.band, each, BAND_H, "Threads", t.threads, t.threads_max)
    proctable.level(g, x + 2 * step, m.band, each, BAND_H, "Processes", t.processes, t.processes_max)
    proctable.level(g, x + 3 * step, m.band, each, BAND_H, "Endpoints", t.endpoints, t.endpoints_max)

    draw_strip(g, x, m.strip, inner, m.strip_h)
    g:text(x + 3, m.foot, pace().foot, theme.text_dim, theme.window, "ui")
  end

  --------------------------------------------------------------------------
  -- What this page shows, a row of `docs/sysmonitor.md`'s inventory each,
  -- for `--tell` (rows 19-41).
  --------------------------------------------------------------------------

  -- The strip in words: whom it follows and how much of the minute it has.
  function page:strip_words()
    local r, strip = list:chosen(), S.strip
    local share, mem = strip.share or {}, strip.memory or {}

    if not r then return "nothing chosen" end

    local held = M.holdings(S, r.id)

    return ("%d %s: share %d columns, now %d%%; memory %d columns, now %s; drives %s; connections %s; "
            .. "buffers %s"):format(
           r.id, r.name, #share, share[#share] and share[#share][1] or 0,
           #mem, memory_of(r), held.drives, held.connections, held.buffers)
  end

  function page:rows()
    self:refresh()

    local t = S.totals
    local r = list:chosen() or {}
    local COL = list.column
    local nebula = nil

    for _, row in ipairs(list.list) do
      if row.synthetic then nebula = row end
    end

    local function column(n, key)
      return { n, "Column " .. key,
               ("%s; %s"):format(COL[key].shown and "shown" or "hidden", r.id and list.text_of(r, key) or "-") }
    end

    local function pool(n, name)
      return { n, name, ("%s of %s"):format(tostring(t[name] or "-"), tostring(t[name .. "_max"] or "-")) }
    end

    return {
      { 19, "The header's line", self:summary(true) },
      { 20, "What the last End did", self.said or (self.ended and ("ended " .. self.ended.name))
                                   or "nothing ended yet" },
      { 21, "Memory, used of total", t.used_mb and ("%d of %d MB"):format(t.used_mb, t.total_mb) or "-" },
      pool(22, "threads"), pool(23, "processes"), pool(24, "endpoints"),
      column(25, "id"), column(26, "name"), column(27, "file"), column(28, "kind"),
      column(29, "privilege"), column(30, "draws"), column(31, "priority"), column(32, "core"),
      column(33, "threads"), column(34, "memory"), column(35, "processor"),
      { 36, "Nebula's own row", nebula and ("holds %s, %d threads, %s"):format(
              memory_of(nebula), nebula.threads or 0, privilege(nebula.level)) or "-" },
      { 37, "Sorted by", ("%s, %s, ties by id, empty cells last"):format(
              list.sort_key, list.sort_down and "descending" or "ascending") },
      { 38, "The columns", list.columns:text() },
      { 39, "The selection", r.id and ("%d %s, row %d of %d"):format(r.id, r.name, list.selected,
              #list.list) or "none" },
      { 40, "End", "in the header, any process (needs processes)" },
      { 41, "Refresh", "in the dots' menu; the wheel, three rows a notch" },
    }
  end

  page:measure()
  return page
end

return M
