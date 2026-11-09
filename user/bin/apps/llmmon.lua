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

local W        = 900
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

local win, err = ui.window{ title = "LLM Monitor", w = W, h = L.head + M.h,
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

local body = ui.view{ x = 0, y = L.head, w = W, h = M.h }

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
-- The sampling, on the window kit's clock.
--------------------------------------------------------------------------

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  sample()
  win.dirty = true
end

win:add(sampler)

sample()
print(now and ("llmmon: the LLM server, up " .. up_words(now.up_us))
          or ("llmmon: the LLM server does not answer: " .. tostring(why_not)))
win.tick_every = counter_hz * PACE_S
win:run()
