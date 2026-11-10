-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Xavier from anywhere** (`docs/xavier.html`, *Asking from anywhere*):
-- Super+X opens a box over whatever is in front, as Super+Space opens the
-- launcher pad. Ask, read the answer as it is written, Escape, and you are
-- back where you were; Tab carries the conversation into Xavier's window.
--
-- It is Xavier's - `assistant --ask` - rather than an application of its
-- own, as Shortcuts over everything is `shortcuts --modal`: one entry in
-- the launcher, and the same conversations, kept in `/Home/Xavier` after
-- each answer as the window keeps them. A new conversation, so it begins
-- with the model chosen last, which is loaded when it is not the one in -
-- and a question asked while it comes in is asked when it is in.
--
-- A popup: no title bar, and the window manager closes it when a press
-- lands outside it. Started fresh each time, so there is nothing to hide
-- and no state to keep.

local ui = use("/Kosmos/Libraries/ui.lua")
local llm = use("/Kosmos/Libraries/llm.lua")
local conversations = use("/Kosmos/Libraries/conversations.lua")
local clock = use("/Kosmos/Libraries/clock.lua")

local box = {}

local W, H = 760, 340

local function short(name)
  local n = (name or ""):gsub("%.gguf$", "")
  n = n:gsub("%.([^.]*)$", " %1")
  return n
end

local function hex(s)
  return (s:gsub(".", function(ch) return ("%02x"):format(ch:byte()) end))
end

