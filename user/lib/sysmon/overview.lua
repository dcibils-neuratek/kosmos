-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's Overview: the whole machine at once, a tile for each
-- part this machine has (`docs/sysmonitor.md` S3, `docs/sysmonitor.html`'s
-- first window).
--
--   local page = use("/Kosmos/Libraries/sysmon/overview.lua").new{
--     S = S, w = 680, pace = function() return pace end, bits = function() ... end,
--     processor = cpu_page.processor }
--   page:measure()            -- page.h, and where every tile is
--   page:draw(g)
--   page:tile_at(x, y)        -- the tile under a point, by its id
--
-- **Each tile gives its device's name, the one or two numbers that say how
-- it is doing now, and a small graph of the history window**, as the
-- drawing has them: the processor with a graph a core, heat and power,
-- memory, the GPU, the network, the drives, the LLM server while it runs,
-- and the busiest programs. **Nothing that is absent gets a tile**: under
-- QEMU there is no GPU tile, and the M700 has no battery, which the heat
-- tile says rather than drawing a tile of its own.
--
-- **A measurement this machine does not give says so, in its place**, and
-- never as a nought that would read as one (`sysmonitor.md`, *Sensors*):
-- QEMU's processor keeps no heat and no power, and its heat tile says
-- "not measured on this machine".
--
-- **Nothing here asks anything**: the tiles draw what the window's sample
-- (`sysmon/sample.lua`) last heard, and a tile's graph is the kit's
-- (`ui.history`), one drawing command a graph.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

local UNMEASURED = cpu_page.UNMEASURED
local GROUND = cpu_page.GRAPH_GROUND

-- The drawing's colours for a part, the same in every look as Monitor's
-- red and green are: a temperature, a power, memory, the GPU, the LLM.
local HEAT_COL = 0xffff8a5c
local MEM_COL  = 0xff4fb3ff
local NEBULA_COL = 0xff2f7fc0    -- Nebula's part of memory, under the rest
local GPU_COL  = 0xffb98cff
local LLM_COL  = 0xffd98cff

local PAD, GAP = 12, 12          -- in from a tile's edges, and between two
local PIC = 46                   -- a tile's picture
local SMALL_H = 40               -- a tile's graph
local DRIVE_H = 24               -- a drive's graph, in the drives' tile
local MOST_DRIVES = 3            -- drives drawn in the tile; the rest are counted

local function gb(mb)
  return mb and ("%.1f GB"):format(mb / 1024) or "-"
end

