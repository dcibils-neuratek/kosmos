-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Deskbar
-- kosmos: name Menu bar
-- kosmos: needs screen network
-- Pewter's menu bar: one strip across the top of the screen for every
-- application (`docs/pewter.md` P3).
--
--   wm Pewter.desktop    starts this, as Astra's file starts the Deskbar
--
-- The Kosmos menu at its left; then the menus of whichever application is
-- in front; then, from the right, the Application menu - what is running,
-- to bring forward, hide or show - the clock, and what the machine is
-- doing: the sound, the network, the battery.
--
-- **The menus are the application's, not this bar's.** Under `menus =
-- screen` the kit hands the window manager each window's titles - only
-- the titles - and the window manager says here, in the `windows` reply's
-- `front`, which application is in front and what it said. A press on a
-- title is a `menu_open` to the window manager, which posts the window
-- the `menubar` event a direct window's strip has always sent, and the
-- application opens its own menu under the title, its items run where
-- they were written. Nothing here knows what is in any of them.
--
-- **Nothing waits** (*Nothing on the desktop waits on a server*). The bar
-- draws what it was last told, is told again when the front or its
-- titles change (`watch`), and asks the indicators' servers on its own
-- second, never in a repaint or a press. An application that has stopped
-- answering still shows its titles; a press opens nothing and holds
-- nobody.
--
-- The indicators and the Kosmos menu are the Deskbar's, moved into
-- `indicators.lua` and `kosmosmenu.lua` for both bars rather than copied.

