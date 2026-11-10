-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name LLM Monitor
-- kosmos: section system
-- **What the LLM server is doing** (`docs/xavier.md`, *The LLM Monitor*;
-- drawn in `docs/xavier.html`, *What the engine is doing*): which model is
-- in and where it runs, how fast it writes and reads and how long a person
-- waits for the first word, five minutes of writing as a graph, and every
-- application that has asked, by name, with what it took.
--
--   wm llmmon
--
-- **And Inside** (`xavier.md`, *The LLM Monitor: Inside*; drawn in
-- `docs/llmviz.html`): the model at work, a token at a time - each layer's
-- heads and where they look, its feed-forward neurons that fired, the
-- residual stream gathering, and the candidates for what comes next -
-- from the ring of passages the server fills while this page is open.
--
--   wm llmmon:--inside        opened on Inside
--
-- **And Story** (`xavier.md`, *The Story page*; drawn in
-- `docs/llmstory.html`): one token's passage told as a tower, a row a
-- layer - the words it looked back at, how much of it fired, and what the
-- model would have said had it stopped there (the logit lens), with the
-- layer the answer arrives at marked. The guesses are asked for once a
-- token is chosen, after the fact, so watching never slows an answer.
--
--   wm llmmon:--story         opened on Story
--   wm llmmon:--inside --story
--                             on Inside, then Story a sample later: both
--                             drawn, for a suite that cannot click
--
-- **It measures nothing itself**, as the other monitors do not: the server
-- counts (`llm.stats`, `struct llm_reply`), and this asks every two seconds
-- and takes the difference between two readings. A name is the kernel's for
-- the process that asked, never what a message said of itself.