function box.run(tokens)
  local theme = ui.theme
  local screen = gfx.screen()
  local sw, sh = 1920, 1080
  if screen then sw, sh = screen:size() end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local x, y = (sw - W) // 2, sh // 5

  local win, err = ui.window{ title = "Xavier", w = W, h = H, x = x, y = y, popup = true }

  if not win then
    print("xavier box: " .. tostring(err))
    return
  end

  print(("xavier box: open at %d,%d %dx%d"):format(x, y, W, H))

  local conv = conversations.new()
  conv.model = conversations.chosen() or llm.choose() or ""

  local typed = ""
  local model = { state = "looking", name = "", arch = "", loaded = 0, size = 0 }
  local live = nil                       -- { turn, answer } while one is written
  local pending = nil                    -- a question asked while the model comes in
  local said = ""
  local looked = 0

  local TEXT_LH = gfx.height("text")
  local UI_LH = gfx.height("ui")
  local TITLE_LH = gfx.height("title")

  local view = ui.view{ x = 0, y = 0, w = W, h = H }
  view.focusable = true
  view.takes_tab = true

  -- The latest turn, as lines for the room there is: the question, then
  -- the answer as far as it has come.
  local function shown_lines(room)
    local t = conv.turns[#conv.turns]
    local out = {}

    if not t then return out end

    for _, l in ipairs(ui.wrapped(t.you, room, "text")) do out[#out + 1] = { s = l, dim = true } end
    out[#out + 1] = { gap = true }

    for raw in (t.xavier .. "\n"):gmatch("(.-)\n") do
      if raw:match("^%s*$") then
        if #out > 0 and not out[#out].gap then out[#out + 1] = { gap = true } end
      else
        for _, l in ipairs(ui.wrapped((raw:gsub("\t", "    ")), room, "text")) do
          out[#out + 1] = { s = l }
        end
      end
    end

    return out
  end

  local function model_line()
    if model.state == "loading" or model.state == "looking" then
      local name = model.state == "loading" and model.name or conv.model
      local pct = model.size > 0 and math.floor(model.loaded * 100 / model.size) or 0
      return ("Loading %s \u{00b7} %d%%\u{2026}"):format(short(name), pct)
    end

    return nil
  end

  function view:draw(g)
    g:fill(0, 0, self.w, self.h, theme.window)

    -- The question being typed, beside Xavier's mark.
    g:fill_round(20, 18, 30, 30, theme.mix(theme.accent, theme.window, 400), 15)
    g:text(20 + (30 - gfx.measure("X", "ui")) // 2, 18 + (30 - UI_LH) // 2, "X", theme.text, nil, "ui")

    local tx, ty = 64, 18 + (30 - TITLE_LH) // 2

    if typed == "" then
      g:text(tx, ty, live and "" or "Ask Xavier\u{2026}", theme.text_dim, nil, "title")
      g:fill(tx, ty, 2, TITLE_LH, theme.accent)
    else
      local shown = ui.fitted(typed, self.w - tx - 24, "title")
      g:text(tx, ty, shown, theme.text, nil, "title")
      g:fill(tx + gfx.measure(shown, "title") + 1, ty, 2, TITLE_LH, theme.accent)
    end

    g:fill(0, 64, self.w, 1, theme.line_soft)

    -- The answer, its end kept in view as it grows.
    local top, bottom = 78, self.h - 44
    local lines = shown_lines(self.w - tx - 24)
    local waiting = model_line()

    if pending then
      lines = { { s = pending, dim = true }, { gap = true }, { s = waiting or "Asking\u{2026}", dim = true } }
    elseif #lines == 0 then
      lines = { { s = said ~= "" and said or (waiting or ("Answered here, on this machine, by " ..
                  short(conv.model) .. ".")), dim = true } }
    end

    local h = 0
    for _, l in ipairs(lines) do h = h + (l.gap and TEXT_LH // 2 or TEXT_LH) end

    local ly = top - math.max(0, h - (bottom - top))

    for _, l in ipairs(lines) do
      local lh = l.gap and TEXT_LH // 2 or TEXT_LH

      if not l.gap and ly >= top - 1 and ly + lh <= bottom + 1 then
        g:text(tx, ly, l.s, l.dim and theme.text_dim or theme.text, nil, "text")
      end

      ly = ly + lh
    end

    -- What the keys do, and whether it is kept.
    g:fill(0, self.h - 36, self.w, 1, theme.line_soft)
    g:text(20, self.h - 36 + (36 - UI_LH) // 2,
           "Return ask \u{00b7} Tab open in Xavier \u{00b7} Esc close", theme.text_dim, nil, "ui")

    if conv.path then
      local kept = "a new conversation, kept"
      g:text(self.w - 20 - gfx.measure(kept, "ui"), self.h - 36 + (36 - UI_LH) // 2, kept,
             theme.text_dim, nil, "ui")
    end
  end

  local function save()
    local ok, why = conversations.save(conv, clock.stamp(clock.now() or clock.at(0)))

    if ok then
      print(("xavier box: kept %s, %d turns"):format(conv.path, #conv.turns))
    else
      said = "could not keep the conversation: " .. tostring(why)
    end
  end

  local function ask(question)
    local turns = {}

    for i = (conv.forgotten or 0) + 1, #conv.turns do turns[#turns + 1] = conv.turns[i] end

    local a, why, text
    local seed = llm.seed()

    while true do
      local stops
      text, stops = llm.chat(model.arch, turns, question, conversations.preamble())
      a, why = llm.ask(text, { tokens = tokens, stops = stops, seed = seed })

      if a or not llm.too_long(why) or #turns == 0 then break end

      table.remove(turns, 1)
      conv.forgotten = (conv.forgotten or 0) + 1
    end

    if not a then
      said = "Xavier could not ask: " .. tostring(why)
      return
    end

    if conv.created == 0 then conv.created = (clock.now() or {}).epoch or 0 end

    local t = { you = question, xavier = "", tokens = 0 }

    conv.turns[#conv.turns + 1] = t
    live = { turn = t, answer = a }
    said = ""
    print(("xavier box: asked %d bytes, drawn from %d: %s"):format(#text, seed, hex(text)))
  end

  local function finished()
    local t = live.turn
    local s = llm.stats()

    live = nil

    if s then
      t.tokens, t.read_us, t.write_us, t.first_us = s.answer_tokens, s.prompt_us, s.answer_us,
                                                    s.first_us
      t.model = s.model
      conv.held, conv.room, conv.model = s.held_tokens, s.n_ctx, s.model
      print(("xavier box: answered %d tokens, %d held: %s"):format(s.answer_tokens,
            s.held_tokens, hex(t.xavier)))
    end

    conv.updated = (clock.now() or {}).epoch or 0
    save()
  end

  -- The model: the conversation's, loaded when it is not the one in.
  local function watch_model()
    local s, why = llm.stats()

    if not s then
      model.state, said = "none", tostring(why)
      return
    end

    local want = conv.model ~= "" and conv.model or nil

    if s.state == "empty" or (s.state == "ready" and want and s.model ~= want) then
      if not want then
        model.state, said = "none", "There is no model in " .. llm.MODELS .. "."
        return
      end

      local got, refused = llm.load(want, { wait = false })

      if not got then
        model.state, said = "none", ("Xavier could not load %s: %s"):format(short(want), tostring(refused))
        return
      end

      print("xavier box: loading " .. want)
      model.state, model.name, model.loaded, model.size = "loading", want, 0, 0
      return
    end

    if s.state == "loading" then
      model.state, model.name, model.loaded, model.size = "loading", s.model, s.loaded_bytes,
                                                           s.model_bytes
      return
    end

    if s.state == "answering" then
      model.state = "looking"
      return
    end

    if model.state ~= "ready" then print("xavier box: ready, " .. s.model) end

    model.state, model.name, model.arch = "ready", s.model, s.arch
    conv.model = s.model
  end

  local function close()
    if live then live.answer:stop() end
    print("xavier box: closed")
    win:close()
  end

  -- Into the window: the conversation, when it has been kept; else the
  -- window as it opens.
  local function to_window()
    if live then return end

    fs.send("/Running/wm", { type = "launch", program = "assistant",
                             args = conv.path and ("--open " .. conv.path) or "" })
    print("xavier box: to the window, " .. (conv.path or "a new conversation"))
    win:close()
  end

  function view:key(c)
    if c == 27 then
      close()
    elseif c == 9 then
      to_window()
    elseif c == 10 or c == 13 then
      local q = typed:gsub("^%s+", ""):gsub("%s+$", "")

      if q ~= "" and not live and not pending then
        typed = ""

        if model.state == "ready" then ask(q) else pending = q end
      end
    elseif c == 8 or c == 127 then
      typed = typed:sub(1, (utf8.offset(typed, -1) or 1) - 1)
    elseif c >= 32 then
      typed = typed .. utf8.char(c)
    else
      return false
    end

    win.dirty = true
    return true
  end

  function win:on_frame()
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

      return grew
    end

    if model.state ~= "ready" and model.state ~= "none" then
      self.poll_wait_ticks = 50

      if sys.ticks() - looked > hz // 2 then
        looked = sys.ticks()
        local before = model_line()
        watch_model()

        if model.state == "ready" and pending then
          local q = pending
          pending = nil
          ask(q)
          return true
        end

        return model_line() ~= before or model.state ~= "loading"
      end

      return false
    end

    self.poll_wait_ticks = nil
    return false
  end

  win:add(view)
  win:focus_on(view)
  watch_model()
  win:run()
end

return box
