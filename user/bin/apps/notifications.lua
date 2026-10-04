-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Notifications
-- kosmos: section none
-- kosmos: needs screen
--
-- notifications: what applications have said, shown (`roadmap.md`,
-- *Notifications*, step 2; the drawing is `docs/notifications.html`).
--
--   notifications          the banners: started with the desktop
--   notifications panel    the history, from the clock in the strip
--
-- **Banners.** A process of its own rather than more of the Deskbar: it
-- asks the notification server four times a second what came after the
-- last number it saw, and shows each at the top right under the strip - a
-- card with who said it, when, the title and a line - for as long as the
-- person asked (five seconds unless they said otherwise), or until closed
-- when it is an alert. Several stack, newest on top, three at most; a
-- press on one opens what it names, the cross closes it. While nothing is
-- shown there is no window at all, and the asking is one call a quarter
-- of a second.
--
-- **The person's rules are applied here**, not by the server, which keeps
-- everything (`notifyproto.h`): Do Not Disturb holds every banner, and an
-- application turned off in Preferences shows none; both are still in the
-- history. They are `/Home/Preferences/notifications`, read when something
-- arrives - which is rarely - rather than on a clock.
--
-- **The window is a banner** (`ui.window{ banner = true }`): it takes a
-- press and never the keys, so a banner arriving while somebody types
-- takes nothing from them, and it is drawn in front of every window.
--
-- **The history** is a popup under the strip at the right, as a press on
-- the clock opens it: Do Not Disturb, Clear all, and everything kept,
-- grouped by who said it, newest first. A press outside closes it, which
-- is also how a second press on the clock does. **While it is open there
-- are no banners**, as the drawing has it: it tells the banners showing to
-- go (`/Running/Notifications/hide`), and the banners hold what comes while
-- it is there (`/Running/Notification history`) - it is in the history.