local ui = use("/Kosmos/Libraries/ui.lua")
local llm = use("/Kosmos/Libraries/llm.lua")
local filetypes = use("/Kosmos/Libraries/filetypes.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- `--tell`: a line a sample, for a suite to read.
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil
local OPEN_INSIDE = (tostring(args or "")):find("%-%-inside") ~= nil
local OPEN_STORY = (tostring(args or "")):find("%-%-story") ~= nil
local keys = use("/Kosmos/Libraries/keys.lua")

local W        = 1140
local TABS_H   = 32
local FAST_HZ  = 10                     -- Inside reads the ring ten times a second
local TILE_H   = 92
local GRAPH_H  = 130
local ROW_H    = 26
local ROWS     = 6
local PACE_S   = 2
local COLUMNS  = 150                    -- five minutes, a column every two seconds
local GRAPH_GROUND = 0xff16181d         -- the monitors': dark in every look
local LLM_COLOUR   = 0xff8b7cf6         -- Xavier's violet, as the mockup has it
local FAINT        = 0xff8b949e

--------------------------------------------------------------------------
-- What the window shows, refreshed a sample at a time.
--------------------------------------------------------------------------

local now, last, why_not = nil, nil, nil
local rates = {}                        -- tokens written a second, a column each
local firsts = {}                       -- { at, first_us } of each answer seen to end
local writes = {}                       -- { at, tokens a second } of each answer seen to end
local seen_answers = nil
local best = 0

local function seconds() return sys.ticks() / counter_hz end

-- The last minute of a list of { at, value }: the list kept to it.
local function last_minute(list)
  local cut = seconds() - 60

  while #list > 0 and list[1][1] < cut do table.remove(list, 1) end

  return list
end

local function sample()
  local s, why = llm.stats()

  if not s then
    now, why_not = nil, why
    return
  end

  if last and s.up_us > last.up_us then
    local dt = (s.up_us - last.up_us) / 1e6
    -- A server started again counts from nought: no column below zero.
    local wrote = s.written_tokens >= last.written_tokens and s.written_tokens - last.written_tokens or 0

    ui.history.push(rates, { wrote / dt }, COLUMNS)
  end

  -- An answer that ended since the last look: its speed, and its first word.
  if seen_answers and s.answers > seen_answers and s.answer_us > 0 then
    local r = s.answer_tokens * 1e6 / s.answer_us

    writes[#writes + 1] = { seconds(), r }
    firsts[#firsts + 1] = { seconds(), s.first_us / 1e6 }
    if r > best then best = r end
  end

  seen_answers = s.answers
  now, last, why_not = s, s, nil

  if TELL then
    local who = {}

    for _, a in ipairs(s.askers) do
      who[#who + 1] = ("%s %d/%d/%d/%d"):format(a.name, a.answers, a.refused, a.read_tokens,
                                              a.written_tokens)
    end

    print(("llmmon: up %d s, %s, model %s, read %d, written %d, answers %d, refused %d; %s")
          :format(s.up_us // 1000000, s.state, s.model ~= "" and s.model or "-", s.read_tokens,
                  s.written_tokens, s.answers, s.refused, #who > 0 and table.concat(who, ", ") or "nobody"))
  end
end

--------------------------------------------------------------------------
-- How numbers are said.
--------------------------------------------------------------------------

local function short(name)
  local n = (name or ""):gsub("%.gguf$", "")
  local base, quant = n:match("^(.*)%.([^.]*)$")

  return base or n, quant or ""
end

local function gb(bytes)
  return bytes >= 1e9 and ("%.2f GB"):format(bytes / 1e9) or ("%.0f MB"):format(bytes / 1e6)
end

-- Seconds as a person reads them: milliseconds under one.
local function time_words(seconds)
  if seconds < 1 then return ("%d ms"):format(math.floor(seconds * 1000 + 0.5)) end

  return ("%.1f s"):format(seconds)
end

local function rate_words(r)
  return r >= 10 and ("%.0f"):format(r) or ("%.1f"):format(r)
end

local function ago(us)
  local s = (now.up_us - us) // 1000000

  if s < 60 then return s .. " s ago" end
  if s < 3600 then return (s // 60) .. " min ago" end

  return (s // 3600) .. " h ago"
end

local function up_words(us)
  local s = us // 1000000
  local h, m = s // 3600, (s % 3600) // 60

  if h > 0 then return ("%d hour%s %d minute%s"):format(h, h == 1 and "" or "s", m, m == 1 and "" or "s") end
  if m > 0 then return ("%d minute%s"):format(m, m == 1 and "" or "s") end

  return s .. " seconds"
end

-- An application by the name it declares (`kosmos: name Xavier`), else its
-- process's: `assistant` is Xavier, and `xavier` the program at the prompt.
local app_names = {}

local function app_name(process)
  if app_names[process] then return app_names[process] end

  local source = fs.read("/Kosmos/Apps/" .. process .. ".lua")
  local named = type(source) == "string" and filetypes.declared(source, "name")

  if not named and fs.read("/Kosmos/Programs/" .. process .. ".lua") then
    named = process .. ", at the prompt"
  end

  app_names[process] = named or process
  return app_names[process]
end

local function where_it_runs(s)
  local loops = s.ran.loops == "avx2" and "AVX2" or s.ran.loops == "base" and "SSE2"
                or s.ran.loops == "plain" and "plain loops" or s.ran.loops
  return ("%d core%s %s"):format(s.ran.cores, s.ran.cores == 1 and "" or "s", loops)
end

--------------------------------------------------------------------------
-- Where everything goes.
--------------------------------------------------------------------------

local function measure()
  local m = { inner = W - 2 * L.page_side }
  local y = L.page_top

  m.tiles = y
  y = y + TILE_H + L.between
  m.graph_heading = y
  y = y + L.group + 4
  m.graph = y
  y = y + GRAPH_H + L.between
  m.askers_heading = y
  y = y + L.group + 4
  m.askers = y
  y = y + (ROWS + 1) * ROW_H
  m.foot = y + 10
  m.h = m.foot + gfx.height() + L.page_foot
  return m
end

local M = measure()

local win, err = ui.window{ title = "LLM Monitor", w = W, h = L.head + TABS_H + M.h,
                            x = 150, y = 100, header = true }

if not win then
  print("llmmon: " .. tostring(err))
  return
end

win:add(ui.header{ x = 0, y = 0, w = W, title = "LLM Monitor", title_bar = true })

--------------------------------------------------------------------------
-- The page.
--------------------------------------------------------------------------

local function heading(g, x, y, left, right, colour)
  g:text(x + 3, y + (L.group - gfx.height("heading")) // 2, left, theme.text, theme.window, "heading")

  if right then
    g:text(x + M.inner - gfx.measure(right), y + (L.group - gfx.height()) // 2, right,
           colour or theme.text_dim, theme.window, "ui")
  end
end

local BIG = ui.sized("title", 24)

-- A tile: what it is small at the top, the number large, a line under it.
local function tile(g, x, y, w, key, value, unit, line)
  g:fill_round(x, y, w, TILE_H, theme.raised, L.card_r)
  g:frame_round(x, y, w, TILE_H, theme.line_soft, L.card_r)

  local tx, ty = x + 16, y + 12

  g:text(tx, ty, key:upper(), theme.text_dim, theme.raised, "label")
  ty = ty + gfx.height("label") + 4

  local shown = ui.fitted(value, w - 32, BIG)

  g:text(tx, ty, shown, theme.text, theme.raised, BIG)

  if unit and unit ~= "" then
    local ux = tx + gfx.measure(shown, BIG) + 6

    g:text(ux, ty + gfx.height(BIG) - gfx.height("ui") - 2, ui.fitted(unit, x + w - 16 - ux, "ui"),
           theme.text_dim, theme.raised, "ui")
  end

  ty = ty + gfx.height(BIG) + 4
  g:text(tx, ty, ui.fitted(line or "", w - 32, "ui"), theme.text_dim, theme.raised, "ui")
end

local function tiles(g, x, y)
  local gap = 14
  local tw = (M.inner - 3 * gap) // 4
  local s = now
  local at = function(i) return x + (i - 1) * (tw + gap) end

  -- Loaded.
  if s.state == "loading" then
    local pct = s.model_bytes > 0 and math.floor(s.loaded_bytes * 100 / s.model_bytes) or 0
    local base, quant = short(s.model)

    tile(g, at(1), y, tw, "Loading", base, quant, ("%d%% of %s"):format(pct, gb(s.model_bytes)))
  elseif s.state == "empty" then
    tile(g, at(1), y, tw, "Loaded", "Nothing", "", "no model is in memory")
  else
    local base, quant = short(s.model)

    tile(g, at(1), y, tw, "Loaded", base, quant,
         ("%s \u{b7} %s"):format(gb(s.model_bytes), where_it_runs(s)))
  end

  -- Writing: the answer being made, else the last one.
  local writing = s.answer_us > 0 and s.answer_tokens * 1e6 / s.answer_us or nil
  local minute = last_minute(writes)
  local sum = 0

  for _, w in ipairs(minute) do sum = sum + w[2] end

  tile(g, at(2), y, tw, s.state == "answering" and "Writing" or "Wrote", writing and rate_words(writing) or "-",
       "tokens / s",
       writing and (#minute > 0 and ("last minute %s \u{b7} best %s"):format(rate_words(sum / #minute),
                                                                          rate_words(best))
                    or ("%d tokens in %s"):format(s.answer_tokens, time_words(s.answer_us / 1e6)))
       or "no answer yet")

  -- Reading.
  local read = s.prompt_us > 0 and s.prompt_tokens * 1e6 / s.prompt_us or nil

  tile(g, at(3), y, tw, "Reading", read and rate_words(read) or "-", "tokens / s",
       read and ("%d tokens in %s%s"):format(s.prompt_tokens, time_words(s.prompt_us / 1e6),
                                            s.kept_tokens > 0 and (", " .. s.kept_tokens .. " kept") or "")
       or "no question yet")

  -- The first word.
  local fm = last_minute(firsts)
  local lo, hi

  for _, f in ipairs(fm) do
    lo = (not lo or f[2] < lo) and f[2] or lo
    hi = (not hi or f[2] > hi) and f[2] or hi
  end

  local fv, fu = "-", ""

  if s.first_us > 0 then
    fv, fu = time_words(s.first_us / 1e6):match("^(%S+) (%S+)$")
  end

  tile(g, at(4), y, tw, "First word", fv, fu,
       lo and (#fm > 1 and ("last minute %s - %s"):format(time_words(lo), time_words(hi))
                        or "how long a person waited")
       or (s.first_us > 0 and "how long a person waited" or "no answer yet"))
end

-- The graph's top: a round number above the busiest column.
local function graph_top()
  local most = 1

  for _, r in ipairs(rates) do if r[1] > most then most = r[1] end end

  for _, step in ipairs({ 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000 }) do
    if most <= step then return step end
  end

  return math.ceil(most)
end

local function graph(g, x, y)
  local top = graph_top()
  local shown = {}

  for i, r in ipairs(rates) do shown[i] = { math.min(100, math.floor(r[1] * 100 / top + 0.5)) } end

  ui.history.draw(g, x, y, M.inner, GRAPH_H, shown, { columns = COLUMNS, ground = GRAPH_GROUND,
                                                      colours = { LLM_COLOUR } })
  g:text(x + 10, y + 6, top .. " tokens / s", FAINT, GRAPH_GROUND, "label")

  local wrote = false

  for _, r in ipairs(rates) do if r[1] > 0 then wrote = true end end

  if not wrote then
    local text = "Nothing written since this window opened. A column every two seconds."
    g:text(x + (M.inner - gfx.measure(text)) // 2, y + (GRAPH_H - gfx.height()) // 2, text,
           FAINT, GRAPH_GROUND, "ui")
  end
end

local COLS = { 0, 0.30, 0.56, 0.70, 0.85 }

local function askers(g, x, y)
  local w = M.inner
  local function col(i) return x + 8 + math.floor((w - 16) * COLS[i]) end
  local hy = y + (ROW_H - gfx.height("label")) // 2

  for i, name in ipairs({ "Application", "State", "Answers", "Read", "Written" }) do
    g:text(col(i), hy, name:upper(), theme.text_dim, theme.window, "label")
  end

  g:fill(x, y + ROW_H - 1, w, 1, theme.line_soft)

  local list = {}

  for _, a in ipairs(now.askers) do list[#list + 1] = a end
  table.sort(list, function(a, b) return a.last_us > b.last_us end)

  if #list == 0 then
    g:text(col(1), y + ROW_H + (ROW_H - gfx.height()) // 2, "Nobody has asked since the server started.",
           theme.text_dim, theme.window, "ui")
    return
  end

  for r, a in ipairs(list) do
    if r > ROWS then break end

    local ty = y + r * ROW_H + (ROW_H - gfx.height()) // 2
    local state, colour

    if a.answering then
      state, colour = ("writing, %d of %d held"):format(now.held_tokens, now.n_ctx), LLM_COLOUR
    elseif a.loading then
      state, colour = "loading the model", theme.text
    else
      state = "idle, asked " .. ago(a.last_us)
      if a.refused > 0 then state = state .. (" \u{b7} %d refused, busy"):format(a.refused) end
    end

    g:text(col(1), ty, ui.fitted(app_name(a.name), col(2) - col(1) - 8, "ui"), theme.text, theme.window, "ui")
    g:text(col(2), ty, ui.fitted(state, col(3) - col(2) - 8, "ui"), colour or theme.text_dim, theme.window, "ui")
    g:text(col(3), ty, tostring(a.answers), theme.text_dim, theme.window, "ui")
    g:text(col(4), ty, tostring(a.read_tokens), theme.text_dim, theme.window, "ui")
    g:text(col(5), ty, tostring(a.written_tokens), theme.text_dim, theme.window, "ui")
  end
end

local body = ui.view{ x = 0, y = L.head + TABS_H, w = W, h = M.h }

function body:draw(g)
  local x = L.page_side

  g:fill(0, 0, self.w, self.h, theme.window)

  if not now then
    g:text(x + 3, M.tiles, "The LLM server does not answer: " .. tostring(why_not),
           theme.text_dim, theme.window, "ui")
    return
  end

  tiles(g, x, M.tiles)

  heading(g, x, M.graph_heading, "Writing", "the last five minutes, gaps between answers included",
          LLM_COLOUR)
  graph(g, x, M.graph)

  heading(g, x, M.askers_heading, "Who is asking",
          ("since the server started %s ago \u{b7} %d answers, %d tokens read, %d written")
          :format(up_words(now.up_us), now.answers, now.read_tokens, now.written_tokens))
  askers(g, x, M.askers)

  g:text(x + 3, M.foot, ui.fitted("One answer at a time: an application that asks while another "
         .. "is answered is refused, and counted. llm at the prompt says the same.", M.inner - 6, "ui"),
         theme.text_dim, theme.window, "ui")
end

win:add(body)

--------------------------------------------------------------------------
-- **Inside** (`docs/llmviz.html`): the ring of passages, read on this
-- window's clock - ten times a second while the page is shown - and drawn
-- from what has been read. The lease is renewed every two seconds, so the
-- server records only while somebody is looking.
--------------------------------------------------------------------------

local VIOLET, READ, FFN = 0xff8b7cf6, 0xff4fb6c9, 0xfff0b429
local GPU_C, CPU_C = 0xff3fb950, 0xff3b82f6
local CELL_OFF = 0xff23262e
local KEEP = 256                        -- tokens held to step back through

local ring, ring_why = nil, nil
local recs = {}                         -- the last KEEP records, oldest first
local shown = nil                       -- index into recs, or nil: the latest
local chosen = nil                      -- { layer, head }, 1-based, or nil: the strongest
local layers = {}                       -- the shown record's layers, read once

local function current() return recs[shown or #recs] end

local function inside_read()
  if not ring then
    ring, ring_why = llm.inside()
    if not ring then return false end
  end

  local got = ring:read()

  for _, r in ipairs(got) do
    recs[#recs + 1] = r
    if #recs > KEEP then
      table.remove(recs, 1)
      if shown then shown = math.max(1, shown - 1) end
    end
  end

  if #got > 0 then
    if not shown then layers = {} end

    if TELL then
      local r = recs[#recs]
      print(("llmmon: inside %d tokens, last at %d %q %s, %d layers, next %q at %.0f%%")
            :format(#recs, r.pos, r.text, r.reading and "read" or "written", r.n_layer,
                    r.cands[1] and r.cands[1].text or "", (r.cands[1] and r.cands[1].odds or 0) * 100))
    end
  end

  return #got > 0
end

local function layer_of(r, l)
  if not layers[l] then layers[l] = llm.inside_layer(r, l) end
  return layers[l]
end

-- The text of the token at a position: the latest record of it, at or
-- before record `before`, in the same conversation - a position that goes
-- back is a new one begun.
local function text_at(pos, before)
  for i = before, 1, -1 do
    if recs[i].pos == pos then return recs[i].text end
    if i < before and recs[i].pos >= recs[i + 1].pos then break end
  end

  return nil
end

-- The head to tell of: the one chosen, else the one that looked hardest
-- at a token other than itself.
local function telling(r)
  if chosen then return chosen[1], chosen[2] end

  local bl, bh, bs = 1, 1, -1

  for l = 1, math.min(r.n_layer, 64) do
    local L = layer_of(r, l)

    for h = 1, #L.share do
      if L.back[h] > 0 and L.share[h] > bs then bl, bh, bs = l, h, L.share[h] end
    end
  end

  return bl, bh
end

local function mix(a, b, t)
  local out = 0xff000000

  for _, k in ipairs({ 16, 8, 0 }) do
    local ca, cb = (a >> k) & 255, (b >> k) & 255
    out = out | (math.floor(ca + (cb - ca) * t + 0.5) << k)
  end

  return out
end

local function shown_text(text) return (text:gsub("\n", "\u{21b5}")) end
local function quoted(text) return '"' .. shown_text(text) .. '"' end

-- Where everything on the page goes.
local IN = {}
do
  local inner = W - 2 * L.page_side
  IN.side_w = 280
  IN.net_w = inner - IN.side_w - 24 - 56      -- the grid's own labels to its right
  IN.phase = L.page_top - 8
  IN.strip = IN.phase + 32
  IN.strip_h = 108
  IN.grid_label = IN.strip + IN.strip_h + 14
  IN.grid = IN.grid_label + 36
  IN.cell_h = 6
  IN.ffn = IN.grid + 32 * IN.cell_h + 10
  IN.ffn_h = 34
  IN.stream = IN.ffn + IN.ffn_h + 8
  IN.note = IN.stream + 24
end

local inside_view = ui.view{ x = 0, y = L.head + TABS_H, w = W, h = M.h, hidden = true }

local function token_strip(g, x, y, w, r, look_pos)
  local chip_h, gap, row_h = 20, 3, 24
  local top, bottom = y + 40, y + IN.strip_h - 30
  local placed = {}
  local last = shown or #recs

  -- From the shown token back, as many as fit; then laid out from the
  -- first of them, so the text reads left to right.
  local rows = (bottom - top + 4) // row_h
  local first, width, row = last, 0, 1

  while first >= 1 do
    local cw = gfx.measure(shown_text(recs[first].text), "mono") + 8 + gap

    if width + cw > w then
      row, width = row + 1, 0
      if row > rows then break end
    end

    width = width + cw
    first = first - 1
  end
  first = first + 1

  local cx, cy = x, top

  for i = first, #recs do
    local t = recs[i]
    local label = shown_text(t.text)
    local cw = gfx.measure(label, "mono") + 8

    if cx + cw > x + w then
      cx, cy = x, cy + row_h
      if cy + chip_h > bottom then break end
    end

    local ground = t.reading and 0xff1d2733 or 0xff241f40
    local ink = t.reading and 0xffb8dfe6 or 0xffcfc8fb

    if i > last then ink = mix(ink, GRAPH_GROUND, 0.7) end

    g:fill_round(cx, cy, cw, chip_h, ground, 4)
    if i == last then g:frame_round(cx, cy, cw, chip_h, VIOLET, 4) end
    g:text(cx + 4, cy + (chip_h - gfx.height("mono")) // 2, label, ink, ground, "mono")
    placed[i] = { x = cx + cw // 2, y = cy, w = cw, left = cx }
    cx = cx + cw + gap
  end

  -- The arc from the token being made to where the chosen head looks.
  if look_pos and placed[last] then
    local to

    for i = last - 1, first, -1 do
      if recs[i].pos == look_pos then to = placed[i] break end
      if recs[i].pos > recs[i + 1].pos then break end
    end

    if to then
      local a, b = placed[last], to
      local lift = math.min(30, 10 + math.abs(a.x - b.x) // 10)
      local pts = {}

      for k = 0, 20 do
        local t = k / 20
        pts[#pts + 1] = a.x + (b.x - a.x) * t
        pts[#pts + 1] = a.y + (b.y - a.y) * t - lift * 4 * t * (1 - t)
      end

      g:polyline(pts, 2, READ)
      g:frame_round(b.left, b.y, b.w, chip_h, READ, 4)
    end
  end
end

-- A head's row, as tall as the heads' room allows: phi-2's 32 heads take
-- six pixels each, and a small model's few are not left in a corner of it.
local function head_h(r)
  return (32 * IN.cell_h) // math.max(1, math.min(r.n_head, 32))
end

local function grid(g, x, y, r, tl, th)
  local n_l = math.min(r.n_layer, 64)
  local n_h = math.min(r.n_head, 32)
  local ch = head_h(r)
  local col = math.min(28, IN.net_w // math.max(1, n_l))
  local cw = math.max(2, col - 3)
  local most = 0

  for l = 1, n_l do
    local L = layer_of(r, l)
    if L.stream > most then most = L.stream end
  end

  g:text(x, IN.grid_label, ("THE %d LAYERS, THE TOKEN PASSING THROUGH THEM LEFT TO RIGHT"):format(n_l),
         FAINT, theme.window, "label")

  for l = 1, n_l do
    local L = layer_of(r, l)
    local lx = x + (l - 1) * col

    -- Each head: bright when it looked hard at one token, dim when its
    -- attention was spread out.
    for h = 1, n_h do
      g:fill(lx, y + (h - 1) * ch, cw, ch - 1, mix(CELL_OFF, VIOLET, math.min(1, L.share[h] or 0)))
    end

    if l == tl then
      g:frame_round(lx - 2, y + (th - 1) * ch - 2, cw + 4, ch + 3, theme.text, 0)
    end

    -- The share of its feed-forward neurons that fired.
    local f = r.n_ff > 0 and L.fired / r.n_ff or 0
    local fh = math.max(1, math.floor(IN.ffn_h * f + 0.5))

    g:fill(lx, IN.ffn, cw, IN.ffn_h, CELL_OFF)
    g:fill(lx, IN.ffn + IN.ffn_h - fh, cw, fh, FFN)

    -- The residual stream, thicker as it gathers.
    local sh = most > 0 and math.max(1, math.floor(12 * L.stream / most + 0.5)) or 1

    g:fill(lx, IN.stream + (12 - sh) // 2, col, sh, VIOLET)

    if l == 1 or l % 4 == 0 then
      g:text(lx, y - 16, tostring(l), l == tl and theme.text or FAINT, theme.window, "label")
    end
  end

  g:text(x + n_l * col + 4, y, "heads", FAINT, theme.window, "label")
  g:text(x + n_l * col + 4, IN.ffn + IN.ffn_h - gfx.height("label"), "fired", FFN, theme.window, "label")
  g:text(x + n_l * col + 4, IN.stream, "stream", VIOLET, theme.window, "label")
end

local function side(g, x, y, r)
  local w = IN.side_w

  g:text(x, y, "WHAT IT MIGHT SAY NEXT", FAINT, theme.window, "label")
  y = y + 22

  if #r.cands == 0 then
    g:text(x, y, "a batch's last token says", theme.text_dim, theme.window, "ui")
    y = y + 18
  end

  for k, c in ipairs(r.cands) do
    local ink = k == 1 and VIOLET or theme.text_dim
    local bar = w - 104 - 52

    g:text(x, y, ui.fitted(quoted(c.text), 98, "mono"), ink, theme.window, "mono")
    g:fill_round(x + 104, y + 4, bar, 8, theme.sunken, 4)
    g:fill_round(x + 104, y + 4, math.max(2, math.floor(bar * c.odds + 0.5)), 8, k == 1 and VIOLET or FAINT, 4)

    local pct = c.odds >= 0.1 and ("%.0f%%"):format(c.odds * 100) or ("%.1f%%"):format(c.odds * 100)

    g:text(x + w - gfx.measure(pct, "mono"), y, pct, theme.text_dim, theme.window, "mono")
    y = y + 16
  end

  y = y + 10
  g:text(x, y, "WHERE ITS PRODUCTS RAN", FAINT, theme.window, "label")
  y = y + 20

  local gpu = r.rows_all > 0 and r.rows_gpu / r.rows_all or 0
  local pct = math.floor(gpu * 100 + 0.5)

  g:fill_round(x, y, w, 10, CPU_C, 5)
  if gpu > 0 then g:fill_round(x, y, math.max(6, math.floor(w * gpu + 0.5)), 10, GPU_C, 5) end
  y = y + 16
  g:text(x, y, ("GPU %d%%, processor %d%%"):format(pct, 100 - pct), theme.text_dim, theme.window, "ui")
  y = y + 24

  g:text(x, y, "THIS TOKEN", FAINT, theme.window, "label")
  y = y + 20

  for _, kv in ipairs({
    { "position", tostring(r.pos) },
    { r.reading and "read with" or "written", r.reading and (r.batch .. " tokens") or "alone" },
    { "its step through the model", time_words(r.us / 1e6) },
    { "layers, heads, neurons", ("%d, %d, %d"):format(r.n_layer, r.n_head, r.n_ff) },
  }) do
    g:text(x, y, kv[1], theme.text_dim, theme.window, "ui")
    g:text(x + w - gfx.measure(kv[2], "ui"), y, kv[2], theme.text, theme.window, "ui")
    y = y + 18
  end
end

function inside_view:draw(g)
  local x = L.page_side

  g:fill(0, 0, self.w, self.h, theme.window)

  local r = current()

  if not r then
    local why = ring and ("Nothing recorded yet: ask Xavier something, and each token appears here as "
                          .. "it passes through the model.")
                or ("The LLM server does not hand over its ring: " .. tostring(ring_why))

    g:text(x + 3, IN.phase + 8, ui.fitted(why, W - 2 * x, "ui"), theme.text_dim, theme.window, "ui")
    return
  end

  local tl, th = telling(r)
  local look = layer_of(r, tl)
  local back = look and look.back[th] or 0
  local look_pos = r.pos - back

  -- What the model is doing now.
  local pill = r.reading and "reading" or "writing"
  local pw = gfx.measure(pill, "mono") + 18

  g:fill_round(x, IN.phase, pw, 20, r.reading and 0xff193a42 or 0xff2c2850, 10)
  g:text(x + 9, IN.phase + (20 - gfx.height("mono")) // 2, pill, r.reading and READ or VIOLET, nil, "mono")
  g:text(x + pw + 12, IN.phase + (20 - gfx.height("ui")) // 2,
         r.reading and ("the question: token %d, read with %d at once"):format(r.pos, r.batch)
                   or ("the answer: token %d, made from all before it"):format(r.pos),
         theme.text_dim, theme.window, "ui")

  local state = shown and ("held on %d of the last %d \u{b7} End follows"):format(shown, #recs)
                or ("following \u{b7} %d tokens held"):format(#recs)
  if ring and ring.skipped > 0 then state = state .. (" \u{b7} %d skipped"):format(ring.skipped) end
  g:text(W - x - gfx.measure(state, "ui"), IN.phase + (20 - gfx.height("ui")) // 2, state, FAINT,
         theme.window, "ui")

  -- The text as the model sees it, and where the chosen head looks.
  g:fill_round(x, IN.strip, W - 2 * x, IN.strip_h, GRAPH_GROUND, L.card_r)
  -- The key, along the strip's foot, so the arcs have the room above.
  do
    local ky = IN.strip + IN.strip_h - 8 - gfx.height("label")
    local kx = x + 12

    for _, k in ipairs({ { 0xff1d2733, 0xffb8dfe6, "the question, read in batches" },
                         { 0xff241f40, 0xffcfc8fb, "the answer, a token at a time" },
                         { nil, READ, "the arc: where the chosen head looks from the token being made" } }) do
      if k[1] then
        g:fill_round(kx, ky + 3, 12, 12, k[1], 3)
        g:frame_round(kx, ky + 3, 12, 12, k[2], 3)
        kx = kx + 18
      end

      g:text(kx, ky, k[3], k[2], GRAPH_GROUND, "label")
      kx = kx + gfx.measure(k[3], "label") + 28
    end
  end
  token_strip(g, x + 12, IN.strip, W - 2 * x - 24, r, back > 0 and look_pos or nil)

  grid(g, x, IN.grid, r, tl, th)
  side(g, x + IN.net_w + 56 + 24, IN.grid_label, r)

  -- In words.
  if look then
    local said = text_at(look_pos, shown or #recs)
    local note = ("Layer %d, head %d looks mostly at %s, %d back, with %.0f%% of its attention. "
                  .. "Layer %d's feed-forward fired %d of its %d neurons.")
                 :format(tl, th, back == 0 and "the token itself" or (said and quoted(said) or "an earlier token"),
                         back, look.share[th] * 100, tl, look.fired, r.n_ff)

    g:text(x, IN.note, ui.fitted(note, IN.net_w, "ui"), theme.text, theme.window, "ui")
  end

  g:text(x + 3, M.foot, ui.fitted("Recorded only while this page is open; the server's answers are the same "
         .. "with it shut. Click a head; Left and Right step through the last " .. KEEP
         .. " tokens, End follows. llm inside at the prompt says the same.", W - 2 * x - 6, "ui"),
         theme.text_dim, theme.window, "ui")
end

-- A click on a head chooses it.
function inside_view:mouse(action, x, y)
  return ui.click(self, action, x, y, function(px, py)
    local r = current()
    if not r then return nil end

    local n_l = math.min(r.n_layer, 64)
    local col = math.min(28, IN.net_w // math.max(1, n_l))
    local gx, gy = px - L.page_side, py - IN.grid

    if gx >= 0 and gx < col * n_l and gy >= 0 and gy < 32 * IN.cell_h then
      local l, h = gx // col + 1, gy // head_h(r) + 1

      if h <= math.min(r.n_head, 32) then
        return "head " .. l .. " " .. h, function()
          chosen = { l, h }
          win.dirty = true
        end
      end
    end

    return nil
  end)
end

win:add(inside_view)

--------------------------------------------------------------------------
-- **Story** (`docs/llmstory.html`): the shown token's passage as a tower,
-- its last layer at the top. Each layer's guess comes from the server's
-- lens (`ring:lens`), asked on the window's clock for the token shown and
-- kept; while an answer runs the server is busy, and it is asked again.
--------------------------------------------------------------------------

local story_view = ui.view{ x = 0, y = L.head + TABS_H, w = W, h = M.h, hidden = true }
local lenses, lens_order = {}, {}       -- by record seq: { layers } or { why }
local story_layer = nil                 -- a row chosen, 1-based, or nil: where it arrives

local ST = {}
do
  ST.tower_label = IN.strip + IN.strip_h + 12
  ST.tower = ST.tower_label + 22
  ST.bottom = M.foot - 30
  ST.num_w, ST.look_w, ST.fire_w = 56, 330, 130
end

local function lens_kept(seq, got)
  lenses[seq] = got
  lens_order[#lens_order + 1] = seq
  if #lens_order > 32 then lenses[table.remove(lens_order, 1)] = nil end
end

-- The first layer from which every guess is the top layer's.
local function arrival(got)
  local n = #got.layers
  local a = n

  while a > 1 and got.layers[a - 1].token == got.layers[n].token do a = a - 1 end
  return a
end

-- On the clock: the shown token's guesses, if not yet had. True when
-- something came to draw.
local function story_fetch()
  local r = current()
  if not r or not ring or lenses[r.seq] then return false end

  local got, why = ring:lens(r.seq)

  if not got and why and why:find("busy") then return false end
  lens_kept(r.seq, got or { why = why })

  if TELL then
    if got and #got.layers > 0 then
      local top = got.layers[#got.layers]
      print(("llmmon: story at %d %q, %d layers, the top layer's guess %q at %.0f%%, arriving at layer %d")
            :format(r.pos, r.text, #got.layers, top.text, top.odds * 100, arrival(got)))
    else
      print(("llmmon: story at %d %q, no guesses: %s"):format(r.pos, r.text, tostring(why)))
    end
  end
  return true
end

local function looked_words(r, Ly, at)
  local words = {}

  for _, look in ipairs(Ly.looks or {}) do
    local said = look.back == 0 and "itself" or text_at(r.pos - look.back, at)
    said = look.back == 0 and said or (said and quoted(said) or "an earlier word")
    words[#words + 1] = ("%s %.0f%%"):format(said, look.share * 100)
  end
  return #words > 0 and table.concat(words, ", ") or "nothing before it"
end

local function story_row_h(r)
  local n_l = math.min(r.n_layer, 64)
  return n_l, math.max(6, math.min(20, (ST.bottom - ST.tower) // math.max(1, n_l)))
end

function story_view:draw(g)
  local x = L.page_side
  local r = current()

  g:fill(0, 0, self.w, self.h, theme.window)

  if not r then
    local why = ring and ("Nothing recorded yet: ask Xavier something, and choose a word here to see how "
                          .. "the model came to it.")
                or ("The LLM server does not hand over its ring: " .. tostring(ring_why))

    g:text(x + 3, IN.phase + 8, ui.fitted(why, W - 2 * x, "ui"), theme.text_dim, theme.window, "ui")
    return
  end

  local got = lenses[r.seq]
  local made = r.cands[1] and r.cands[1].text or nil
  local headline = made and ("How %s led to %s: up its %d layers, and what each would have said"):format(
                     quoted(r.text), quoted(made), r.n_layer)
                   or ("%s, read with the question: up its %d layers"):format(quoted(r.text), r.n_layer)

  g:text(x, IN.phase + 2, ui.fitted(headline, W - 2 * x - 240, "ui"), theme.text, theme.window, "ui")

  local state = shown and ("held on %d of the last %d \u{b7} End follows"):format(shown, #recs)
                or ("following \u{b7} %d tokens held"):format(#recs)
  g:text(W - x - gfx.measure(state, "ui"), IN.phase + 2, state, FAINT, theme.window, "ui")

  g:fill_round(x, IN.strip, W - 2 * x, IN.strip_h, GRAPH_GROUND, L.card_r)
  token_strip(g, x + 12, IN.strip, W - 2 * x - 24, r, nil)

  local n_l, row_h = story_row_h(r)
  local face = row_h >= gfx.height("mono") + 2 and "mono" or (row_h >= gfx.height("label") and "label" or nil)
  local layers_got = got and got.layers and #got.layers > 0 and got.layers or nil
  local arrive = layers_got and arrival(got) or nil
  local final = layers_got and layers_got[#layers_got].token
  local guess_x = x + ST.num_w + ST.look_w + ST.fire_w + 24
  local odds_x = W - x - 110

  g:text(x, ST.tower_label, "LAYER", FAINT, theme.window, "label")
  g:text(x + ST.num_w, ST.tower_label, "LOOKED BACK AT, PAST THE FIRST WORD", FAINT, theme.window, "label")
  g:text(x + ST.num_w + ST.look_w, ST.tower_label, "RECALLED", FAINT, theme.window, "label")
  g:text(guess_x, ST.tower_label, "WOULD HAVE SAID, STOPPING HERE", FAINT, theme.window, "label")

  -- The tower, the last layer on top: the word climbs from the bottom.
  for l = 1, n_l do
    local Ly = layer_of(r, l)
    local ry = ST.tower + (n_l - l) * row_h
    local guess = layers_got and layers_got[l]
    local lit = l == (story_layer or arrive)
    local ground = lit and mix(theme.window, VIOLET, 0.16) or theme.window

    if lit then g:fill(x - 4, ry, W - 2 * x + 8, row_h - 1, ground) end
    if arrive and l >= arrive then g:fill(x - 4, ry, 3, row_h - 1, VIOLET) end

    local f = r.n_ff > 0 and Ly.fired / r.n_ff or 0
    local bar_h = math.max(2, math.min(8, row_h - 3))
    local bar_y = ry + (row_h - bar_h) // 2
    local fx = x + ST.num_w + ST.look_w

    g:fill_round(fx, bar_y, ST.fire_w - 16, bar_h, CELL_OFF, 3)
    g:fill_round(fx, bar_y, math.max(2, math.floor((ST.fire_w - 16) * f + 0.5)), bar_h, FFN, 3)

    if guess then
      g:fill_round(odds_x, bar_y, 100, bar_h, CELL_OFF, 3)
      g:fill_round(odds_x, bar_y, math.max(2, math.floor(100 * guess.odds + 0.5)), bar_h,
                   guess.token == final and VIOLET or FAINT, 3)
    end

    if face then
      local ty = ry + (row_h - gfx.height(face)) // 2

      g:text(x, ty, tostring(l), lit and theme.text or FAINT, ground, face)
      g:text(x + ST.num_w, ty, ui.fitted(looked_words(r, Ly, shown or #recs), ST.look_w - 12, face),
             READ, ground, face)
      if guess then
        g:text(guess_x, ty, ui.fitted(("%s %.0f%%"):format(quoted(guess.text), guess.odds * 100),
                                      odds_x - guess_x - 12, face),
               guess.token == final and VIOLET or theme.text_dim, ground, face)
      end
    end
  end

  -- In words: the row chosen, or where the answer arrives.
  local note

  if got and got.why then
    note = "The layers' guesses are not kept for this word: " .. got.why .. "."
  elseif not layers_got then
    note = "The layers' guesses come when the answer has ended: the model is busy writing."
  else
    local l = story_layer or arrive
    local Ly, gl = layer_of(r, l), layers_got[l]

    note = ("Layer %d looked back at %s; %d%% of its neurons fired; had it been the last, it would "
            .. "have said %s at %.0f%%. "):format(l, looked_words(r, Ly, shown or #recs),
                                                   r.n_ff > 0 and Ly.fired * 100 // r.n_ff or 0,
                                                   quoted(gl.text), gl.odds * 100)
           .. ("The answer arrives at layer %d of %d and stays."):format(arrive, n_l)
  end
  g:text(x, ST.bottom + 6, ui.fitted(note, W - 2 * x, "ui"), theme.text, theme.window, "ui")

  g:text(x + 3, M.foot, ui.fitted("Click a layer to tell of it; Left and Right step through the last "
         .. KEEP .. " tokens, End follows. The guesses are worked out after the word, for the last 64 "
         .. "words written while this page or Inside was open.", W - 2 * x - 6, "ui"),
         theme.text_dim, theme.window, "ui")
end

-- A click on a row chooses its layer.
function story_view:mouse(action, x, y)
  return ui.click(self, action, x, y, function(_, py)
    local r = current()
    if not r then return nil end

    local n_l, row_h = story_row_h(r)
    local k = (py - ST.tower) // row_h

    if py >= ST.tower and k < n_l then
      return "layer " .. (n_l - k), function()
        story_layer = n_l - k
        win.dirty = true
      end
    end
    return nil
  end)
end

win:add(story_view)

local tabs = ui.tabs{ x = 0, y = L.head, w = W,
                      items = { { text = "Overview" }, { text = "Inside" }, { text = "Story" } } }

local function show_page(i)
  tabs.on = i
  body.hidden = i ~= 1
  inside_view.hidden = i ~= 2
  story_view.hidden = i ~= 3

  if i >= 2 then inside_read() end
  if i == 3 then story_fetch() end
  win.dirty = true
end


tabs.on_choose = function(_, i) show_page(i) end

win:add(tabs)

-- Left and Right step back and forward through what is held; End follows
-- the model again, and forgets a head chosen.
win.on_key = function(_, c)
  if (inside_view.hidden and story_view.hidden) or #recs == 0 then return false end

  if c == keys.LEFT then
    shown = math.max(1, (shown or #recs) - 1)
  elseif c == keys.RIGHT then
    shown = shown and shown + 1 or nil
    if shown and shown >= #recs then shown = nil end
  elseif c == keys.END then
    shown, chosen, story_layer = nil, nil, nil
  else
    return false
  end

  layers = {}
  win.dirty = true
  return true
end

--------------------------------------------------------------------------
-- The sampling, on the window kit's clock: Overview's every two seconds,
-- Inside's ring ten times a second while it is shown, and its lease
-- renewed every two.
--------------------------------------------------------------------------

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }
local ticks = 0

function sampler:tick()
  ticks = ticks + 1

  local watching = not inside_view.hidden or not story_view.hidden

  if watching and inside_read() and not shown then win.dirty = true end
  if not story_view.hidden and ticks % (FAST_HZ // 2) == 0 and story_fetch() then win.dirty = true end

  if ticks % (FAST_HZ * PACE_S) == 0 then
    if OPEN_INSIDE and OPEN_STORY and ticks == FAST_HZ * PACE_S then show_page(3) end
    sample()
    if watching and ring then ring:renew() end
    win.dirty = true
  end
end

win:add(sampler)

sample()
print(now and ("llmmon: the LLM server, up " .. up_words(now.up_us))
          or ("llmmon: the LLM server does not answer: " .. tostring(why_not)))
if OPEN_INSIDE then show_page(2) elseif OPEN_STORY then show_page(3) end
win.tick_every = counter_hz // FAST_HZ
win:run()