function M.new(spec)
  local S = spec.S
  local W = spec.w
  local pace = spec.pace
  local bits = spec.bits or function() return false end
  local P = spec.processor or cpu_page.processor(S.cores)
  local CORES = S.cores

  local NAME_FACE = ui.sized("title", 17)
  local BIG_FACE = ui.sized("title", 20)
  local LH, NH, UH = gfx.height("label"), gfx.height(NAME_FACE), gfx.height("ui")
  local TOP_H = math.max(PIC, LH + NH + UH)
  local NUMS_H = LH + UH + 8

  local page = { name = "overview", title = "Overview", w = W, tiles = {} }

  local function rate(b) return files.rate(b, bits()) end

  --------------------------------------------------------------------------
  -- The parts of a tile, each drawn the same way in every tile.
  --------------------------------------------------------------------------

  local function card(g, x, y, w, h)
    g:fill_round(x, y, w, h, theme.raised, L.card_r)
    g:frame_round(x, y, w, h, theme.line_soft, L.card_r)
  end

  --
  -- The tile's top: its picture (or none), its kind small and dim above its
  -- name, its facts under them, and at the right its numbers now, each
  -- `{ value, caption, colour }`, right to left as `ui.meter_card`'s rates.
  --
  local function top(g, x, y, w, picture, kind, name, facts, big)
    local right = x + w - PAD
    local at = right

    for i = #(big or {}), 1, -1 do
      local b = big[i]
      local cw = math.max(gfx.measure(b[1], BIG_FACE), gfx.measure(b[2] or "", "ui"))

      g:text(at - gfx.measure(b[1], BIG_FACE), y + PAD, b[1], b[3] or theme.text, theme.raised, BIG_FACE)
      g:text(at - gfx.measure(b[2] or "", "ui"), y + PAD + gfx.height(BIG_FACE), b[2] or "",
             theme.text_dim, theme.raised, "ui")
      at = at - cw - 18
    end

    local tx = x + PAD

    if picture then
      picture(g, x + PAD, y + PAD)
      tx = tx + PIC + 12
    end

    local room = math.max(0, at - tx - 6)
    local ty = y + PAD + (TOP_H - (LH + NH + UH)) // 2

    g:text(tx, ty, ui.fitted(kind:upper(), room, "label"), theme.text_dim, theme.raised, "label")
    g:text(tx, ty + LH, ui.fitted(name or "", room, NAME_FACE), theme.text, theme.raised, NAME_FACE)
    g:text(tx, ty + LH + NH, ui.fitted(facts or "", room, "ui"), theme.text_dim, theme.raised, "ui")
  end

  -- A row of small numbers, each `{ caption, value, colour }`.
  local function nums(g, x, y, w, list)
    local cw = (w - 2 * PAD) // math.max(1, #list)

    for i, n in ipairs(list) do
      local nx = x + PAD + (i - 1) * cw

      g:text(nx, y, ui.fitted(n[1], cw - 8, "label"), theme.text_dim, theme.raised, "label")
      g:text(nx, y + LH, ui.fitted(n[2], cw - 8, "ui"), n[3] or theme.text, theme.raised, "ui")
    end
  end

  local function graph(g, x, y, w, h, list, spec_)
    spec_.columns, spec_.ground, spec_.radius = S.columns, GROUND, 6
    return ui.history.draw(g, x, y, w, h, list, spec_)
  end

  local function picture(kind, label, maker)
    return function(g, x, y) ui.device_picture(g, x, y, PIC, kind, label, maker) end
  end

  --------------------------------------------------------------------------
  -- The tiles, each with its height, its drawing, what a click on it opens,
  -- and the inventory's rows it shows (`sysmonitor.md`, *Every metric
  -- today*), for `--tell`.
  --------------------------------------------------------------------------

  -- The processor: the chip, the model, busy, the clock now, the package's
  -- °C and W, and a small graph a core - or one past sixteen.
  local function cpu_tile()
    local one = CORES > 16
    local across = one and 1 or math.min(8, CORES)
    local down = one and 1 or (CORES + across - 1) // across
    local h = PAD + TOP_H + 10 + down * (UH + 2 + SMALL_H) + (down - 1) * 6 + PAD

    local function heat_now()
      local heat = S.hot.heat

      if not heat or not heat.temp_c then return UNMEASURED, nil end

      return ("%d °C"):format(heat.temp_c), heat.watts and ("%.1f W"):format(heat.watts) or nil
    end

    return {
      id = "cpu", h = h, full = true, opens = "cpu",
      draw = function(g, x, y, w)
        local ghz = S:speed()
        local hot, watts = heat_now()
        local big = { { ("%d%%"):format(S:busy()), "busy", cpu_page.USER_COL } }

        if ghz then big[#big + 1] = { ("%.2f GHz"):format(ghz), "now" } end
        if hot ~= UNMEASURED then big[#big + 1] = { hot, watts or "the package" } end

        -- Switches a second after the facts, from who ran (row 97).
        local switches = S.who_ran.switches
        local facts = switches and ("%s \u{b7} %d switches a second"):format(P.facts, switches) or P.facts

        top(g, x, y, w, function(gg, px, py)
          ui.processor_chip(gg, px + 3, py + 3, PIC - 6, P.lid, nil)
        end, "CPU", P.model, facts, big)

        local gy = y + PAD + TOP_H + 10
        local inner = w - 2 * PAD
        local gw = (inner - (across - 1) * 6) // across

        for c = 1, (one and 1 or CORES) do
          local col, row = (c - 1) % across, (c - 1) // across
          local gx = x + PAD + col * (gw + 6)
          local yy = gy + row * (UH + 2 + SMALL_H + 6)
          local words = one and ("Every core \u{b7} %d%%"):format(S:busy())
                        or ("Core %d \u{b7} %d%%"):format(c, S.cpu.user[c] + S.cpu.kernel[c])

          g:text(gx + 1, yy, ui.fitted(words, gw - 2, "ui"), theme.text_dim, theme.raised, "ui")
          graph(g, gx, yy + UH + 2, gw, SMALL_H, one and S.all or S.core[c],
                { mode = "stacked", colours = { cpu_page.KERNEL_COL, cpu_page.USER_COL } })
        end
      end,
      rows = function()
        local busy = {}

        for c = 1, CORES do busy[#busy + 1] = ("%d:%d%%"):format(c, S.cpu.user[c] + S.cpu.kernel[c]) end

        local heat = S.hot.heat
        local ghz = S:speed()

        return {
          { 1, "The chip", ("%s | %s"):format(P.lid, P.part) },
          { 2, "The model", P.model ~= "" and P.model or "-" },
          { 6, "How fast it runs now", ghz and ("%.2f GHz"):format(ghz) or UNMEASURED },
          { 7, "The package's temperature", heat and heat.temp_c and ("%d °C"):format(heat.temp_c) or UNMEASURED },
          { 8, "The package's power", heat and heat.watts and ("%.1f W"):format(heat.watts) or UNMEASURED },
          { 10, "Each core busy", table.concat(busy, " ") },
          { 13, "A graph a core", ("%d small %s"):format(one and 1 or CORES, (one or CORES == 1) and "graph" or "graphs") },
          { 97, "Switches a second", S.who_ran.switches and tostring(S.who_ran.switches) or "not counted yet" },
          { 100, "How busy the machine is", ("%d%%"):format(S:busy()) },
        }
      end,
    }
  end

  -- Heat and power: the package's, the GPU's, the hottest core's and the
  -- battery's, in one line, and the package's temperature over the window.
  local function heat_tile()
    return {
      id = "heat", h = PAD + TOP_H + 8 + NUMS_H + SMALL_H + PAD, opens = "heat",
      draw = function(g, x, y, w)
        local heat = S.hot.heat
        local name, facts = UNMEASURED, "QEMU's processor and others keep none"

        if heat and heat.temp_c then
          name = ("%d °C"):format(heat.temp_c) .. (heat.watts and (" \u{b7} %.1f W"):format(heat.watts) or "")
          facts = "the package"
                  .. ((heat.limit_c or heat.limit_w) and (", of %s%s"):format(
                        heat.limit_c and ("%d °C"):format(heat.limit_c) or "",
                        heat.limit_w and (" and %.0f W"):format(heat.limit_w) or "") or "")
        end

        if S:slow("cpu") then facts = "slow to answer" end

        top(g, x, y, w, nil, "Heat and power", name, facts, nil)

        local hottest = S:hottest()
        local b = S.battery
        local gpu_w = S.gpu_lib and S.gpu.watts

        nums(g, x, y + PAD + TOP_H + 8, w, {
          { "CPU package", heat and heat.watts and ("%.1f W"):format(heat.watts) or "not measured" },
          { "GPU", gpu_w and ("%.1f W"):format(gpu_w) or "not measured" },
          { "Hottest core", hottest and ("%d °C"):format(hottest) or "not measured" },
          { "Battery", b and (("%d%%"):format(b.percent) .. (b.state == "charging" and ", charging" or ""))
                        or "none" },
        })

        graph(g, x + PAD, y + PAD + TOP_H + 8 + NUMS_H, w - 2 * PAD, SMALL_H, S.heat_h,
              { mode = "line", most = heat and heat.limit_c or 100, colours = { HEAT_COL } })
      end,
      rows = function()
        local heat, b = S.hot.heat, S.battery

        return {
          { 7, "The package's temperature", heat and heat.temp_c and ("%d °C"):format(heat.temp_c) or UNMEASURED },
          { 8, "The package's power", heat and heat.watts and ("%.1f W"):format(heat.watts) or UNMEASURED },
          { 54, "The GPU's power", S.gpu_lib and S.gpu.watts and ("%.1f W"):format(S.gpu.watts) or UNMEASURED },
          { 102, "The battery", b and ("%d%% %s"):format(b.percent, tostring(b.state or "")) or "none" },
        }
      end,
    }
  end

  -- Memory: used of total, and the kernel's part - Nebula's at the floor of
  -- its graph and the rest over it. A click opens the Memory page (S6).
  local function memory_tile()
    return {
      id = "memory", h = PAD + TOP_H + 10 + SMALL_H + PAD, opens = "memory",
      draw = function(g, x, y, w)
        local t, k = S.totals, S.kernel or {}
        local facts = k.memory_kb and ("Nebula %s"):format(files.size(k.memory_kb * 1024)) or ""

        if S:slow("machine") then facts = "slow to answer" end

        top(g, x, y, w, picture("memory"), "Memory",
            t.used_mb and ("%s of %s"):format(gb(t.used_mb), gb(t.total_mb)) or "-", facts, nil)
        graph(g, x + PAD, y + PAD + TOP_H + 10, w - 2 * PAD, SMALL_H, S.memory_h,
              { mode = "stacked", colours = { NEBULA_COL, MEM_COL } })
      end,
      rows = function()
        local t = S.totals

        return {
          { 21, "Memory, used of total", t.used_mb and ("%d of %d MB"):format(t.used_mb, t.total_mb) or "-" },
          { 101, "How much memory is spoken for", t.used_mb and ("%d%%"):format(t.used_mb * 100 // math.max(1, t.total_mb)) or "-" },
        }
      end,
    }
  end

  -- The GPU, where its driver knows one.
  local function gpu_tile()
    return {
      id = "gpu", h = PAD + TOP_H + 10 + SMALL_H + PAD, opens = "gpu",
      draw = function(g, x, y, w)
        local s, r = S.gpu.now or {}, S.gpu.rate
        local parts = { s.mhz and ("%d MHz of %d"):format(s.mhz, s.max_mhz or 0) or nil }

        if r and r.asleep then parts[#parts + 1] = ("asleep %d%%"):format(math.floor(r.asleep * 100 + 0.5)) end
        if S.gpu.watts then parts[#parts + 1] = ("%.1f W"):format(S.gpu.watts) end

        local facts = S:slow("gpu") and "slow to answer" or table.concat(parts, " \u{b7} ")

        top(g, x, y, w, picture("gpu", nil, "intel"), "GPU", S.gpu_model or s.model, facts,
            { { ("%d%%"):format(math.floor((S.gpu.busy or 0) * 100 + 0.5)), "busy", GPU_COL } })
        graph(g, x + PAD, y + PAD + TOP_H + 10, w - 2 * PAD, SMALL_H, S.gpu_h,
              { mode = "stacked", colours = { GPU_COL } })
      end,
      rows = function()
        local s = S.gpu.now or {}

        return {
          { 42, "The card", tostring(S.gpu_model or s.model) },
          { 43, "The card's rates", s.mhz and ("%d MHz"):format(s.mhz) or "-" },
          { 45, "Busy", ("%d%%"):format(math.floor((S.gpu.busy or 0) * 100 + 0.5)) },
        }
      end,
    }
  end

  -- The network: its card, in and out now, and the minute mirrored.
  local function network_tile()
    return {
      id = "network", h = PAD + TOP_H + 8 + NUMS_H + SMALL_H + PAD, opens = "network",
      draw = function(g, x, y, w)
        local info = S.net_info or {}
        local facts = info.card and info.address and ("MTU %d"):format(info.mtu or 0) or "not on a network"

        if S:slow("network") then facts = "slow to answer" end

        top(g, x, y, w, picture("port"), "Network", S.card, facts, nil)
        nums(g, x, y + PAD + TOP_H + 8, w, {
          { "In", rate(S.net.rin), theme.io_in },
          { "Out", rate(S.net.rout), theme.io_out },
        })
        graph(g, x + PAD, y + PAD + TOP_H + 8 + NUMS_H, w - 2 * PAD, SMALL_H, S.net_h,
              { mode = "mirrored", colours = { theme.io_in, theme.io_out } })
      end,
      rows = function()
        return {
          { 75, "The card", tostring(S.card) },
          { 77, "In now, out now", rate(S.net.rin) .. " | " .. rate(S.net.rout) },
          { 78, "The minute, in over out", ("%d columns"):format(#S.net_h) },
        }
      end,
    }
  end

  -- The drives: each one's name, reading and writing now, and its minute.
  local function disks_tile()
    local n = math.min(MOST_DRIVES, #S.drives)

    return {
      id = "disks", h = PAD + TOP_H + 6 + n * (UH + 2 + DRIVE_H + 6) + PAD - 6, opens = "disks",
      draw = function(g, x, y, w)
        local more = #S.drives - n

        top(g, x, y, w, picture("ssd"), "Disks",
            ("%d %s"):format(#S.drives, #S.drives == 1 and "drive" or "drives"),
            "reading \u{b7} writing, now" .. (more > 0 and (", and %d more"):format(more) or ""), nil)

        local yy = y + PAD + TOP_H + 6

        for i = 1, n do
          local d = S.drives[i]
          local m = S.drive[d.key]
          local words = (m and m.silent) and "says nothing of its reading and writing"
                        or S:slow(d.key) and "slow to answer"
                        or (m and (rate(m.rin) .. " \u{b7} " .. rate(m.rout)) or "")

          g:text(x + PAD + 1, yy, ui.fitted(d.name, (w - 2 * PAD) // 2, "label"),
                 theme.text, theme.raised, "label")
          g:text(x + w - PAD - gfx.measure(words, "ui"), yy, words, theme.text_dim, theme.raised, "ui")
          graph(g, x + PAD, yy + UH + 2, w - 2 * PAD, DRIVE_H, m and m.history or {},
                { mode = "mirrored", colours = { theme.io_in, theme.io_out } })
          yy = yy + UH + 2 + DRIVE_H + 6
        end
      end,
      rows = function()
        local names, rates = {}, {}

        for _, d in ipairs(S.drives) do
          local m = S.drive[d.key]

          names[#names + 1] = d.name .. " " .. d.path
          rates[#rates + 1] = d.path .. " " .. (m and (rate(m.rin) .. "/" .. rate(m.rout)) or "-")
        end

        return {
          { 61, "The drives", table.concat(names, ", ") },
          { 64, "Each drive's reading and writing", table.concat(rates, ", ") },
        }
      end,
    }
  end

  -- The LLM server, while it runs: the model, and tokens a second.
  local function llm_tile()
    return {
      id = "llm", h = PAD + TOP_H + 10 + SMALL_H + PAD, opens = "llm",
      draw = function(g, x, y, w)
        local s = S.llm_now or {}
        local model = (s.model or "") ~= "" and s.model:gsub("%.gguf$", "") or "Nothing loaded"
        local last = S.llm_last and ("%.1f tokens/s last"):format(S.llm_last) or "nothing written yet"

        top(g, x, y, w, picture(nil, "LLM"), "LLM", model, tostring(s.state or ""),
            { { tostring(s.state == "answering" and "writing" or "idle"), last } })
        graph(g, x + PAD, y + PAD + TOP_H + 10, w - 2 * PAD, SMALL_H, S.llm_h,
              { mode = "line", most = math.max(1, ui.history.most(S.llm_h)), colours = { LLM_COL } })
      end,
      rows = function()
        local s = S.llm_now or {}

        return {
          { 87, "Loaded", (s.model or "") ~= "" and s.model or tostring(s.state or "-") },
          { 88, "Writing", S.llm_last and ("%.1f tokens a second"):format(S.llm_last) or "nothing written yet" },
        }
      end,
    }
  end

  -- The busiest five, by processor, each with its share and memory.
  local function busiest_tile()
    local ROW = UH + 6

    return {
      id = "busiest", h = PAD + LH + 8 + 5 * ROW + PAD, opens = "processes",
      draw = function(g, x, y, w)
        local all = ("All %d \u{203a}"):format(S.alive)

        g:text(x + PAD, y + PAD, "THE BUSIEST PROGRAMS", theme.text_dim, theme.raised, "label")
        g:text(x + w - PAD - gfx.measure(all, "ui"), y + PAD, all, theme.accent, theme.raised, "ui")

        local yy = y + PAD + LH + 8
        local mem_w = gfx.measure("000.0 MB", "ui")
        local bar_w = 54

        for _, r in ipairs(S.busiest) do
          local bx = x + w - PAD - mem_w - 12 - gfx.measure("100%", "ui") - 8 - bar_w
          local pct = ("%d%%"):format(r.pct)
          local size = files.size(r.bytes)

          g:text(x + PAD, yy + 3, ui.fitted(r.name, bx - x - PAD - 8, "ui"), theme.text, theme.raised, "ui")
          g:fill_round(bx, yy + (ROW - 6) // 2, bar_w, 6, theme.track, 3)
          if r.pct > 0 then
            g:fill_round(bx, yy + (ROW - 6) // 2, math.max(6, bar_w * r.pct // 100), 6, cpu_page.USER_BAR, 3)
          end
          g:text(bx + bar_w + 8, yy + 3, pct, theme.text_dim, theme.raised, "ui")
          g:text(x + w - PAD - gfx.measure(size, "ui"), yy + 3, size, theme.text_dim, theme.raised, "ui")
          yy = yy + ROW
        end
      end,
      rows = function()
        local out = {}

        for _, r in ipairs(S.busiest) do out[#out + 1] = ("%s %d%%"):format(r.name, r.pct) end

        return { { 35, "The busiest by processor", #out > 0 and table.concat(out, ", ") or "-" } }
      end,
    }
  end

  --------------------------------------------------------------------------
  -- Where the tiles go: the processor across the top, and the rest in two
  -- columns, each tile in the shorter, so the page is as short as it can be.
  --------------------------------------------------------------------------

  function page:arrange()
    local list = { cpu_tile(), heat_tile(), memory_tile() }

    if S.gpu_lib then list[#list + 1] = gpu_tile() end
    if S.card then list[#list + 1] = network_tile() end
    if #S.drives > 0 then list[#list + 1] = disks_tile() end
    if S.llm_now then list[#list + 1] = llm_tile() end

    list[#list + 1] = busiest_tile()
    self.tiles = list
    return self:measure()
  end

  function page:measure()
    local inner = self.w - 2 * L.page_side
    local cw = (inner - GAP) // 2
    local y = L.page_top
    local col = { y, y }

    for _, t in ipairs(self.tiles) do
      if t.full then
        local at = math.max(col[1], col[2])

        t.x, t.y, t.w = L.page_side, at, inner
        col[1], col[2] = at + t.h + GAP, at + t.h + GAP
      else
        local i = (col[2] < col[1]) and 2 or 1

        t.x, t.y, t.w = L.page_side + (i - 1) * (cw + GAP), col[i], cw
        col[i] = col[i] + t.h + GAP
      end
    end

    self.foot = math.max(col[1], col[2]) + 10 - GAP
    self.h = self.foot + gfx.height() + L.page_foot
    return self.h
  end

  -- Which tiles there are, in their order, for `--tell` and its suite.
  function page:names()
    local out = {}

    for _, t in ipairs(self.tiles) do out[#out + 1] = t.id end

    return table.concat(out, ", ")
  end

  -- A part present now that was not, or gone: the tiles made again.
  function page:stale()
    local want = { cpu = true, heat = true, memory = true, busiest = true,
                   gpu = S.gpu_lib ~= nil, network = S.card ~= nil,
                   disks = #S.drives > 0, llm = S.llm_now ~= nil }
    local have = {}

    for _, t in ipairs(self.tiles) do have[t.id] = true end

    for id, on in pairs(want) do
      if (have[id] or false) ~= on then return true end
    end

    -- And the drives' tile when how many it draws has changed.
    for _, t in ipairs(self.tiles) do
      if t.id == "disks" and t.h ~= disks_tile().h then return true end
    end

    return false
  end

  function page:tile_at(x, y)
    for _, t in ipairs(self.tiles) do
      if x >= t.x and x < t.x + t.w and y >= t.y and y < t.y + t.h then return t end
    end

    return nil
  end

  function page:draw(g)
    for _, t in ipairs(self.tiles) do
      card(g, t.x, t.y, t.w, t.h)
      t.draw(g, t.x, t.y, t.w)
    end

    g:text(L.page_side + 3, self.foot, pace().foot, theme.text_dim, theme.window, "ui")
  end

  -- Every row the tiles show, for `--tell`: a row two tiles show (the
  -- package's heat, on the processor's and on heat and power's) once.
  function page:rows()
    local out, seen = {}, {}

    for _, t in ipairs(self.tiles) do
      for _, r in ipairs(t.rows()) do
        if not seen[r[1]] then
          seen[r[1]] = true
          out[#out + 1] = r
        end
      end
    end

    return out
  end

  page:arrange()
  return page
end

return M
