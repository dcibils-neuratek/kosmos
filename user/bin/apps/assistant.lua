-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Xavier
-- kosmos: section applications
--
-- **Xavier, in a window** (`docs/xavier.md` X4; drawn in `docs/xavier.html`,
-- *As it opens*). Ask a question and the answer is written as the model
-- makes it; ask a follow-up and it remembers the conversation, up to what
-- the model can hold; Stop ends an answer where it is. Each conversation is
-- a file in `/Home/Xavier` (`conversations.lua`), listed down the side,
-- opened again where it was left, and thrown away to the Trash.
--
-- **The file is `assistant.lua` and the window is Xavier**: `xavier` at the
-- prompt is the program, and a name is one or the other (`ns.program`).
--
-- **Nothing here waits on the LLM server.** The model is loaded with
-- `wait = false` and the window watches STATS on its own clock; the answer
-- is read out of the region the server writes it into, two loads a pass
-- and no message (`llm.lua`, `answer:poll`). What is asked of the server -
-- STATS, ASK, STOP - it answers between two tokens, at once when it is
-- idle and within a token's time when it is writing one.
--
-- **The whole conversation is asked each turn**, and the server reads only
-- what its cache does not already hold of it (`llmproto.h`); when it no
-- longer fits, the oldest turns are left out first, the file still keeps
-- them, and the list says how many.

