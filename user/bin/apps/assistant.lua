-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Xavier
-- kosmos: section applications
-- kosmos: needs network
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
local files = use("/Kosmos/Libraries/files.lua")
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

-- **Asked from anywhere** (Super+X, `xavier.html`): the box over whatever
-- is in front, rather than this window.
if tostring(args or ""):match("%-%-ask") then
  use("/Kosmos/Libraries/xavierbox.lua").run(ANSWER_TOKENS)
  return
end

-- **A conversation carried in** from the box by Tab: opened, not the newest.
local OPENING = tostring(args or ""):match("%-%-open%s+(%S+)")

local win, err = ui.window{ title = "Xavier", w = W, h = H, x = 90, y = 60, header = true }

if not win then
  print("xavier: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- What there is: the conversations, the one open, and the model.
--------------------------------------------------------------------------

-- **A model for each conversation** (`xavier.html`, *A model for each
-- conversation*): a conversation is asked of its own model, kept in its
-- file, and loaded when it is opened - the server holds one at a time. A
-- new one begins with the model chosen last.
local chosen = conversations.chosen()   -- the model chosen last
local refused = {}                      -- models the server could not find, not asked for again
local load_began = nil                  -- { name, ticks, bytes } as a load was first seen

local function fresh()
  local c = conversations.new()
  c.model = chosen or llm.choose() or ""
  return c
end

local all = conversations.list()
local open = fresh()                    -- not on the disk until its first answer
local model = { state = "looking", name = "", arch = "", n_ctx = 0, loaded = 0, size = 0 }

-- The model this conversation is asked of: its own, else the one chosen
-- last, else the one `llm.choose` prefers.
local function wanted()
  if (open.model or "") ~= "" then return open.model end
  return chosen or llm.choose()
end

-- A model's file name as a person reads it: "phi-2.Q4_0.gguf" is "phi-2 Q4_0".
local function short(name)
  local n = (name or ""):gsub("%.gguf$", "")
  n = n:gsub("%.([^.]*)$", " %1")
  return n
end
local live = nil                        -- { turn, answer, began } while one is written
local said = ""                         -- the foot's word when there is one
local last = nil                        -- the last answer's numbers, for the foot
local looked = 0                        -- when STATS was last asked
local scroll, follow_end = 0, true      -- the conversation's place
local top = 1                           -- the first row of the list drawn
local sheet = nil                       -- Settings, while it is open
local compare = nil                     -- { runs, at, answer, rows, before } while it runs
local compared = {}                     -- the rows Compare last filled

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
local more = ui.iconbutton{ icon = "more" }
local header = ui.header{ x = SIDE, y = 0, w = W - SIDE, title = "New conversation",
                          title_bar = true, right = { chip, more } }

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

  return ("%s%d tokens \u{00b7} read in %s \u{00b7} %.1f tokens a second%s"):format(
    (t.model or "") ~= "" and (short(t.model) .. " \u{00b7} ") or "", t.tokens,
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

-- How a conversation is described: in the list down the side, its turns
-- and its model; in the header, its turns and the memory it holds.
local function describe(c, side)
  local n = #c.turns
  local words = ("%d %s"):format(n, n == 1 and "turn" or "turns")

  if side then
    if (c.model or "") ~= "" then words = words .. " \u{00b7} " .. short(c.model) end
    return words
  end

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
      g:text(18, y + 7 + TEXT_LH, ui.fitted(describe(r.c, true), self.w - 36, "ui"), dim, nil,
             "ui")
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

-- What an answer ran on, as the foot says it: "ran on 4 cores, AVX2".
local LOOP_WORDS = { auto = "Auto", plain = "Plain", base = "Base vectors", avx2 = "AVX2" }

local function ran_words(ran)
  if not ran then return "ran on the processor" end

  return ("ran on %d %s, %s"):format(ran.cores, ran.cores == 1 and "core" or "cores",
                                     LOOP_WORDS[ran.loops] or ran.loops)
end

local function loaded_pct()
  return model.size > 0 and math.floor(model.loaded * 100 / model.size) or 0
end

local function model_words()
  if model.state == "ready" then
    return short(model.name) .. " \u{00b7} processor"
  elseif model.state == "loading" or model.state == "looking" then
    local name = model.state == "loading" and model.name or wanted()
    if not name then return "no model" end
    return ("loading %s \u{00b7} %d%%"):format(short(name), model.state == "loading" and loaded_pct() or 0)
  end

  return "no model"
end

-- The foot's words while a model comes in: why there is a wait, how big,
-- and how long is left - from how fast it has come so far, once there has
-- been a second of it to say.
local function loading_words()
  local words = "This conversation's model is being loaded: the server holds one at a time"

  if model.state ~= "loading" or model.size == 0 then return words end

  words = words .. " \u{00b7} " .. files.size(model.size)

  local b = load_began

  if b and b.name == model.name and model.loaded > b.bytes then
    local spent = (sys.ticks() - b.ticks) / hz

    if spent >= 1 then
      local left = (model.size - model.loaded) * spent / (model.loaded - b.bytes)
      words = words .. (" \u{00b7} about %d s from %s"):format(math.ceil(left), llm.MODELS)
    end
  end

  return words
end

-- The box says what it waits for while it cannot be asked.
local function field_hint()
  if model.state == "ready" or model.state == "none" then return "Ask Xavier\u{2026}" end

  local name = model.state == "loading" and model.name or wanted()
  if not name then return "Ask Xavier\u{2026}" end

  return ("Loading %s for this conversation \u{00b7} %d%%\u{2026}"):format(short(name),
         model.state == "loading" and loaded_pct() or 0)
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
  elseif model.state == "loading" or model.state == "looking" then
    words = loading_words()
  elseif last then
    words = ("Last answer %.1f tokens a second \u{00b7} question read in %s \u{00b7} first word "
             .. "after %s \u{00b7} %s"):format(rate(last.tokens, last.write_us),
             seconds(last.read_us), seconds(last.first_us), ran_words(last.ran))
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
  if live or compare then return end

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
    text, stops = llm.chat(model.arch, turns, question, conversations.preamble())
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
    t.ran, t.model = s.ran, s.model
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

-- What was opened: its turns, its model, and which models wrote its
-- answers, in the order they first did.
local function say_opened(c)
  local by, seen_by = {}, {}

  for _, t in ipairs(c.turns) do
    if (t.model or "") ~= "" and not seen_by[t.model] then
      seen_by[t.model] = true
      by[#by + 1] = t.model
    end
  end

  print(("xavier: opened %s, %d turns, %s; answers by %s"):format(c.path or "a new conversation",
        #c.turns, wanted() or "no model", #by > 0 and table.concat(by, ", ") or "none"))
end

local function open_conversation(c)
  if live then return end

  open = c
  scroll, follow_end = 0, true
  last = nil
  say_opened(c)

  -- Its model, when it is not the one in: looked at on the next pass.
  if model.state ~= "none" and wanted() ~= model.name then
    model.state, looked = "looking", 0
  end
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
  open_conversation(fresh())
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
      open_conversation(all[1] or fresh())
    else
      said = "could not move it to the Trash: " .. tostring(why)
      win.dirty = true
    end

    return true
  end

  return false
end

--------------------------------------------------------------------------
-- **Settings** (`docs/xavier.html`, *Settings*): where the model runs, the
-- LLM server's and so for every application that asks. Each control sets
-- the server at once and is in force from the next question; the server
-- keeps them beside the models across restarts.
--
-- **Compare** asks the loaded model one question in each setting and shows
-- how fast each read it and wrote; **Use the fastest** keeps the winner. It
-- is asked between frames, as an answer is read, so the window keeps
-- drawing while it runs.
--------------------------------------------------------------------------

local COMPARE_QUESTION = "Explain in a few sentences what an operating system does."
local COMPARE_TOKENS = 48

-- The settings it tries: every loop this machine has at Auto's cores, then
-- the cores one way and the other, and one core plain as the reference.
local function compare_runs(st)
  local runs, seen = {}, {}
  local function add(cores, loops)
    local key = cores .. loops
    if not seen[key] and (loops == "auto" or st.loops_have[loops]) and cores <= st.processors then
      seen[key] = true
      runs[#runs + 1] = { cores = cores, loops = loops }
    end
  end

  add(st.cores, "avx2")
  add(st.cores, "base")
  if st.processors ~= st.cores then add(st.processors, "avx2") add(st.processors, "base") end
  if st.cores > 1 then add(st.cores // 2, "avx2") add(st.cores // 2, "base") end
  add(1, "plain")
  return runs
end

local function compare_next()
  local c = compare
  local run = c.runs[c.at]

  if not run then
    llm.set(c.before)
    print(("xavier: compared %d settings"):format(#c.rows))
    compare = nil
    if sheet then sheet.show() end
    return
  end

  local ok, why = llm.set{ cores = run.cores, loops = run.loops }
  local a = ok and llm.ask(COMPARE_QUESTION, { tokens = COMPARE_TOKENS })

  if not a then
    c.rows[#c.rows + 1] = { cores = run.cores, loops = run.loops, why = tostring(why or "refused") }
    c.at = c.at + 1
    return compare_next()
  end

  c.answer = a
end

-- A pass of Compare: the answer read, and the next setting asked when it ends.
local function compare_step()
  local c = compare

  while true do
    local text = c.answer:poll()

    if text == nil then
      local s = llm.stats() or {}
      local run = c.runs[c.at]

      c.rows[#c.rows + 1] = { cores = run.cores, loops = run.loops,
                              read = (s.prompt_tokens or 0) * 1e6 / math.max(1, s.prompt_us or 1),
                              write = (s.answer_tokens or 0) * 1e6 / math.max(1, s.answer_us or 1),
                              ran = s.ran }
      print(("xavier: compare %d cores %s: read %.1f, wrote %.1f tokens a second"):format(
        run.cores, run.loops, c.rows[#c.rows].read, c.rows[#c.rows].write))
      c.at = c.at + 1
      c.answer = nil
      compared = c.rows
      if sheet then sheet.show() end
      compare_next()
      return
    end

    if text == "" then return end
  end
end

local function open_settings()
  if sheet or live then return end

  local st, why = llm.settings()

  if not st then
    said = "Xavier could not ask for the settings: " .. tostring(why)
    win.dirty = true
    return
  end

  -- Wide enough for the machine's line on a PC with AVX2 and no GPU, and
  -- tall enough for seven Compare rows above the buttons (the M700's sheet
  -- at 700 by 560 cut the first and covered the last, 0.11.326).
  local SW, SH = 760, 600
  local s = ui.sheet{ title = "Settings \u{00b7} the LLM server's, for every application that asks",
                      w = SW, h = SH }
  local X, CX = 18, 190

  local function label(y, text, dim)
    local l = ui.label{ x = X, y = y, text = text, color = dim and "text_dim" or nil }
    s:put(l)
    return l
  end

  local function note(y, text)
    local l = ui.label{ x = CX, y = y, w = SW - CX - 18, text = text, color = "text_dim", role = "ui" }
    s:put(l)
    return l
  end

  -- This machine.
  local have = {}
  for _, n in ipairs({ "plain", "base", "avx2" }) do
    if st.loops_have[n] then have[#have + 1] = LOOP_WORDS[n] end
  end
  label(50, "This machine")
  note(52, ("%d processors on %d cores \u{00b7} %s \u{00b7} %s"):format(st.processors, st.cores,
       table.concat(have, ", "), st.gpu ~= "" and st.gpu or "no GPU the server can use"))

  -- Cores: Auto, then the counts there are room for.
  local counts = {}
  for _, n in ipairs({ 1, 2, 4, 8, 16, 32, 64 }) do
    if n <= st.processors then counts[#counts + 1] = n end
  end
  if counts[#counts] ~= st.processors then counts[#counts + 1] = st.processors end

  local core_items, core_on = { "Auto \u{00b7} " .. st.cores }, 1
  for i, n in ipairs(counts) do
    core_items[#core_items + 1] = tostring(n)
    if st.settings.cores == n then core_on = i + 1 end
  end

  label(92, "Cores")
  s:put(ui.segments{ x = CX, y = 88, items = core_items, on = core_on, name = "Cores",
    on_change = function(_, i)
      local ok, why = llm.set{ cores = i == 1 and 0 or counts[i - 1] }
      print("xavier: cores " .. (i == 1 and "auto" or counts[i - 1]) .. (ok and "" or (": " .. tostring(why))))
    end })
  note(124, "How many processors a question is cut across. Auto is one a core.")

  -- Loops: Auto, then each this processor has.
  local loop_names, loop_items, loop_on = { "auto" }, { "Auto" }, 1
  for _, n in ipairs({ "avx2", "base", "plain" }) do
    if st.loops_have[n] then
      loop_names[#loop_names + 1] = n
      loop_items[#loop_items + 1] = LOOP_WORDS[n]
      if st.settings.loops == n then loop_on = #loop_items end
    end
  end

  label(160, "Vector loops")
  s:put(ui.segments{ x = CX, y = 156, items = loop_items, on = loop_on, name = "Vector loops",
    on_change = function(_, i)
      local ok, why = llm.set{ loops = loop_names[i] }
      print("xavier: loops " .. loop_names[i] .. (ok and "" or (": " .. tostring(why))))
    end })
  note(192, "The same answer to the bit in each; only the speed differs.")
  note(210, "Base vectors are SSE2 on a PC and NEON on ARM.")

  -- The GPU (X7): a question's products, an answer's, or both, where the
  -- server has a GPU to give them to. The answer is the same either way.
  local function gpu_switch(y, bit, name)
    label(y + 4, name)
    s:put(ui.switch{ x = CX, y = y, on = (st.settings.gpu or 0) & bit ~= 0, disabled = st.gpu_have & bit == 0,
      name = name,
      on_change = function(_, on)
        local now = llm.settings()
        local bits = now and now.settings.gpu or 0
        bits = on and (bits | bit) or (bits & ~bit)
        local ok, why = llm.set{ gpu = bits }
        print(("xavier: gpu %d"):format(bits) .. (ok and "" or (": " .. tostring(why))))
      end })
  end

  gpu_switch(228, 1, "Read questions")
  gpu_switch(264, 2, "Write answers")
  note(300, st.gpu_have == 0 and "No GPU here the server can use: the model runs on the processor."
                              or ("On %s, the same answer to the bit; Compare says where it pays."):format(st.gpu))

  -- Compare.
  label(340, "Compare")
  local status = note(372, "")
  local rows = {}
  for i = 1, 7 do rows[i] = note(394 + (i - 1) * 18, "") end

  local fastest = ui.button{ x = SW - 18 - 140, y = SH - 50, w = 140, text = "Use the fastest",
                             disabled = true, name = "Use the fastest" }

  local function best()
    local b = nil
    for _, r in ipairs(compared) do
      if r.write and (not b or r.write > b.write) then b = r end
    end
    return b
  end

  function s.show()
    status.text = compare and ("asking in setting %d of %d\u{2026}"):format(compare.at, #compare.runs)
                  or (#compared > 0 and "Writing is what a person waits for; the fastest at it is marked."
                      or ("Asks the loaded model one question in each setting, %d tokens each."):format(COMPARE_TOKENS))
    local b = best()
    for i, l in ipairs(rows) do
      local r = compared[i]
      l.text = r and (r.why and ("%d cores, %s: %s"):format(r.cores, LOOP_WORDS[r.loops], r.why)
               or ("%s%d %s, %s: question %.1f tokens a second, answer %.1f"):format(
                 r == b and "\u{2713} " or "", r.cores, r.cores == 1 and "core" or "cores",
                 LOOP_WORDS[r.loops], r.read, r.write)) or ""
    end
    fastest.disabled = compare ~= nil or b == nil
    win.dirty = true
  end

  local compare_button
  compare_button = ui.button{ x = CX, y = 336, w = 110, text = "Compare", go = true, name = "Compare",
    on_click = function()
      if compare or model.state ~= "ready" then return end
      local now = llm.settings()
      if not now then return end
      compare = { runs = compare_runs(now), at = 1, rows = {},
                  before = { cores = now.settings.cores, loops = now.settings.loops } }
      compared = compare.rows
      print(("xavier: comparing %d settings"):format(#compare.runs))
      compare_next()
      s.show()
    end }
  s:put(compare_button)

  fastest.on_click = function()
    local b = best()
    if not b or compare then return end
    llm.set{ cores = b.cores, loops = b.loops }
    print(("xavier: the fastest kept, %d cores %s"):format(b.cores, b.loops))
    s:close()
    sheet = nil
    open_settings()
  end
  s:put(fastest)

  local done = ui.button{ x = SW - 18 - 140 - 10 - 80, y = SH - 50, w = 80, text = "Done", name = "Done",
    on_click = function() if not compare then s:close() sheet = nil print("xavier: settings closed") end end }
  s:put(done)

  s.show()
  sheet = s
  win:add(s)

  local function at(b) return s.panel.x + b.x + b.w // 2, s.panel.y + b.y + b.h // 2 end
  local cx, cy = at(compare_button)
  local dx, dy = at(done)
  local fx, fy = at(fastest)
  print(("xavier: settings, %d processors on %d cores, cores %d, loops %s; compare %d,%d done %d,%d "
         .. "fastest %d,%d"):format(st.processors, st.cores, st.settings.cores, st.settings.loops,
         cx, cy, dx, dy, fx, fy))
end

--------------------------------------------------------------------------
-- **The model chip is a menu**: every model in the models folder, its size,
-- and a mark on this conversation's. Choosing one makes it this
-- conversation's and the one a new conversation begins with, and loads it.
--------------------------------------------------------------------------

local show_page                         -- the Models page, shown or not (below)

local function chip_width()
  return math.min(chip.w, gfx.measure(model_words(), "ui") + 20)
end

local function choose_model(name)
  if live or compare then return end

  open.model, chosen, refused[name] = name, name, nil

  local ok, why = conversations.choose(name)
  if not ok then said = "could not keep the model chosen: " .. tostring(why) end

  if open.path then save(open) end
  print(("xavier: %s chosen for this conversation"):format(name))

  if model.name ~= name or model.state ~= "ready" then
    model.state, looked = "looking", 0
  end

  refresh()
end

local function open_models()
  if live or compare then return end

  local this, items, names = wanted(), {}, {}

  for _, n in ipairs(llm.models()) do
    local a = fs.getattr(llm.MODELS .. "/" .. n)
    local hint = files.size(a and a.size or 0)

    if n == model.name and model.state == "ready" then hint = hint .. " \u{00b7} loaded now" end

    items[#items + 1] = { text = short(n), hint = hint, mark = n == this,
                          on_choose = function() choose_model(n) end }
    names[#names + 1] = n
  end

  if #items == 0 then
    items[1] = { text = "No model in " .. llm.MODELS, disabled = true }
  else
    items[#items + 1] = { separator = true }
    items[#items + 1] = { text = "Every model in " .. llm.MODELS, disabled = true }
  end

  -- The page, where more come from.
  items[#items + 1] = { text = "Get more in Models\u{2026}", on_choose = function() show_page(true) end }

  local m = win:open_menu(win.origin_x + header.x + chip.x + chip.w - chip_width(),
                          win.origin_y + L.head, items)

  if m then
    print(("xavier: models menu at %d,%d, rows of %d: %s"):format(m.x - win.origin_x,
          m.y - win.origin_y, m.row, table.concat(names, ", ")))
  end
end

function chip:mouse(action, x)
  if action ~= "press" or x < self.w - chip_width() then return false end

  open_models()
  return true
end

more.on_click = function()
  local m = win:open_menu(win.origin_x + header.x + more.x, win.origin_y + L.head,
                          { { text = "Settings\u{2026}", on_choose = open_settings } })

  if m then
    print(("xavier: menu at %d,%d, rows of %d"):format(m.x - win.origin_x, m.y - win.origin_y, m.row))
  end
end

--------------------------------------------------------------------------
-- **The Models page** (`docs/xavier.md`, *Models*; drawn in `xavier.html`,
-- *Models*): what is in the models folder, with Load and Remove; then a
-- search of Hugging Face, each repository with the file the kit would take
-- from it and Get, or why there is none.
--
-- **Nothing on it waits on the network.** A search and a download are each
-- `models` in a process of its own with `--report`, a file this reads four
-- times a second (`models.lua`, the program); Pause leaves a file beside
-- the report that the program looks for. The bytes never pass through the
-- window, and a TLS handshake - which the TLS Kit does waiting - is never
-- this window's to wait for.
--------------------------------------------------------------------------

local models = use("/Kosmos/Libraries/models.lua")

-- `--source URL [--name NAME]`: another place that answers as Hugging Face
-- does - a mirror, or a test's server - handed on to `models`.
local SOURCE = tostring(args or ""):match("%-%-source%s+(%S+)")
local SOURCE_NAME = tostring(args or ""):match("%-%-name%s+(%S+)")

local PAGE_ROW = 58
local here = {}                         -- models.list(), as last looked at
local found = nil                       -- the search's rows, nil before one
local found_term = ""
local searching = nil                   -- { report } while a search runs
local getting = {}                      -- file -> { repo, report, got, total, state, why }
local page_said = ""
local page_scroll = 0
local page_targets = {}                 -- what the page drew to be pressed

local back = ui.button{ text = "Conversations", name = "Conversations" }
local term = ui.field{ w = 220, text = "", hint = "phi, tinyllama\u{2026}", name = "Search words" }
local look = ui.button{ text = "Search Hugging Face", name = "Search Hugging Face" }
local page_head = ui.header{ x = 0, y = 0, w = W, title = "Models", title_bar = true,
                             left = { back }, right = { term, look }, hidden = true }
local page = ui.view{ x = 0, y = L.head, w = W, h = H - L.head - FOOT, hidden = true,
                      follow = { "left", "right", "top", "bottom" } }
local page_foot = ui.view{ x = 0, y = H - FOOT, w = W, h = FOOT, hidden = true,
                           follow = { "left", "right", "bottom" } }
local PAGE_VIEWS = { page_head, page, page_foot }
local CHAT_VIEWS = { side_ground, new_button, list, header, chat, field, stop, foot }

local function report_name(what)
  return ("/Temporary/xavier-%s-%d.txt"):format(what, sys.ticks() % 1000000007)
end

-- `models` with these words, in a process of its own; true, or nil and why.
local function run_models(words)
  if SOURCE then words = words .. " --source " .. SOURCE end
  if SOURCE_NAME then words = words .. " --name " .. SOURCE_NAME end

  local reply, why = fs.send("/Running/wm", { type = "launch", program = "models", args = words,
                                              wait = false })

  if not reply or not reply.ok then
    return nil, tostring(why or (reply and reply.error) or "refused")
  end

  return true
end

local function crumb()
  local free = models.free()

  return llm.MODELS .. (free and (", " .. files.size(free) .. " free") or "")
end

local function look_here()
  here = models.list()
  page_head.sub = crumb()
  win.dirty = true
end

-- Every line of a report, or nil while it is empty.
local function report_lines(path)
  local text = fs.read(path)

  if type(text) ~= "string" or text == "" then return nil end

  local lines = {}

  for l in text:gmatch("[^\n]+") do lines[#lines + 1] = l end

  return lines
end

local function forget_report(path)
  files.remove(path)
  if fs.getattr(path .. ".stop") then files.remove(path .. ".stop") end
end

local function search(words)
  words = words:gsub("^%s+", ""):gsub("%s+$", "")

  if searching or words == "" or words:find("[^%w%-%._ ]") then
    if words:find("[^%w%-%._ ]") then page_said = "A search is letters, digits and - . _" end
    win.dirty = true
    return
  end

  local report = report_name("search")

  fs.write(report, "")

  local ok, why = run_models("search " .. words .. " --report " .. report)

  if not ok then
    page_said = "models would not start: " .. why
  else
    searching, found, found_term, page_said = { report = report }, {}, words, ""
    print(("xavier: searching Hugging Face for %s"):format(words))
  end

  win.dirty = true
end

local function follow_search()
  local lines = searching and report_lines(searching.report)

  if not lines then return false end

  local rows, by = {}, {}

  for _, l in ipairs(lines) do
    local repo, downloads = l:match("^repo (%S+) (%d+)$")

    if repo then
      rows[#rows + 1] = { repo = repo, downloads = tonumber(downloads) }
      by[repo] = rows[#rows]
    end

    local brepo, file, size, quant = l:match("^best (%S+) (%S+) (%d+) (%S+)$")

    if brepo and by[brepo] then
      by[brepo].best = { file = file, size = tonumber(size), quant = quant }
    end

    local nrepo, why = l:match("^none (%S+) (.*)$")

    if nrepo and by[nrepo] then by[nrepo].why = why end
  end

  found = rows

  local final = lines[#lines]

  if final:match("^done") then
    if final ~= "done ok" then page_said = "Hugging Face did not answer: " .. final:sub(13) end

    local said_rows = {}

    for _, r in ipairs(rows) do
      said_rows[#said_rows + 1] = r.repo .. "=" .. (r.best and r.best.file or ("none: " .. tostring(r.why)))
    end

    print(("xavier: found %d on Hugging Face for %s: %s"):format(#rows, found_term,
          final == "done ok" and table.concat(said_rows, ", ") or final))
    forget_report(searching.report)
    searching = nil
  end

  return true
end

local function get(repo, file, size)
  if getting[file] and getting[file].state == "going" then return end

  local report = report_name("get")

  fs.write(report, "")

  local ok, why = run_models(("get %s %s --report %s"):format(repo, file, report))

  if not ok then
    page_said = "models would not start: " .. why
  else
    getting[file] = { repo = repo, report = report, got = 0, total = size or 0, state = "going" }
    page_said = ""
    print(("xavier: getting %s from %s"):format(file, repo))
  end

  win.dirty = true
end

local function pause(file)
  local d = getting[file]

  if d and d.state == "going" then
    fs.write(d.report .. ".stop", "stop\n")
    print("xavier: pausing " .. file)
  end
end

local function follow_gets()
  local changed = false

  for file, d in pairs(getting) do
    local lines = d.state == "going" and report_lines(d.report)
    local final = lines and lines[#lines]

    if final then
      local got, total = final:match("^(%d+) (%d+)$")

      if got then
        changed = changed or tonumber(got) ~= d.got
        d.got, d.total = tonumber(got), math.max(d.total, tonumber(total))
      elseif final:match("^done") then
        changed = true
        forget_report(d.report)

        if final == "done ok" then
          getting[file] = nil
          print(("xavier: got %s, %d bytes, checked"):format(file, d.total))
        else
          d.state = final == "done paused" and "paused" or "failed"
          d.why = final:match("^done failed (.*)$")
          print(("xavier: %s %s%s"):format(file, d.state, d.why and (": " .. d.why) or ""))
        end
      end
    end
  end

  if changed then look_here() end

  return changed
end

function show_page(on)
  for _, v in ipairs(PAGE_VIEWS) do v.hidden = not on end
  for _, v in ipairs(CHAT_VIEWS) do v.hidden = on end

  -- Stop is shown with the conversation only while an answer is written.
  if not on then stop.hidden = live == nil end

  if on then
    look_here()
    win:focus_on(term)
    print(("xavier: models page, %d in %s"):format(#here, llm.MODELS))
  else
    win:focus_on(field)
    print("xavier: conversations again")
  end

  win.dirty = true
end

local function remove(name)
  local m

  for _, one in ipairs(here) do
    if one.name == name then m = one end
  end

  if not m or sheet then return end

  local SW, SH = 520, 180
  local s = ui.sheet{ title = "Remove " .. short(m.name) .. "?", w = SW, h = SH }

  local words = ("%s, %s, is deleted from %s, not put in the Trash. Hugging Face still has it, "
                 .. "and Get brings it back."):format(m.name, files.size(m.size), llm.MODELS)

  for i, l in ipairs(ui.wrapped(words, SW - 36, "ui")) do
    s:put(ui.label{ x = 18, y = 50 + (i - 1) * UI_LH, role = "ui", color = "text_dim", text = l })
  end

  local function close()
    s:close()
    sheet = nil
  end

  local yes = ui.button{ x = SW - 18 - 100, y = SH - 50, w = 100, text = "Remove", go = true,
                         name = "Remove",
    on_click = function()
      local ok, why = models.remove(m.name)

      page_said = ok and ("Removed " .. m.name) or ("Could not remove it: " .. tostring(why))
      print("xavier: " .. (ok and ("removed " .. m.name) or ("not removed, " .. tostring(why))))
      getting[m.name:gsub("%.part$", "")] = nil
      close()
      look_here()
    end }
  local no = ui.button{ x = SW - 18 - 100 - 10 - 90, y = SH - 50, w = 90, text = "Cancel",
                        name = "Cancel", on_click = function() close() win.dirty = true end }

  s:put(no)
  s:put(yes)
  sheet = s
  win:add(s)
  print(("xavier: remove %s? remove %d,%d cancel %d,%d"):format(m.name,
        s.panel.x + yes.x + yes.w // 2, s.panel.y + yes.y + yes.h // 2,
        s.panel.x + no.x + no.w // 2, s.panel.y + no.y + no.h // 2))
end

back.on_click = function() show_page(false) end
look.on_click = function() search(term.text) end
term.on_enter = function(_, text) search(text) end

-- What a row of the page is: its words, and the buttons at its end.
local function page_rows()
  local rows = {}
  local this = wanted()

  for _, m in ipairs(here) do
    local file = m.name:gsub("%.part$", "")
    local d = getting[file]

    if not (d and d.state == "going") then
      local mark, about, buttons = nil, llm.MODELS .. "/" .. m.name, {}

      if m.part then
        local want = d and d.total or 0

        mark = "cut short"
        about = ("%s of %s here"):format(files.size(m.size), want > 0 and files.size(want) or "the file")
                .. (d and d.state == "failed" and d.why and (" \u{00b7} " .. d.why) or "")
                .. (d and "" or " \u{00b7} Get it from its repository again to go on")

        if d then buttons[#buttons + 1] = { text = "Go on", key = "get:" .. file,
                                            act = function() get(d.repo, file, d.total) end } end
        buttons[#buttons + 1] = { text = "Remove", key = "rm:" .. m.name, act = function() remove(m.name) end }
      else
        if m.name == model.name and model.state == "ready" then
          mark = "loaded"
        elseif m.name == model.name and model.state == "loading" then
          mark = ("loading \u{00b7} %d%%"):format(loaded_pct())
        end

        if m.name == this then about = about .. " \u{00b7} this conversation's" end

        if not (m.name == this and (model.state == "ready" or model.state == "loading") and m.name == model.name) then
          buttons[#buttons + 1] = { text = "Load", go = true, key = "load:" .. m.name,
                                    act = function() choose_model(m.name) win.dirty = true end }
        end

        buttons[#buttons + 1] = { text = "Remove", key = "rm:" .. m.name, act = function() remove(m.name) end }
      end

      rows[#rows + 1] = { title = short(m.name:gsub("%.part$", "")), mark = mark, about = about,
                          size = m.size, kind = m.quant, buttons = buttons }
    end
  end

  -- What is coming in, from this window's Gets.
  for file, d in pairs(getting) do
    if d.state == "going" then
      rows[#rows + 1] = { title = short(file), file = file,
                          about = ("%s \u{00b7} coming in \u{00b7} %s of %s"):format(d.repo,
                                  files.size(d.got), d.total > 0 and files.size(d.total) or "?"),
                          bar = d.total > 0 and d.got / d.total or 0, size = d.total,
                          kind = select(2, models.kind(file)),
                          buttons = { { text = "Pause", key = "pause:" .. file,
                                        act = function() pause(file) end } } }
    end
  end

  if #rows == 0 then rows[1] = { empty = ("No models in %s yet: search Hugging Face above."):format(llm.MODELS) } end

  if found then
    rows[#rows + 1] = { section = ("On Hugging Face, as GGUF, matching \u{201c}%s\u{201d}"):format(found_term)
                                  .. (searching and " \u{00b7} looking\u{2026}" or "") }

    for _, r in ipairs(found) do
      local about, buttons, b = nil, {}, r.best

      if b then
        local there = fs.getattr(llm.MODELS .. "/" .. b.file)

        about = ("%s \u{00b7} %d downloads"):format(b.file, r.downloads)

        if there then
          about = about .. " \u{00b7} here already"
        elseif not (getting[b.file] and getting[b.file].state == "going") then
          buttons[1] = { text = "Get", key = "get:" .. r.repo,
                         act = function() get(r.repo, b.file, b.size) end }
        end
      else
        about = r.why or "looking at its files\u{2026}"
      end

      rows[#rows + 1] = { title = r.repo, about = about, size = b and b.size, kind = b and b.quant,
                          buttons = buttons, dim = r.why ~= nil }
    end

    if #found == 0 and not searching then
      rows[#rows + 1] = { empty = "Nothing with GGUF files matches." }
    end
  end

  return rows
end

local function row_height(r)
  return (r.section and 40) or (r.empty and 36) or PAGE_ROW
end

function page:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  local BH = new_button.h
  local size_r, kind_x = self.w - 300, self.w - 270
  local y = 8 - page_scroll

  page_targets = {}

  g:text(24, y + 4, "Model", theme.text_dim, nil, "ui")
  g:text(size_r - gfx.measure("Size", "ui"), y + 4, "Size", theme.text_dim, nil, "ui")
  g:text(kind_x, y + 4, "Kind", theme.text_dim, nil, "ui")
  y = y + 28
  g:fill(16, y - 1, self.w - 32, 1, theme.line_soft)

  for _, r in ipairs(page_rows()) do
    local h = row_height(r)

    if r.section then
      g:text(24, y + 16, r.section, theme.text, nil, "ui")
    elseif r.empty then
      g:text(24, y + 10, r.empty, theme.text_dim, nil, "ui")
    elseif y + h >= 0 and y <= self.h then
      local tw = gfx.measure(r.title, "text")

      g:text(24, y + 9, ui.fitted(r.title, size_r - 120 - 24, "text"), r.dim and theme.text_dim or theme.text,
             nil, "text")

      -- The mark: a dot and its word, in the accent - "loaded" as the
      -- drawing has it; a dot drawn, since the face has no glyph for one.
      if r.mark then
        local mx = 24 + math.min(tw, size_r - 144) + 12
        local my = y + 9 + (TEXT_LH - UI_LH) // 2

        g:fill_round(mx, my + (UI_LH - 7) // 2, 7, 7, theme.accent, 3)
        g:text(mx + 12, my, r.mark, theme.accent, nil, "ui")
      end

      g:text(24, y + 11 + TEXT_LH, ui.fitted(r.about or "", size_r - 120 - 24, "ui"), theme.text_dim,
             nil, "ui")

      if r.bar then
        local bw = math.min(360, size_r - 160)

        g:fill_round(24, y + h - 10, bw, 4, theme.mix(theme.window, theme.text, 80), 2)
        g:fill_round(24, y + h - 10, math.max(4, math.floor(bw * r.bar)), 4, theme.accent, 2)
      end

      if r.size and r.size > 0 then
        local s = files.size(r.size)
        g:text(size_r - gfx.measure(s, "ui"), y + 9 + (TEXT_LH - UI_LH) // 2, s, theme.text, nil, "ui")
      end

      if r.kind and r.kind ~= "" then
        g:text(kind_x, y + 9 + (TEXT_LH - UI_LH) // 2, r.kind, theme.text, nil, "ui")
      end

      local bx = self.w - 24

      for i = #r.buttons, 1, -1 do
        local b = r.buttons[i]
        local bw = gfx.measure(b.text) + 28
        local by = y + (h - BH) // 2
        local held = self.click_key == b.key

        bx = bx - bw

        if b.go then
          g:fill_round(bx, by, bw, BH, held and theme.lift(theme.accent, -24) or theme.accent, 7)
          g:text(bx + 14, by + (BH - gfx.height()) // 2, b.text, theme.text_on, nil)
        else
          if held then g:sunken(bx, by, bw, BH, theme.raised) else g:raised(bx, by, bw, BH, theme.raised) end
          g:text(bx + 14 + (held and 1 or 0), by + (BH - gfx.height()) // 2 + (held and 1 or 0), b.text,
                 theme.text, theme.raised)
        end

        page_targets[#page_targets + 1] = { x = bx, y = by, w = bw, h = BH, key = b.key, act = b.act,
                                            text = b.text, row = r.title }
        bx = bx - 8
      end

      g:fill(16, y + h - 1, self.w - 32, 1, theme.line_soft)
    end

    y = y + h
  end

  self.content_h = y + page_scroll

  -- Where the buttons are, said when they change - for the display suite.
  local said_now = { ("Conversations back %d,%d"):format(page_head.x + back.x + back.w // 2,
                                                         page_head.y + back.y + back.h // 2) }

  for _, t in ipairs(page_targets) do
    said_now[#said_now + 1] = ("%s %s %d,%d"):format(t.row, t.text, self.x + t.x + t.w // 2,
                                                    self.y + t.y + t.h // 2)
  end

  said_now = table.concat(said_now, "; ")

  if said_now ~= self.said_buttons then
    self.said_buttons = said_now
    print("xavier: page buttons " .. said_now)
  end

  if page_said ~= "" then
    local w = gfx.measure(page_said, "ui") + 24

    g:fill_round((self.w - w) // 2, self.h - 40, w, 28, theme.mix(theme.window, theme.accent, 160), 14)
    g:text((self.w - w) // 2 + 12, self.h - 40 + (28 - UI_LH) // 2, page_said, theme.text, nil, "ui")
  end
end

function page:mouse(action, x, y)
  return ui.click(self, action, x, y, function(px, py)
    for _, t in ipairs(page_targets) do
      if px >= t.x and px < t.x + t.w and py >= t.y and py < t.y + t.h then return t.key, t.act end
    end

    return nil
  end)
end

function page:wheel(n)
  local most = math.max(0, (self.content_h or 0) - self.h + 40)

  page_scroll = math.max(0, math.min(most, page_scroll - n * ui.WHEEL_ROWS * TEXT_LH))
  win.dirty = true
  return true
end

function page_foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local n, bytes, loaded = 0, 0, nil

  for _, m in ipairs(here) do
    if not m.part then n, bytes = n + 1, bytes + m.size end
    if m.loaded then loaded = m end
  end

  local words = ("%d %s in %s \u{00b7} %s"):format(n, n == 1 and "model" or "models", llm.MODELS,
                loaded and ("one loaded, " .. files.size(loaded.size)) or "none loaded")
  local right = "models at the prompt does all of this too"
  local ty = (self.h - UI_LH) // 2

  g:text(16, ty, words, theme.text_dim, nil, "ui")
  g:text(self.w - 16 - gfx.measure(right, "ui"), ty, right, theme.text_dim, nil, "ui")
end

-- Four times a second while something is in flight: the reports read.
local followed = 0

local function page_step()
  if not (searching or next(getting)) or sys.ticks() - followed < hz // 4 then return false end

  followed = sys.ticks()

  local a = follow_search()
  local b = follow_gets()

  return a or b
end

--------------------------------------------------------------------------
-- The clock: the model watched until it is in, the answer read as it grows.
--------------------------------------------------------------------------

-- A load begun, without waiting; a model not there is not asked for again
-- until somebody chooses it.
local function begin_load(name)
  local got, why, r = llm.load(name, { wait = false })

  if not got then
    if not (r and r.error == 6) then refused[name] = true end
    said = ("Xavier could not load %s: %s"):format(short(name), tostring(why))
    print(("xavier: could not load %s: %s"):format(name, tostring(why)))
    return false
  end

  print("xavier: loading " .. name)
  model.state, model.name, model.loaded, model.size = "loading", name, 0, 0
  load_began = nil
  return true
end

local function watch_model()
  local s, why = llm.stats()

  if not s then
    model.state, said = "none", tostring(why)
    return
  end

  local want = wanted()
  local can = want ~= nil and not refused[want]

  if s.state == "empty" then
    if not want then
      model.state, said = "none", "There is no model in " .. llm.MODELS .. "."
    elseif not can or not begin_load(want) then
      model.state = "none"
    end
    return
  end

  if s.state == "loading" then
    model.state, model.name, model.loaded, model.size = "loading", s.model, s.loaded_bytes,
                                                         s.model_bytes
    if not load_began or load_began.name ~= s.model then
      load_began = { name = s.model, ticks = sys.ticks(), bytes = s.loaded_bytes }
    end
    return
  end

  -- In: this conversation's, or this conversation's is asked for - unless
  -- another application is being answered, which is waited out.
  if can and s.model ~= want then
    if s.state == "answering" then
      model.state = "looking"
      return
    end

    if begin_load(want) then return end
  end

  if model.state ~= "ready" then print("xavier: ready, " .. s.model) end

  model.state, model.name, model.arch, model.n_ctx = "ready", s.model, s.arch, s.n_ctx
  if said:match("^The model is still loading") then said = "" end
end

local placed = false

local function frame(self)
  if not placed then
    placed = true
    print(("xavier: places field %d,%d stop %d,%d new %d,%d list %d,%d rows %d more %d,%d chip %d,%d"):format(
      field.x + 20, field.y + field.h // 2, stop.x + stop.w // 2, stop.y + stop.h // 2,
      new_button.x + new_button.w // 2, new_button.y + new_button.h // 2, list.x, list.y, ROWH,
      header.x + more.x + more.w // 2, header.y + more.y + more.h // 2,
      header.x + chip.x + chip.w - 40, header.y + chip.y + chip.h // 2))
  end

  if compare then
    self.poll_wait_ticks = 8
    if compare.answer then compare_step() end
    return false
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
    local before = model_words() .. loading_words()

    if sys.ticks() - looked > hz // 2 then
      looked = sys.ticks()
      watch_model()
    end

    field.hint = field_hint()
    return model_words() .. loading_words() ~= before
  end

  if field.hint ~= field_hint() then
    field.hint = field_hint()
    return true
  end

  self.poll_wait_ticks = nil
  return false
end

-- A pass: the Models page's reports followed, and then the conversation's
-- own - each says whether it changed what is drawn.
function win:on_frame()
  local paged = page_step()
  local drawn = frame(self)

  if searching or next(getting) then
    self.poll_wait_ticks = math.min(self.poll_wait_ticks or 50, 50)
  end

  return drawn or paged
end

function win:on_resize(w, h)
  W, H = w, h
end

for _, v in ipairs({ side_ground, new_button, list, header, chat, field, stop, foot,
                     page_head, page, page_foot }) do
  win:add(v)
end

-- The conversation carried in from the box; else the newest, as it was
-- left; else a new one.
if all[1] then open = all[1] end

for _, c in ipairs(all) do
  if OPENING and c.path == OPENING then
    open = c
    print("xavier: carried in " .. c.path)
  end
end

watch_model()
refresh()
win:focus_on(field)
print(("xavier: %d conversations in %s"):format(#all, conversations.DIR))
say_opened(open)

win:run()