local ui = use("/Kosmos/Libraries/ui.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local kosmosmenu = use("/Kosmos/Libraries/kosmosmenu.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local indicators = use("/Kosmos/Libraries/indicators.lua").new("menubar")

-- The kit's palette, the one it changes in place when the look changes.
local theme = ui.theme
local heard = indicators.heard

local screen = gfx.screen()
local W = screen and (screen:size()) or 1024

-- A glyph and ten pixels of air: Mac OS's twenty at its font, here at ours.
local H = gfx.height("ui") + 10

-- The indicators' line glyphs, at the size the kit's icon buttons use.
local LINE = 15

-- Room between one thing and the next, and inside a title.
local PAD, IN = 14, 10

local function say(text)
  print("menubar: " .. tostring(text))
end

-- `keeps_front`: a press here leaves the application in front, its keys
-- and its boxes with it - the menus this bar opens are its.
local win, err = ui.window{ title = "Menu bar", w = W, h = H, strip = "top",
                            keeps_front = true }

if not win then
  say(tostring(err))
  return
end

-- The Kosmos menu's sections, read once - about fifty messages to the disk
-- server - and again when told, as the Deskbar is: `setprop "/Running/Menu
-- bar/menu" reload`.
local sections = kosmosmenu.sections()

win:publish("menu",
  function() return #sections .. " sections" end,
  function(value)
    if value == "reload" then sections = kosmosmenu.sections() end
    return true
  end)

--------------------------------------------------------------------------
-- What the window manager last said: the application in front and its
-- menus' titles, and what is running.
--------------------------------------------------------------------------

local front = nil      -- { handle, title, program, menus }
local running = {}     -- the windows that are applications, by handle

local function front_key(f)
  if not f then return "" end

  return tostring(f.handle) .. "\t" .. tostring(f.title) .. "\t"
         .. table.concat(f.menus or {}, "\t")
end

local function refresh()
  local reply = wmproto.windows(win.handle)

  if not reply then return false end

  local list = {}

  for _, w_ in ipairs(reply.windows) do
    if not w_.chrome then list[#list + 1] = w_ end
  end

  table.sort(list, function(a, b) return a.handle < b.handle end)

  local was = front_key(front)

  front, running = reply.front, list

  if front_key(front) ~= was then
    -- Said, as the Deskbar says what it shows, for whoever reads the log -
    -- the harness finds the titles by it.
    say(front and ("%s in front, its menus %s"):format(tostring(front.title),
                    front.menus and table.concat(front.menus, ", ") or "none")
        or "nothing in front")
  end

  return true
end

--------------------------------------------------------------------------
-- The bar.
--------------------------------------------------------------------------

local bar = ui.view{ x = 0, y = 0, w = W, h = H }

-- Where each thing on the bar is, worked out by the draw and read by the
-- press, so the two cannot disagree: `{ x, w, kind, index }`.
bar.spans = {}

-- Which of this bar's own menus is open - the Kosmos menu or the
-- Application menu - lit while it is; derived from the window's open
-- menus, so a menu closed by a press elsewhere unlights it.
bar.lit = nil

local function title_ink() return theme.text end

function bar:draw(g)
  local top, bottom = theme.chrome(theme.raised)

  theme.vgradient(g, 0, 0, self.w, self.h - 1, top, bottom, 0, self.h - 1)
  g:fill(0, self.h - 1, self.w, 1, theme.edge_dark)

  local ty = (self.h - 1 - gfx.height("ui")) // 2
  local tty = (self.h - 1 - gfx.height("title")) // 2
  local ly = (self.h - 1 - LINE) // 2
  local spans = {}

  if #win.menus == 0 then self.lit = nil end

  local function lit(kind, x, w)
    if self.lit == kind then g:fill(x, 1, w, self.h - 3, theme.accent) end

    return self.lit == kind and theme.text_on or title_ink()
  end

  -- The Kosmos menu, in the title face, at the left.
  local x = 4
  local kw = gfx.measure("Kosmos", "title") + 2 * IN

  g:text(x + IN, tty, "Kosmos", lit("kosmos", x, kw), nil, "title")
  spans[#spans + 1] = { x = x, w = kw, kind = "kosmos" }
  x = x + kw

  -- The front application's menus, as it said them.
  for i, words in ipairs(front and front.menus or {}) do
    local w = gfx.measure(words) + 2 * IN

    g:text(x + IN, ty, words, title_ink())
    spans[#spans + 1] = { x = x, w = w, kind = "title", index = i }
    x = x + w
  end

  local left_end = x

  -- From the right: the Application menu, the clock, the indicators.
  x = self.w - 4

  local app = front and tostring(front.title) or "Kosmos"

  app = ui.fitted(app, 180)

  local aw = gfx.measure(app, "title") + 2 * IN

  x = x - aw
  g:text(x + IN, tty, app, lit("application", x, aw), nil, "title")
  spans[#spans + 1] = { x = x, w = aw, kind = "application" }

  local time = heard.now and clock.time_string(heard.now) or ""

  if time ~= "" then
    local cw = gfx.measure(time) + 2 * IN

    x = x - PAD - cw
    g:text(x + IN, ty, time, title_ink())

    if indicators.news() then
      g:fill_round(x + 2, (self.h - 6) // 2, 6, 6, theme.accent, 3)
    end

    spans[#spans + 1] = { x = x, w = cw, kind = "clock" }
  end

  local bat = heard.battery

  if bat then
    local label = ("%d%%"):format(bat.percent)
    local low = bat.critical == 1 or (bat.state == "discharging" and bat.percent <= 10)
    local ink = low and (theme.bad or 0xffe04848) or title_ink()

    x = x - PAD - LINE - 4 - gfx.measure(label)
    g:line_icon(x, ly, indicators.battery_glyph(bat), ink, LINE)
    g:text(x + LINE + 4, ty, label, ink)
  end

  if heard.network then
    x = x - PAD - LINE
    g:line_icon(x, ly, heard.network, title_ink(), LINE)
    spans[#spans + 1] = { x = x - 4, w = LINE + 8, kind = "network" }
  end

  if heard.level then
    x = x - PAD - LINE
    g:line_icon(x, ly, heard.muted and "muted" or "sound", title_ink(), LINE)
    spans[#spans + 1] = { x = x - 4, w = LINE + 8, kind = "volume" }
  end

  -- Menus that would run under the indicators are not drawn over them; a
  -- bar with that many titles is an application asking too much of it.
  if left_end > x then
    say(("the menus run %d pixels under the indicators"):format(left_end - x))
  end

  self.spans = spans

  -- Where each title is, said once a front: a harness presses them by it.
  local where = {}

  for _, sp in ipairs(spans) do
    if sp.kind == "title" then
      where[#where + 1] = ("%s %d,%d"):format(front.menus[sp.index],
                                             sp.x + sp.w // 2, self.h // 2)
    end
  end

  for _, sp in ipairs(spans) do
    if sp.kind == "kosmos" or sp.kind == "application" then
      where[#where + 1] = ("the %s menu %d,%d"):format(
        sp.kind == "kosmos" and "Kosmos" or "Application", sp.x + sp.w // 2, self.h // 2)
    end
  end

  where = table.concat(where, ", ")

  if where ~= self.said then
    self.said = where
    say("at " .. where)
  end
end

--------------------------------------------------------------------------
-- What a press does.
--------------------------------------------------------------------------

local HOW = { say = say }

local function launch(program)
  fs.send("/Running/wm", { type = "launch", program = program, wait = false })
end

-- What is running, to bring forward; then hide the one in front, or show
-- every one put away.
local function application_items()
  local items = {}

  for _, w_ in ipairs(running) do
    items[#items + 1] = {
      text = tostring(w_.title),
      mark = (front and w_.handle == front.handle) or false,
      on_choose = function()
        fs.send("/Running/wm", { type = "raise", window = w_.handle })
      end,
    }
  end

  if #items == 0 then items[1] = { text = "(nothing running)" } end

  items[#items + 1] = { separator = true }

  items[#items + 1] = {
    text = "Hide " .. (front and tostring(front.title) or "the front window"),
    disabled = not front or nil,
    on_choose = function()
      if front then fs.send("/Running/wm", { type = "minimise", window = front.handle }) end
    end,
  }

  local any_hidden = false

  for _, w_ in ipairs(running) do if w_.hidden then any_hidden = true end end

  items[#items + 1] = {
    text = "Show All",
    disabled = not any_hidden or nil,
    on_choose = function()
      for _, w_ in ipairs(running) do
        if w_.hidden then fs.send("/Running/wm", { type = "raise", window = w_.handle }) end
      end
    end,
  }

  return items
end

local function open_kosmos_menu()
  bar.lit = "kosmos"
  win:open_menu(win.origin_x + 4, win.origin_y + H, kosmosmenu.items(sections, HOW))
end

function bar:mouse(action, x, _)
  if action ~= "press" then return true end

  local at = sys.ticks()

  for _, sp in ipairs(self.spans) do
    if x >= sp.x and x < sp.x + sp.w then
      if sp.kind == "kosmos" then
        open_kosmos_menu()
      elseif sp.kind == "title" and front then
        -- Posted on, never waited for: the window opens its own menu.
        local reply = fs.send("/Running/wm", { type = "menu_open", window = front.handle,
                                               index = sp.index,
                                               x = win.origin_x + sp.x,
                                               y = win.origin_y + H, at = at })

        if not (reply and reply.ok) then
          say(("%s's menu %d: %s"):format(tostring(front.title), sp.index,
              tostring(reply and reply.error or "no answer")))
        end
      elseif sp.kind == "application" then
        self.lit = "application"
        win:open_menu(win.origin_x + sp.x + sp.w - 220, win.origin_y + H,
                      application_items())
      elseif sp.kind == "clock" then
        indicators.open_history()
      elseif sp.kind == "volume" then
        launch("/Kosmos/Apps/mixer.lua")
      elseif sp.kind == "network" then
        launch("/Kosmos/Apps/network.lua")
      end

      return true
    end
  end

  return true
end

win:add(bar)

--
-- The Super key's menu, posted by the window manager to the bar at the top
-- (`OUT.open_kosmos_menu`), and the window manager saying the front or its
-- menus changed, which it posts because `refresh` asks with `watch`.
--
win.on_event = function(_, ev)
  if ev.type == "menu" then
    open_kosmos_menu()
    return true
  end

  if ev.type == "windows" then
    return refresh()
  end

  return false
end

-- The clock and the indicators, asked on the bar's own second - which is
-- also what puts the window on the kit's once-a-second repaint.
local clockwork = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function clockwork:tick()
  indicators.listen()
end

win:add(clockwork)

refresh()
say("up")
win:run()