local ui = use("/Kosmos/Libraries/ui.lua")
local llm = use("/Kosmos/Libraries/llm.lua")
local conversations = use("/Kosmos/Libraries/conversations.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local theme = ui.theme
local L = ui.layout

local W, H = 1080, 680
local SIDE = 260
local FOOT = 30
local BOX = 52
local ROWH = 50
-- The most an answer may be: 512, or `--tokens N` - which the display
-- suite gives, so the tiny model's 256 tokens hold a follow-up.
local ANSWER_TOKENS = tonumber(tostring(args or ""):match("%-%-tokens%s+(%d+)")) or 512

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

local win, err = ui.window{ title = "Xavier", w = W, h = H, x = 90, y = 60, header = true }

if not win then
  print("xavier: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- What there is: the conversations, the one open, and the model.
--------------------------------------------------------------------------

local all = conversations.list()
local open = conversations.new()        -- not on the disk until its first answer
local model = { state = "looking", name = "", arch = "", n_ctx = 0, loaded = 0, size = 0 }
local live = nil                        -- { turn, answer, began } while one is written
local said = ""                         -- the foot's word when there is one
local last = nil                        -- the last answer's numbers, for the foot
local looked = 0                        -- when STATS was last asked
local scroll, follow_end = 0, true      -- the conversation's place
local top = 1                           -- the first row of the list drawn

local function now_epoch()
  return (clock.now() or {}).epoch or 0
end

local function seconds(us) return ("%.1f s"):format((us or 0) / 1e6) end

local function rate(tokens, us)
  if not us or us <= 0 then return 0 end
  return tokens * 1e6 / us
end

--------------------------------------------------------------------------
-- The views.
--------------------------------------------------------------------------

local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = H, follow = { "left", "top", "bottom" } }
local new_button = ui.button{ text = "New conversation", x = 16, y = L.head + 12 }
local list = ui.view{ x = 0, y = L.head + 56, w = SIDE - 1, h = H - L.head - 56,
                      follow = { "left", "top", "bottom" } }
list.focusable = true

local chip = ui.view{ w = 220, h = 24 }
local header = ui.header{ x = SIDE, y = 0, w = W - SIDE, title = "New conversation",
                          title_bar = true, right = { chip } }

local chat = ui.view{ x = SIDE, y = L.head, w = W - SIDE, h = H - L.head - BOX - FOOT,
                      follow = { "left", "right", "top", "bottom" } }

local stop = ui.button{ text = "Stop", hidden = true, follow = { "right", "bottom" } }
local field = ui.field{ x = SIDE + 16, y = H - FOOT - BOX + 10,
                        w = W - SIDE - 32 - stop.w - 8, text = "",
                        hint = "Ask Xavier\u{2026}", follow = { "left", "right", "bottom" } }
stop.x, stop.y = W - 16 - stop.w, field.y + (field.h - stop.h) // 2

local foot = ui.view{ x = SIDE, y = H - FOOT, w = W - SIDE, h = FOOT,
                      follow = { "left", "right", "bottom" } }

--------------------------------------------------------------------------
-- The conversation, laid out: each message into lines once for a width,
-- and again only when its words or the width change - a long conversation
-- is not measured word by word on every pass an answer grows.
--------------------------------------------------------------------------

local TEXT_LH = gfx.height("text")
local MONO_LH = gfx.height("mono")
local UI_LH = gfx.height("ui")

-- Words into lines: a paragraph wrapped, a blank line a gap, and a line
-- that begins indented - code, which phi-2 writes without fences - kept as
-- it is in the terminal's face. A fence line says code begins or ends.
local function lay(text, room)
  local out, code = {}, false

  for raw in (text .. "\n"):gmatch("(.-)\n") do
    raw = raw:gsub("\t", "    ")

    if raw:match("^%s*```") then
      code = not code
    elseif code or raw:match("^  ") then
      out[#out + 1] = { kind = "code", s = (ui.fitted(raw, room - 20, "mono")) }
    elseif raw:match("^%s*$") then
      if #out > 0 and out[#out].kind ~= "gap" then out[#out + 1] = { kind = "gap" } end
    else
      for _, l in ipairs(ui.wrapped(raw, room, "text")) do
        out[#out + 1] = { kind = "text", s = l }
      end
    end
  end

  while #out > 0 and out[#out].kind == "gap" do out[#out] = nil end

  return out
end

local function lines_height(lines)
  local h = 0

  for _, l in ipairs(lines) do
    h = h + (l.kind == "code" and MONO_LH or l.kind == "gap" and TEXT_LH // 2 or TEXT_LH)
  end

  return h
end

-- A turn's two messages, laid out for the conversation's width.
local function layout(t, w)
  local you_room = math.floor((w - 48) * 0.62)
  local xa_room = w - 48 - 56

  if not t.laid or t.laid.w ~= w or t.laid.you ~= #t.you or t.laid.xa ~= #t.xavier then
    local you = lay(t.you, you_room - 24)
    local widest = 0

    for _, l in ipairs(you) do
      widest = math.max(widest, gfx.measure(l.s or "", l.kind == "code" and "mono" or "text"))
    end

    t.laid = { w = w, you = #t.you, xa = #t.xavier, you_lines = you, you_w = widest + 24,
               xa_lines = lay(t.xavier, xa_room) }
  end

  return t.laid
end

-- What is said under an answer: what it cost, or how it is going.
local function note(t)
  if live and live.turn == t then
    local made = live.answer:tokens() or 0
    local spent = (sys.ticks() - live.began) / hz

    if made == 0 then return ("reading the question \u{00b7} %.1f s"):format(spent) end
    return ("writing \u{00b7} %d %s so far"):format(made, made == 1 and "token" or "tokens")
  end

  if (t.tokens or 0) == 0 then return t.stopped and "stopped before a word" or "" end

  return ("%d tokens \u{00b7} read in %s \u{00b7} %.1f tokens a second%s"):format(t.tokens,
    seconds(t.read_us), rate(t.tokens, t.write_us), t.stopped and " \u{00b7} stopped" or "")
end

local GAP = 18

local function turn_height(t, w)
  local laid = layout(t, w)

  return lines_height(laid.you_lines) + 20 + GAP
         + math.max(28, lines_height(laid.xa_lines) + UI_LH + 8) + GAP
end

local function conversation_height(w)
  local h = 20

  for _, t in ipairs(open.turns) do h = h + turn_height(t, w) end

  return h
end

local function draw_lines(g, x, y, lines, ink, code_ground, room)
  for _, l in ipairs(lines) do
    if l.kind == "code" then
      g:fill(x - 6, y, room + 12, MONO_LH, code_ground)
      g:text(x, y, l.s, ink, nil, "mono")
      y = y + MONO_LH
    elseif l.kind == "gap" then
      y = y + TEXT_LH // 2
    else
      g:text(x, y, l.s, ink, nil, "text")
      y = y + TEXT_LH
    end
  end

  return y
end

function chat:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  if #open.turns == 0 then
    local lines = ui.wrapped(model.state == "ready" and
      "Ask anything. The answer is written here as the model makes it, on this machine."
      or (said ~= "" and said or "Xavier is getting the model ready."), self.w - 120, "text")

    for i, l in ipairs(lines) do
      g:text(60, 60 + (i - 1) * TEXT_LH, l, theme.text_dim, nil, "text")
    end

    return
  end

  local total = conversation_height(self.w)

  if follow_end then scroll = math.max(0, total - self.h) end
  scroll = math.max(0, math.min(scroll, math.max(0, total - self.h)))

  local y = 20 - scroll
  local code_ground = theme.mix(theme.window, theme.text, 60)

  for _, t in ipairs(open.turns) do
    local h = turn_height(t, self.w)

    if y + h >= 0 and y <= self.h then
      local laid = layout(t, self.w)
      local bw = math.min(laid.you_w, math.floor((self.w - 48) * 0.62))
      local bh = lines_height(laid.you_lines) + 20

      -- Yours on the right, in the accent.
      g:fill_round(self.w - 24 - bw, y, bw, bh, theme.accent, 10)
      draw_lines(g, self.w - 24 - bw + 12, y + 10, laid.you_lines, theme.text_on,
                 theme.lift(theme.accent, -20), bw - 24)

      -- Xavier's on the left, beside its mark, and what it cost under it.
      local ay = y + bh + GAP

      g:fill_round(24, ay, 26, 26, theme.mix(theme.accent, theme.window, 400), 13)
      g:text(24 + (26 - gfx.measure("X", "ui")) // 2, ay + (26 - UI_LH) // 2, "X",
             theme.text, nil, "ui")

      local after = draw_lines(g, 64, ay + 3, laid.xa_lines, theme.text, code_ground,
                               self.w - 64 - 56)

      if live and live.turn == t then
        g:fill(64 + (laid.xa_lines[#laid.xa_lines] and
               gfx.measure(laid.xa_lines[#laid.xa_lines].s or "", "text") + 2 or 0),
               math.max(ay + 3, after - TEXT_LH), 2, TEXT_LH, theme.accent)
      end

      g:text(64, after + 4, note(t), theme.text_dim, nil, "ui")
    end

    y = y + h
  end
end

function chat:wheel(n)
  local total = conversation_height(self.w)

  scroll = math.max(0, math.min(scroll - n * ui.WHEEL_ROWS * TEXT_LH, math.max(0, total - self.h)))
  follow_end = scroll >= total - self.h
  win.dirty = true
  return true
end

--------------------------------------------------------------------------
-- The list down the side, newest first, in Today, This week and Older.
--------------------------------------------------------------------------

local rows = {}                          -- what `list` draws: a group or a conversation

local function regroup()
  local now = now_epoch()
  local offset = clock.offset()
  local today = (now + offset * 60) // 86400
  local shown = {}

  -- The open one first while it has no file yet, so asking is seen at once.
  if not open.path and #open.turns > 0 then shown[#shown + 1] = open end
  for _, c in ipairs(all) do shown[#shown + 1] = c end

  rows = {}

  local group = nil

  for _, c in ipairs(shown) do
    local day = ((c.updated ~= 0 and c.updated or now) + offset * 60) // 86400
    local g = day == today and "Today" or today - day < 7 and "This week" or "Older"

    if g ~= group then
      rows[#rows + 1] = { group = g }
      group = g
    end

    rows[#rows + 1] = { c = c }
  end
end

local function describe(c)
  local n = #c.turns
  local words = ("%d %s"):format(n, n == 1 and "turn" or "turns")

  if (c.room or 0) > 0 and (c.held or 0) > 0 then
    words = words .. (" \u{00b7} %d of %d tokens"):format(c.held, c.room)
  end

  if (c.forgotten or 0) > 0 then
    words = words .. (" \u{00b7} %d forgotten"):format(c.forgotten)
  end

  return words
end

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
  g:text(L.head_in, (L.head - 1 - gfx.height("title")) // 2, "Xavier", theme.text, nil, "title")
end

function list:draw(g)
  local y = 0

  for i = top, #rows do
    local r = rows[i]

    if y > self.h then break end

    if r.group then
      g:text(16, y + 10, r.group, theme.text_dim, nil, "ui")
      y = y + 30
    else
      local on = r.c == open
      local ink = on and theme.text_on or theme.text
      local dim = on and theme.text_on or theme.text_dim

      if on then g:fill_round(8, y + 2, self.w - 16, ROWH - 4, theme.accent, 6) end

      g:text(18, y + 7, ui.fitted(conversations.title_of(r.c), self.w - 36, "text"), ink, nil,
             "text")
      g:text(18, y + 7 + TEXT_LH, ui.fitted(describe(r.c), self.w - 36, "ui"), dim, nil, "ui")
      y = y + ROWH
    end
  end

  if #rows == 0 then
    g:text(16, 10, "No conversations yet.", theme.text_dim, nil, "ui")
  end
end

-- The row under a point in the list, or nil.
local function row_at(y)
  local at = 0

  for i = top, #rows do
    local h = rows[i].group and 30 or ROWH

    if y >= at and y < at + h then return rows[i] end

    at = at + h
  end

  return nil
end

--------------------------------------------------------------------------
-- The header's chip and the foot.
--------------------------------------------------------------------------

local function model_words()
  if model.state == "ready" then
    return model.name:gsub("%.gguf$", "") .. " \u{00b7} processor"
  elseif model.state == "loading" then
    local pct = model.size > 0 and math.floor(model.loaded * 100 / model.size) or 0
    return ("loading %s \u{00b7} %d%%"):format(model.name:gsub("%.gguf$", ""), pct)
  end

  return "no model"
end

function chip:draw(g)
  local words = model_words()
  local w = math.min(self.w, gfx.measure(words, "ui") + 20)

  g:fill_round(self.w - w, 0, w, self.h, theme.mix(theme.window, theme.accent, 160), self.h // 2)
  g:text(self.w - w + 10, (self.h - UI_LH) // 2, ui.fitted(words, w - 20, "ui"), theme.text,
         nil, "ui")
end

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local words

  if said ~= "" then
    words = said
  elseif last then
    words = ("Last answer %.1f tokens a second \u{00b7} question read in %s \u{00b7} first word "
             .. "after %s \u{00b7} ran on the processor"):format(rate(last.tokens, last.write_us),
             seconds(last.read_us), seconds(last.first_us))
  else
    words = model.state == "ready" and "Ready" or model_words()
  end

  local memory = ""

  if (open.room or 0) > 0 and (open.held or 0) > 0 then
    memory = ("%d of %d tokens of memory used"):format(open.held, open.room)
  end

  local mw = memory ~= "" and gfx.measure(memory, "ui") or 0
  local ty = (self.h - UI_LH) // 2

  g:text(16, ty, ui.fitted(words, self.w - 48 - mw, "ui"), theme.text_dim, nil, "ui")
  if memory ~= "" then g:text(self.w - 16 - mw, ty, memory, theme.text_dim, nil, "ui") end
end

--------------------------------------------------------------------------
-- What is done.
--------------------------------------------------------------------------

local function refresh()
  header.title = conversations.title_of(open)
  header.sub = #open.turns > 0 and describe(open) or nil
  stop.hidden = live == nil
  regroup()
  win.dirty = true
end

local function save(c)
  local stamp = clock.stamp(clock.now() or clock.at(0))
  local ok, why = conversations.save(c, stamp)

  if not ok then
    said = "could not keep the conversation: " .. tostring(why)
  else
    print(("xavier: kept %s, %d turns"):format(c.path, #c.turns))
  end
end

local function hex(s)
  return (s:gsub(".", function(ch) return ("%02x"):format(ch:byte()) end))
end

-- Ask: the whole conversation, the oldest turns left out while it does not
-- fit, and the answer then read as it comes.
local function ask(question)
  if live then return end

  if model.state ~= "ready" then
    said = model.state == "loading" and ("The model is still loading: " .. model_words())
           or ("There is no model to ask: put a .gguf in " .. llm.MODELS .. ".")
    win.dirty = true
    return
  end

  local turns = {}

  for i = (open.forgotten or 0) + 1, #open.turns do turns[#turns + 1] = open.turns[i] end

  local a, why, text

  while true do
    local stops
    text, stops = llm.chat(model.arch, turns, question)
    a, why = llm.ask(text, { tokens = ANSWER_TOKENS, stops = stops })

    if a or not llm.too_long(why) or #turns == 0 then break end

    table.remove(turns, 1)
    open.forgotten = (open.forgotten or 0) + 1
    print(("xavier: the conversation did not fit; %d turns forgotten"):format(open.forgotten))
  end

  if not a then
    said = "Xavier could not ask: " .. tostring(why)
    win.dirty = true
    return
  end

  if open.created == 0 then open.created = now_epoch() end

  local t = { you = question, xavier = "", tokens = 0 }

  open.turns[#open.turns + 1] = t
  live = { turn = t, answer = a, began = sys.ticks() }
  said = ""
  follow_end = true
  print(("xavier: asked %d bytes: %s"):format(#text, hex(text)))
  refresh()
end

-- The answer has ended: what it cost, from STATS, and the file kept.
local function finished()
  local t = live.turn
  local s = llm.stats()

  live = nil

  if s then
    t.tokens, t.read_us, t.write_us, t.first_us = s.answer_tokens, s.prompt_us, s.answer_us,
                                                  s.first_us
    open.held, open.room, open.model = s.held_tokens, s.n_ctx, s.model
    last = t
    print(("xavier: answered %d tokens, %d of the conversation kept and %d read, %d held: %s")
          :format(s.answer_tokens, s.kept_tokens, s.prompt_tokens, s.held_tokens, hex(t.xavier)))
  end

  open.updated = now_epoch()
  save(open)
  all = conversations.list()

  -- The open conversation is the one in the list now, so it is drawn chosen.
  for i, c in ipairs(all) do
    if c.path == open.path then all[i] = open end
  end

  refresh()
end

local function open_conversation(c)
  if live then return end

  open = c
  scroll, follow_end = 0, true
  last = nil
  print(("xavier: opened %s, %d turns"):format(c.path or "a new conversation", #c.turns))
  refresh()
  win:focus_on(field)
end

field.on_enter = function(self, text)
  local q = text:gsub("^%s+", ""):gsub("%s+$", "")

  if q == "" then return end

  self.text, self.caret = "", 1
  ask(q)
end

stop.on_click = function()
  if not live then return end

  live.turn.stopped = true
  live.answer:stop()
  print("xavier: stopped")
end

new_button.on_click = function()
  open_conversation(conversations.new())
end

function list:mouse(action, _, y)
  if action ~= "press" then return false end

  local r = row_at(y)

  if r and r.c then open_conversation(r.c) end

  win:focus_on(list)
  return true
end

function list:wheel(n)
  top = math.max(1, math.min(top - n * ui.WHEEL_ROWS, math.max(1, #rows)))
  win.dirty = true
  return true
end

-- Delete, on the list: the open conversation to the Trash, where it can
-- be taken back from, so it is not asked about first.
function list:key(c)
  local k = ui.keyparts(c)

  if (k == ui.DELETE or c == 127 or c == 8) and not live and open.path then
    local path = open.path
    local ok, why = conversations.trash(open)

    if ok then
      print("xavier: to the Trash, " .. path)
      all = conversations.list()
      open_conversation(all[1] or conversations.new())
    else
      said = "could not move it to the Trash: " .. tostring(why)
      win.dirty = true
    end

    return true
  end

  return false
end

--------------------------------------------------------------------------
-- The clock: the model watched until it is in, the answer read as it grows.
--------------------------------------------------------------------------

local function watch_model()
  local s, why = llm.stats()

  if not s then
    model.state, said = "none", tostring(why)
    return
  end

  if s.state == "empty" then
    local name = llm.choose()

    if not name then
      model.state, said = "none", "There is no model in " .. llm.MODELS .. "."
      return
    end

    local got, refused = llm.load(name, { wait = false })

    if not got then
      model.state, said = "none", "Xavier could not load " .. name .. ": " .. tostring(refused)
      return
    end

    print("xavier: loading " .. name)
    model.state, model.name = "loading", name
    return
  end

  if s.state == "loading" then
    model.state, model.name, model.loaded, model.size = "loading", s.model, s.loaded_bytes,
                                                         s.model_bytes
    return
  end

  if model.state ~= "ready" then print("xavier: ready, " .. s.model) end

  model.state, model.name, model.arch, model.n_ctx = "ready", s.model, s.arch, s.n_ctx
  if said:match("^The model is still loading") then said = "" end
end

local placed = false

function win:on_frame()
  if not placed then
    placed = true
    print(("xavier: places field %d,%d stop %d,%d new %d,%d list %d,%d rows %d"):format(
      field.x + 20, field.y + field.h // 2, stop.x + stop.w // 2, stop.y + stop.h // 2,
      new_button.x + new_button.w // 2, new_button.y + new_button.h // 2, list.x, list.y, ROWH))
  end

  if live then
    self.poll_wait_ticks = 8
    local grew = false

    while true do
      local text = live.answer:poll()

      if text == nil then
        finished()
        return true
      end

      if text == "" then break end

      live.turn.xavier = live.turn.xavier .. text
      grew = true
    end

    -- Painted when words came, and twice a second for the note under the
    -- answer, which counts as it goes - not every pass while it is read.
    if grew or sys.ticks() - (live.noted or 0) > hz // 2 then
      live.noted = sys.ticks()
      return true
    end

    return false
  end

  if model.state ~= "ready" and model.state ~= "none" then
    self.poll_wait_ticks = 50
    local before = model_words()

    if sys.ticks() - looked > hz // 2 then
      looked = sys.ticks()
      watch_model()
    end

    return model_words() ~= before
  end

  self.poll_wait_ticks = nil
  return false
end

function win:on_resize(w, h)
  W, H = w, h
end

for _, v in ipairs({ side_ground, new_button, list, header, chat, field, stop, foot }) do
  win:add(v)
end

-- The newest conversation is opened as it was left; a first one is new.
if all[1] then open = all[1] end

watch_model()
refresh()
win:focus_on(field)
print(("xavier: %d conversations in %s"):format(#all, conversations.DIR))

win:run()