local ui     = use("/Kosmos/Libraries/ui.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
local menu   = use("/Kosmos/Libraries/deskbarmenu.lua")
local clock  = use("/Kosmos/Libraries/clock.lua")
local types  = use("/Kosmos/Libraries/filetypes.lua")
local theme  = ui.theme

local prefs  = use("/Kosmos/Libraries/prefs.lua")
local HISTORY = "Notification history"      -- its window's title, and /Running name

local mode = tostring(args or ""):match("^%s*(%S*)")

--------------------------------------------------------------------------
-- What both halves need.
--------------------------------------------------------------------------

-- The person's rules, with the drawing's defaults: banners for five
-- seconds, two hundred kept, nothing silenced.
local function rules()
  local r = prefs.read("notifications")

  return {
    dnd = r.dnd == true,
    seconds = tonumber(r.seconds) or 5,
    keep = tonumber(r.keep) or 200,
    off = type(r.off) == "table" and r.off or {},
  }
end

local function save_rule(key, value)
  return prefs.open("notifications"):set(key, value)
end

-- An application's name and picture, by the file it runs: what its header
-- declares (`kosmos: name`, `kosmos: icon`), asked once a file.
local known = {}

local function sender(e)
  local key = notify.key(e)
  local got = known[key]

  if got then return got end

  local attrs = (e.from ~= "") and fs.getattr(e.from) or nil
  local names = {}

  if attrs and attrs.title and attrs.title ~= "" then names[e.from] = attrs.title end

  local name = notify.who(e, names)

  got = { name = name,
          icon = (name == "System" or e.from == "") and "System_Kernel"
                 or (attrs and attrs.icon) or "App_Generic" }
  known[key] = got
  return got
end

-- "now", "3 min ago", or the time of day it came, as the drawing has them.
local function when(e)
  local age = notify.age(e)

  if age < 60 then return "now" end
  if age < 3600 then return ("%d min ago"):format(age // 60) end

  local t = clock.now()

  if not t then return ("%d h ago"):format(age // 3600) end

  local m = ((t.hour * 60 + t.min - age // 60) % 1440 + 1440) % 1440

  return ("%02d:%02d"):format(m // 60, m % 60)
end

-- What a press on one does: opens what it names as Tracker would - a
-- folder in a Tracker window, a file with what opens its type (`filetypes`)
-- - or nothing, when it names nothing. (It went through `open`, which starts
-- an application and refused a folder; the first notification it caused was
-- its own, "Open stopped".)
local function act(e)
  local path = e.open

  if not path or path == "" then return end

  local attrs = fs.getattr(path)
  local program = (attrs and attrs.kind == "directory") and "tracker"
                  or types.opener(path, attrs)

  if not program then
    print("notifications: nothing opens " .. path)
    return
  end

  fs.send("/Running/wm", { type = "launch", program = program, args = path })
  print(("notifications: opened %s with %s"):format(path, program))
end

-- `text` in lines no wider than `room` in `face`, at most `most` of them,
-- the last cut with an ellipsis when there was more.
local function wrap(text, room, face, most)
  local lines, line = {}, ""

  for word in tostring(text or ""):gmatch("%S+") do
    local try = (line == "") and word or (line .. " " .. word)

    if gfx.measure(try, face) <= room or line == "" then
      line = try
    else
      lines[#lines + 1] = line
      line = word
    end
  end

  if line ~= "" then lines[#lines + 1] = line end

  if #lines > most then
    local cut = { table.unpack(lines, 1, most) }
    cut[most] = ui.fitted(cut[most] .. " " .. lines[most + 1], room, face)
    return cut
  end

  for i, l in ipairs(lines) do
    if gfx.measure(l, face) > room then lines[i] = ui.fitted(l, room, face) end
  end

  return lines
end

local SMALL = ui.sized("ui", 13)
local BODY  = ui.sized("ui", 14)
local TITLE = ui.sized("ui", 15, "bold")

-- The drawing's surfaces for Night, and the look's own otherwise: a card a
-- shade lighter than a window, its hairline, the muted ink.
local function palette()
  return {
    card  = theme.raised or 0xff2a3344,
    line  = 0x30ffffff,
    ink   = theme.text,
    muted = theme.text_dim,
    alert = 0x8cff8c78,
    x     = theme.sunken or 0xff3a4558,
  }
end

--------------------------------------------------------------------------
-- The banners.
--------------------------------------------------------------------------

local CARD_W  = 372
local PAD_X, PAD_Y = 14, 12
local ICON    = 40
local GAP     = 10
local EDGE    = 8          -- room round the cards for the cross and the shadow
local SHOWN   = 3
local R       = 20

local function card_h(e)
  local text_w = CARD_W - 2 * PAD_X - ICON - 12
  local lines = (e.body ~= "") and #wrap(e.body, text_w, BODY, 2) or 0

  return PAD_Y + gfx.height(SMALL) + 2 + gfx.height(TITLE)
         + lines * (gfx.height(BODY) + 2) + PAD_Y
end

local function draw_card(g, x, y, e, p, back)
  local who = sender(e)
  local h = card_h(e)
  local text_x = x + PAD_X + ICON + 12
  local text_w = CARD_W - (text_x - x) - PAD_X

  -- A soft shadow under it, the hairline round it, the card.
  g:fill_round(x, y + 6, CARD_W, h, 0x22000000, R)
  g:fill_round(x - 1, y - 1, CARD_W + 2, h + 2, e.alert and p.alert or p.line, R + 1)
  g:fill_round(x, y, CARD_W, h, p.card, R)

  g:icon(x + PAD_X, y + PAD_Y, who.icon .. ".png", ICON)

  local ty = y + PAD_Y
  local age = when(e)

  g:text(text_x, ty, who.name, p.ink, p.card, SMALL)
  g:text(x + CARD_W - PAD_X - gfx.measure(age, SMALL), ty, age, p.muted, p.card, SMALL)
  ty = ty + gfx.height(SMALL) + 2

  g:text(text_x, ty, ui.fitted(e.title, text_w, TITLE), p.ink, p.card, TITLE)
  ty = ty + gfx.height(TITLE)

  if e.body ~= "" then
    for _, line in ipairs(wrap(e.body, text_w, BODY, 2)) do
      g:text(text_x, ty, line, p.muted, p.card, BODY)
      ty = ty + gfx.height(BODY) + 2
    end
  end

  -- The cross at its top left corner, half over the edge, as the drawing.
  g:fill_round(x - 7, y - 7, 22, 22, p.line, 11)
  g:fill_round(x - 6, y - 6, 20, 20, p.x, 10)
  g:line_icon(x - 6 + 2, y - 6 + 2, "close", p.ink, 15)

  return h
end

local function banners()
  local seen = notify.newest()
  local stack = {}             -- { e = entry, ends = counter or nil }, newest first
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local tick_hz = (sys.info() or {}).tick_hz or 100
  local kept = nil
  local win = nil

  print(("notifications: banners from %d on"):format(seen))

  -- Where the stack goes: the top right of what windows may cover, under
  -- the strip or the bar.
  local function corner(w)
    local wa = fs.send("/Running/wm", { type = "workarea" }) or {}
    local screen = gfx.screen()
    local sw = screen and (screen:size()) or 1920
    local top = tonumber(wa.y) or 32

    return sw - 16 + EDGE - w, top + 10 - EDGE
  end

  local function stack_h()
    local h = 2 * EDGE

    for i, s in ipairs(stack) do
      h = h + card_h(s.e) + ((i > 1) and GAP or 0)
    end

    return h
  end

  -- Asked of the server: what came after the last seen, each through the
  -- person's rules. True when the stack changed.
  local function arrived()
    local changed = false
    local r = nil

    while true do
      local e = notify.next(seen)

      if not e then break end

      seen = e.id
      r = r or rules()

      if kept ~= r.keep then
        kept = r.keep
        notify.keep(kept)
      end

      if r.dnd then
        print(("notifications: %d held, Do Not Disturb"):format(e.id))
      elseif fs.read("/Running/" .. HISTORY .. "/title") then
        print(("notifications: %d held, the history is open"):format(e.id))
      elseif r.off[notify.key(e)] then
        print(("notifications: %d held, %s is off"):format(e.id, notify.key(e)))
      else
        table.insert(stack, 1, { e = e, seconds = (not e.alert) and r.seconds or nil })
        print(("notifications: banner %d, %s"):format(e.id, e.title))
        changed = true
      end
    end

    while #stack > SHOWN do
      table.remove(stack)
      changed = true
    end

    return changed
  end

  -- Counted from when it is on the screen, not from when it came: a
  -- window takes a moment to open, and five seconds is how long it is seen.
  local function expired()
    local now, changed = sys.ticks(), false

    for i = #stack, 1, -1 do
      if stack[i].seconds and not stack[i].ends then
        stack[i].ends = now + stack[i].seconds * hz
      end

      if stack[i].ends and now >= stack[i].ends then
        table.remove(stack, i)
        changed = true
      end
    end

    return changed
  end

  local view

  local function fit()
    local h = stack_h()
    local x, y = corner(CARD_W + 2 * EDGE)

    if win then
      win:resize(CARD_W + 2 * EDGE, h)
      win:move(x, y)
      view:resize(CARD_W + 2 * EDGE, h)
      win.dirty = true
    end
  end

  -- The card a press at `y` landed on, and whether it was on its cross.
  local function hit(x, y)
    local cy = EDGE

    for i, s in ipairs(stack) do
      local h = card_h(s.e)

      if y >= cy - 7 and y < cy + h then
        local on_x = (x >= EDGE - 7 and x < EDGE + 15 and y < cy + 15)
        return i, on_x
      end

      cy = cy + h + GAP
    end

    return nil
  end

  local function open_window()
    local w, h = CARD_W + 2 * EDGE, stack_h()
    local x, y = corner(w)

    -- `hide`, for the history to say it is opening: every banner goes.
    win = ui.window{ title = "Notifications", w = w, h = h, x = x, y = y, banner = true,
                     properties = { hide = {
                       get = function() return tostring(#stack) end,
                       set = function()
                         for i = #stack, 1, -1 do table.remove(stack, i) end
                         print("notifications: banners hidden for the history")
                       end } } }

    if not win then
      print("notifications: no banner window")
      stack = {}
      return
    end

    print(("notifications: banners at %d,%d %dx%d"):format(x, y, w, h))

    view = ui.view{ x = 0, y = 0, w = w, h = h }

    function view:draw(g)
      local p = palette()

      g:fill(0, 0, self.w, self.h, 0x00000000)

      local cy = EDGE

      for _, s in ipairs(stack) do
        cy = cy + draw_card(g, EDGE, cy, s.e, p) + GAP
      end
    end

    function view:mouse(action, x, y)
      if action ~= "press" then return false end

      local i, on_x = hit(x, y)

      if not i then return false end

      local s = table.remove(stack, i)

      if on_x then
        print(("notifications: closed %d"):format(s.e.id))
      else
        act(s.e)
      end

      if #stack == 0 then win:close() else fit() end

      return true
    end

    win:add(view)

    -- Four times a second: what came, and what has had its time.
    win.tick_every = hz // 4
    win.on_frame = function()
      local changed = arrived()

      if expired() then changed = true end

      if #stack == 0 then
        win:close()
        return false
      end

      if changed then fit() end

      -- Its age moves once a minute; repainting each second keeps "now"
      -- honest without a clock of its own.
      return changed
    end

    win:run()
    win = nil
  end

  while true do
    arrived()

    if #stack > 0 then open_window() end

    sys.sleep(math.max(1, tick_hz // 4))
  end
end

--------------------------------------------------------------------------
-- The history.
--------------------------------------------------------------------------

local function history()
  local PW = 396
  local wa = fs.send("/Running/wm", { type = "workarea" }) or {}
  local screen = gfx.screen()
  local sw, sh = 1920, 1080

  if screen then sw, sh = screen:size() end

  local top = (tonumber(wa.y) or 32) + 8
  local ph = math.max(240, (tonumber(wa.h) or (sh - 40)) - 16)
  local px = sw - 12 - PW

  local win = ui.window{ title = HISTORY, w = PW, h = ph, x = px, y = top,
                         popup = true }

  -- No banners while it is open: those showing go.
  fs.write("/Running/Notifications/hide", "1")

  if not win then
    print("notifications: no history window")
    return
  end

  local r = rules()
  local all = notify.all(0)
  local groups, scroll, spans = {}, 0, {}

  local function group()
    local by = {}

    groups = {}

    for i = #all, 1, -1 do
      local e = all[i]
      local key = notify.key(e)

      if not r.off[key] then
        local g_ = by[key]

        if not g_ then
          g_ = { key = key, items = {}, who = sender(e) }
          by[key] = g_
          groups[#groups + 1] = g_
        end

        g_.items[#g_.items + 1] = e
      end
    end
  end

  group()

  print(("notifications: the history at %d,%d %dx%d, %d kept in %d groups"):format(
        px, top, PW, ph, #all, #groups))

  local HEAD = ui.sized("ui", 18, "bold")
  local P = 14
  local ITEM_W = PW - 2 * P - 24

  local view = ui.view{ x = 0, y = 0, w = PW, h = ph }

  function view:draw(g)
    local p = palette()
    local face = theme.window

    spans = {}
    g:fill(0, 0, self.w, self.h, face)

    -- The heading, and Clear all.
    g:text(P + 6, P, "Notifications", p.ink, face, HEAD)

    local clear = "Clear all"
    local cx = self.w - P - 6 - gfx.measure(clear, SMALL)

    g:text(cx, P + 4, clear, theme.accent, face, SMALL)
    spans[#spans + 1] = { x = cx - 6, y = P, w = gfx.measure(clear, SMALL) + 12, h = 26,
                          what = "clear" }

    -- Do Not Disturb: a row of its own, the switch at its end.
    local dy = P + 36
    local dh = 52

    g:fill_round(P, dy, self.w - 2 * P, dh, p.card, 18)
    g:fill_round(P + 12, dy + 11, 30, 30, r.dnd and theme.accent or 0xff4a5468, 15)
    g:line_icon(P + 12 + 6, dy + 11 + 6, "moon", 0xffffffff, 19)
    g:text(P + 54, dy + 8, "Do Not Disturb", p.ink, p.card, TITLE)
    g:text(P + 54, dy + 8 + gfx.height(TITLE),
           r.dnd and "Banners held; everything still kept here" or "Banners shown as they come",
           p.muted, p.card, SMALL)

    local tw, th = 44, 24
    local tx, ty = self.w - P - 12 - tw, dy + (dh - th) // 2

    g:fill_round(tx, ty, tw, th, r.dnd and theme.accent or (theme.track or 0xff4a5468), th // 2)
    g:fill_round(r.dnd and (tx + tw - th + 2) or (tx + 2), ty + 2, th - 4, th - 4, 0xffffffff, (th - 4) // 2)
    spans[#spans + 1] = { x = P, y = dy, w = self.w - 2 * P, h = dh, what = "dnd" }

    -- Everything kept, by who said it.
    local y = dy + dh + 12 - scroll
    local bottom = self.h - P

    if #groups == 0 then
      g:text(P + 6, y + 6, "Nothing has been said", p.muted, face, BODY)
    end

    for _, gr in ipairs(groups) do
      local h = 10 + 22
      local rows = {}

      for _, e in ipairs(gr.items) do
        local lines = (e.body ~= "") and wrap(e.body, ITEM_W, BODY, 2) or {}
        local rh = 6 + gfx.height(TITLE) + #lines * (gfx.height(BODY) + 1) + 6

        rows[#rows + 1] = { e = e, lines = lines, h = rh }
        h = h + rh
      end

      h = h + 6

      if y + h > 0 and y < bottom then
        g:fill_round(P, y, self.w - 2 * P, h, p.card, 18)
        g:icon(P + 12, y + 10, gr.who.icon .. ".png", 20)
        g:text(P + 40, y + 12, gr.who.name, p.muted, p.card, SMALL)

        local n = tostring(#gr.items)

        g:text(self.w - P - 12 - gfx.measure(n, SMALL), y + 12, n, p.muted, p.card, SMALL)

        local ry = y + 10 + 22

        for i, row in ipairs(rows) do
          if i > 1 then g:fill(P + 12, ry, self.w - 2 * P - 24, 1, p.line) end

          local age = when(row.e)

          g:text(P + 12, ry + 6, ui.fitted(row.e.title, ITEM_W - gfx.measure(age, SMALL) - 8, TITLE),
                 p.ink, p.card, TITLE)
          g:text(self.w - P - 12 - gfx.measure(age, SMALL), ry + 8, age, p.muted, p.card, SMALL)

          local ly = ry + 6 + gfx.height(TITLE)

          for _, l in ipairs(row.lines) do
            g:text(P + 12, ly, l, p.muted, p.card, BODY)
            ly = ly + gfx.height(BODY) + 1
          end

          spans[#spans + 1] = { x = P, y = ry, w = self.w - 2 * P, h = row.h, what = "item", e = row.e }
          ry = ry + row.h
        end
      end

      y = y + h + 10
    end

    self.content_h = y + scroll
  end

  function view:wheel(n)
    local most = math.max(0, (self.content_h or 0) - self.h + P)

    scroll = math.max(0, math.min(most, scroll + (n or 0) * 40))
    win.dirty = true
    return true
  end

  function view:mouse(action, x, y)
    if action ~= "press" then return false end

    for _, s in ipairs(spans) do
      if x >= s.x and x < s.x + s.w and y >= s.y and y < s.y + s.h then
        if s.what == "clear" then
          notify.clear()
          all = {}
          group()
          print("notifications: the history cleared")
          win.dirty = true
        elseif s.what == "dnd" then
          r.dnd = not r.dnd
          save_rule("dnd", r.dnd)
          print("notifications: Do Not Disturb " .. (r.dnd and "on" or "off"))
          win.dirty = true
        elseif s.what == "item" then
          act(s.e)
          win:close()
        end

        return true
      end
    end

    return false
  end

  function view:key(c)
    if c == 27 then win:close() return true end
    return false
  end

  view.focusable = true
  win:add(view)
  win:focus_on(view)
  win:run()
end

if mode == "panel" then
  history()
else
  banners()
end
