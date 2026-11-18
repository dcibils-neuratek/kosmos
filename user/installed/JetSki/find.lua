-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki's find bar, one of its files (`jetski.lua`, *JetSki's files*):
-- a function of `J`, filling `J.F` - which the window's layout reads
-- before this is loaded - with the bar's view and `open`, `close`,
-- `paint` and `frame`.
--
-- **Find on the page** (`docs/webkit.html`, *Find on the page*, agreed 17
-- November 2026): Control-F opens a bar under the header - the Writer's,
-- `findbar.lua`, one bar for every application that looks for words - and
-- the page moves down to make room. WebKit marks every match, selects the
-- one shown and scrolls to it; the bar says how many and which, as WebKit
-- answers (`st.found`). Return the next, Shift-Return the one before,
-- Escape or the bar's cross to close it, which takes the marks off.
--
return function(J)
  local ui, keys, theme = J.ui, J.keys, J.theme
  local F = J.F
  local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
  local findbar = use("/Kosmos/Libraries/findbar.lua")
  findbar.into(ui, pk)
  F.H = findbar.H

  F.view = ui.view{ x = 0, y = J.HEAD, w = J.width(), h = F.H, hidden = true }
  F.view.focusable = true
  function F.view:draw() end

  local function look(fresh, backwards)
    if F.state.words == "" then F.state.count, F.state.current = 0, 0 end
    J.web.find(F.state.words, F.state.case, backwards, fresh)
    J.frame()
  end

  F.paint = function(s)
    local b = F.state.bar
    b.x, b.y, b.w = 0, J.HEAD, J.width()
    b.needle, b.count, b.current = F.state.words, math.max(0, F.state.count), F.state.current
    b.case, b.words, b.within, b.replacing = F.state.case, false, false, false
    b.only = { case = true }
    b.chosen = F.state.chosen
    b.focus = J.win.frame:focusables()[J.win.focus] == F.view and "find" or nil
    pk.findbar(s, b)
    if F.state.count < 0 and F.state.words ~= "" then        -- past the thousand
      local at = (b.parts or {})[1]
      if at then
        s:fill(at.box.x + at.box.w + 8, b.y + 4, 70, b.h - 8, theme.window)
        s:text(at.box.x + at.box.w + 8, b.y + (b.h - gfx.height()) // 2, "1000+", theme.text_dim, nil, "ui")
      end
    end
  end

  F.open = function()
    J.downloads.close()
    J.lock_close()
    J.shield_close()
    if not F.state then
      F.state = { words = "", case = false, count = 0, current = 0, bar = {} }
      F.view.hidden = false
      J.relaid()
    end
    -- The words already there chosen, so a word typed replaces them.
    F.state.chosen = F.state.words ~= ""
    J.web.focus(false)
    J.win:focus_on(F.view)
    print("jetski: find bar open")
    if F.state.words ~= "" then look(true, false) else J.frame() end
  end

  F.close = function()
    if not F.state then return end
    J.web.find("", false, false, false)
    F.state = nil
    F.view.hidden = true
    J.relaid()
    J.win:focus_on(J.view)
    J.web.focus(true)
    print("jetski: find bar closed")
  end

  local function step(backwards)
    if F.state and F.state.words ~= "" then look(false, backwards) end
  end

  function F.view:key(c)
    local k, mods = keys.parts(c)
    local chosen = F.state.chosen
    if c == 27 then
      F.close()
      return true
    elseif k == 13 and mods & keys.SHIFT ~= 0 then
      -- Shift-Return, once the board sends it as a key of its own; today it
      -- is Return's byte (`roadmap.md`).
      F.state.chosen = false
      step(true)
      return true
    elseif c == 13 or c == 10 then
      F.state.chosen = false
      step(false)
      return true
    elseif c == 8 or c == 127 then
      local t = F.state.words
      F.state.words = chosen and "" or t:sub(1, (utf8.offset(t, -1) or (#t + 1)) - 1)
      F.state.chosen = false
      look(true, false)
      return true
    elseif c >= 32 and c < 256 then
      F.state.words = (chosen and "" or F.state.words) .. string.char(c)
      F.state.chosen = false
      -- A character of several bytes is looked for once it is whole.
      if utf8.len(F.state.words) then look(true, false) end
      return true
    end
    return false                       -- the window's keys, Control-F among them
  end

  function F.view:mouse(action, x, y)
    if action ~= "press" then return true end
    local part = pk.findbar_at(F.state.bar, x + self.x, y + self.y)
    if part == "close" then
      F.close()
    elseif part == "next" then
      step(false)
    elseif part == "previous" then
      step(true)
    elseif part == "case" then
      F.state.case = not F.state.case
      look(true, false)
    end
    J.win:focus_on(F.view)
    if part then print("jetski: find " .. part) end
    return true
  end

  -- Every frame: WebKit's answer, drawn. What it says is said once, for a
  -- suite to read.
  F.frame = function()
    if not F.state or not J.st.found then return false end
    F.state.count, F.state.current = J.web.found()
    if F.state.count == 0 then
      print(("jetski: found nothing for %q"):format(F.state.words))
    else
      print(("jetski: found %d, showing %d"):format(F.state.count, F.state.current))
    end
    return true
  end

  J.win:add(F.view)
end
