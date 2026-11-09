-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Backgrounds
-- kosmos: name Desktops
-- kosmos: section none
--
-- chooser: which desktop to start (`docs/pewter.md` P1, drawn in
-- `docs/desktops.html`, agreed 7 November 2026), and the loader over it.
--
--   chooser            the cards: Switch Desktop starts this
--   chooser --start    no cards: the desktop the `desktop` setting names,
--                      started at once with the loader - how the machine
--                      boots since 8 November, and Preferences' Desktop row
--
-- The whole screen, one card for each file in `/Kosmos/Desktops`, the one
-- used last chosen already. Left and right, Return; or a click on a card,
-- and a second on the chosen one. Restart and Shut Down beneath, for a
-- machine nobody wants a desktop on.
--
-- **And then the loader, which is this window kept over the screen.** The
-- window manager starts what the desktop's file names and then the Startup
-- page's items, and posts this window a `loading` event as each one starts
-- and as it arrives - its first window drawn, or running when it opens
-- none. Nothing here asks: what is drawn is what was posted. When every
-- row has settled, or when nothing has moved for a second and a half -
-- the desktop does not wait for a slow one - the window goes, and the
-- desktop is what is underneath it.
--
-- A program rather than a part of the window manager, because it is a
-- window like any other, drawn by the kit, and a desktop whose chooser
-- went wrong is a chooser that died rather than a window manager that did.
--

local ui       = use("/Kosmos/Libraries/ui.lua")
local desktops = use("/Kosmos/Libraries/desktops.lua")

-- Its own colours, which are no desktop's: the chooser comes before a
-- look has been chosen, and is the same whichever is chosen after it.
local C = {
  ground   = 0xff10131c,
  card     = 0xff1e2433,
  line     = 0xff2e364a,
  sel      = 0xff7fb0ff,
  glow     = 0xff2a3b5c,
  ink      = 0xffeef0f6,
  dim      = 0xff9aa1b3,
  foot     = 0xff79809a,
  well     = 0xff262d3f,
  good     = 0xff7ed49a,
  bad      = 0xffff8f84,
  slow     = 0xfff0c36a,
}

local W, H = gfx.screen():size()

local CARD_W, CARD_H, GAP = 300, 236, 26
local PIC_H = 150
local QUIET = 1.5                -- seconds with nothing moving: the loader ends
local LINGER = 0.7               -- seconds the finished loader stays

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local version = (sys.build() or {}).version or ""

--------------------------------------------------------------------------
-- What there is to choose.
--------------------------------------------------------------------------

local list = {}

for _, name in ipairs(desktops.list()) do
  local d, why = desktops.read(name)

  if d then
    list[#list + 1] = d
  else
    print(("chooser: %s.desktop is not offered: %s"):format(name, tostring(why)))
  end
end

local last = desktops.chosen()
local sel = 1

for i, d in ipairs(list) do
  if d.name == last then sel = i end
end

print(("chooser: %d desktop%s, %s chosen last"):format(#list, #list == 1 and "" or "s", last))

-- **No question asked** when started to start: the one chosen, if it is
-- still there - a desktop file taken away leaves the cards to choose from.
local START = tostring(args or ""):match("%-%-start") ~= nil

if START and not (list[sel] and list[sel].name == last) then
  print(("chooser: %s is not here to start, so the cards"):format(last))
  START = false
end

--------------------------------------------------------------------------
-- The loader's state: nil until a desktop is chosen.
--------------------------------------------------------------------------

local load = nil       -- { name, rows = { [i] = row }, changed, done, leave }

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

local function centred(g, y, s, colour, role, px)
  local face = ui.sized(role or "ui", px)
  g:text((W - gfx.measure(s, face)) // 2, y, s, colour, C.ground, role, px)
end

-- Each card's place, left to right, centred.
local function card_at(i)
  local n = #list
  local total = n * CARD_W + (n - 1) * GAP
  local x0 = (W - total) // 2

  return x0 + (i - 1) * (CARD_W + GAP), H // 2 - CARD_H // 2
end

-- A picture of each desktop, drawn rather than shipped: what is across the
-- top, and two windows. Pewter's has its menu bar and the Control Strip.
local function picture(g, x, y, w, h, d)
  local pewter = d.name == "Pewter"

  g:fill_round(x, y, w, h, pewter and 0xff5d6b7e or 0xff2b3550, 8)

  if pewter then
    g:fill(x, y, w, 14, 0xffdedede)
    g:fill(x + 8, y + 3, 8, 8, 0xff3c74c8)
  else
    g:fill(x, y, w, 14, 0xff171c29)
    for k = 0, 3 do g:fill(x + 8 + k * 30, y + 4, 22, 6, 0xff3a4560) end
  end

  local function window_(wx, wy, ww, wh)
    g:fill(x + wx, y + wy, ww, wh, pewter and 0xffe6e6e6 or 0xff222a3c)
    g:fill(x + wx, y + wy, ww, 12, pewter and 0xffbdbdbd or 0xff30394f)
  end

  window_(16, 26, 150, 98)
  window_(118, 54, 136, 88)

  if pewter then
    g:fill(x, y + h - 14, 64, 10, 0xffbdbdbd)
  end
end

local STATE = {
  starting = { "starting", C.dim },
  ready    = { "ready", C.good },
  ended    = { "ready", C.good },
  failed   = { "did not start", C.bad },
  slow     = { "left starting", C.slow },
  waiting  = { "left starting", C.slow },
}

local function draw_loader(g)
  local top = H // 5

  centred(g, top, load.done and "READY" or "STARTING", C.dim, "mono", 13)
  centred(g, top + 28, load.name, C.ink, "heading", 30)

  local x, w = (W - 560) // 2, 560
  local y = top + 90
  local part = nil
  local settled = 0

  for _, row in ipairs(load.rows) do
    if row.part ~= part then
      part = row.part
      g:text(x, y, part == "desktop" and "The desktop" or "Opening with it",
             C.foot, C.ground, "ui", 13)
      y = y + 24
    end

    local st = STATE[row.state] or STATE.starting

    if row.state ~= "starting" then settled = settled + 1 end

    g:fill_round(x, y, w, 34, C.card, 6)
    g:text(x + 14, y + 8, row.program, C.ink, C.card, "ui", 15)

    local said = st[1] .. (row.ms and row.state ~= "failed" and ("  " .. row.ms .. " ms") or "")
    local face = ui.sized("mono", 13)

    g:text(x + w - 14 - gfx.measure(said, face), y + 10, said, st[2], C.card, "mono", 13)
    y = y + 40

    if row.error then
      g:text(x + 14, y - 4, row.error, C.bad, C.ground, "ui", 12)
      y = y + 18
    end
  end

  -- The bar: how much has settled.
  y = y + 10
  g:fill_round(x, y, w, 4, C.well, 2)

  if #load.rows > 0 then
    g:fill_round(x, y, w * settled // #load.rows, 4, C.sel, 2)
  end

  local foot = "Each piece is ready when its first window is drawn."

  for _, row in ipairs(load.rows) do
    if row.state == "failed" then
      foot = "One did not start. The desktop opens anyway, and the log says why."
    elseif (row.state == "slow" or row.state == "waiting") and foot:sub(1, 3) ~= "One" then
      foot = "One is slow. The desktop opens without it."
    end
  end

  centred(g, y + 20, foot, C.foot, "ui", 13)
end

local buttons = {}       -- what the foot's buttons are, as drawn

local function draw_chooser(g)
  local _, cy = card_at(1)

  centred(g, cy - 110, ("KOSMOS %s"):format(version), C.dim, "mono", 13)
  centred(g, cy - 82, "Which desktop would you like?", C.ink, "heading", 30)

  for i, d in ipairs(list) do
    local x, y = card_at(i)
    local on = (i == sel)

    if on then g:fill_round(x - 5, y - 5, CARD_W + 10, CARD_H + 10, C.glow, 20) end

    g:fill_round(x, y, CARD_W, CARD_H, C.card, 16)
    g:frame_round(x, y, CARD_W, CARD_H, on and C.sel or C.line, 16)
    picture(g, x + 14, y + 14, CARD_W - 28, PIC_H, d)

    local title = d.name
    g:text(x + 18, y + PIC_H + 26, title, C.ink, C.card, "heading", 19)

    if d.name == last then
      local tx = x + 18 + gfx.measure(title, ui.sized("heading", 19)) + 10
      g:text(tx, y + PIC_H + 31, "USED LAST", C.sel, C.card, "mono", 11)
    end

    g:text(x + 18, y + PIC_H + 54, ui.fitted(d.says or "", CARD_W - 36, ui.sized("ui", 13)),
           C.dim, C.card, "ui", 13)
  end

  -- The foot: how to choose, and the machine's two ways out.
  local fy = cy + CARD_H + 48
  local hint = "Left and right to choose, Return to start"
  local face = ui.sized("ui", 13)
  local bw = { restart = 96, off = 110 }
  local total = gfx.measure(hint, face) + 22 + bw.restart + 12 + bw.off
  local x = (W - total) // 2

  g:text(x, fy + 7, hint, C.foot, C.ground, "ui", 13)
  x = x + gfx.measure(hint, face) + 22

  buttons = {
    { x = x, y = fy, w = bw.restart, h = 30, label = "Restart", action = "restart" },
    { x = x + bw.restart + 12, y = fy, w = bw.off, h = 30, label = "Shut Down", action = "off" },
  }

  for _, b in ipairs(buttons) do
    g:frame_round(b.x, b.y, b.w, b.h, 0xff333b50, 8)
    g:text(b.x + (b.w - gfx.measure(b.label, face)) // 2, b.y + 7, b.label,
           C.dim, C.ground, "ui", 13)
  end
end

local screen = ui.view{ x = 0, y = 0, w = W, h = H }

function screen:draw(g)
  g:fill(0, 0, self.w, self.h, C.ground)

  if load then draw_loader(g) else draw_chooser(g) end
end

--------------------------------------------------------------------------
-- Choosing.
--------------------------------------------------------------------------

local win

local function start()
  local d = list[sel]

  if not d or load then return false end

  load = { name = d.name, rows = {}, changed = sys.ticks() }

  local reply, err = fs.send("/Running/wm", { type = "desktop", name = d.name,
                                              watch = win.handle })

  if not reply or not reply.ok then
    print(("chooser: %s was not started: %s"):format(d.name,
          tostring(reply and reply.error or err)))
    load = nil
    return true
  end

  print(("chooser: %s chosen"):format(d.name))

  -- Asked again soon while it loads, for the quiet and the linger clocks.
  win.poll_wait_ticks = 5

  return true
end

function screen:mouse(action, x, y)
  if action ~= "press" or load then return false end

  for i = 1, #list do
    local cx, cy = card_at(i)

    if x >= cx and x < cx + CARD_W and y >= cy and y < cy + CARD_H then
      if sel == i then return start() end

      sel = i
      return true
    end
  end

  for _, b in ipairs(buttons) do
    if x >= b.x and x < b.x + b.w and y >= b.y and y < b.y + b.h then
      fs.send("/Running/wm", { type = "power", action = b.action })
      return true
    end
  end

  return false
end

win = ui.window{ title = "Desktops", w = W, h = H, fullscreen = true }

if not win then
  print("chooser: no window")
  return
end

win:add(screen)

function win:on_key(c)
  if load then return false end

  if c == -4 and sel > 1 then sel = sel - 1 return true end
  if c == -3 and sel < #list then sel = sel + 1 return true end
  if c == 10 or c == 13 then return start() end

  return false
end

--------------------------------------------------------------------------
-- What the window manager posts, as each piece moves.
--------------------------------------------------------------------------

--
-- A row's name for a person: the application's own (`-- kosmos: name`),
-- with its words after it - `tracker desktop` is Tracker (desktop). Asked
-- once a program, and the program's word when it has none.
--
local names = {}

local function named(program)
  program = tostring(program or "?")

  local word, rest = program:match("^(%S+)%s*(.*)$")
  word = word or program

  if names[word] == nil then
    local path = fs.program and fs.program(word)
    local attrs = path and fs.getattr(path)

    names[word] = (type(attrs) == "table" and type(attrs.title) == "string"
                   and attrs.title ~= "" and attrs.title) or word
  end

  return names[word] .. ((rest and rest ~= "") and (" (" .. rest .. ")") or "")
end

win.on_event = function(_, ev)
  if ev.type ~= "loading" or not load then return false end

  load.changed = sys.ticks()

  if ev.done then
    load.done = true
    load.leave = sys.ticks() + math.floor(LINGER * hz)
    return true
  end

  local i = math.tointeger(ev.index)

  if not i or i < 1 or i > 64 then return false end

  load.rows[i] = { program = named(ev.program), state = tostring(ev.state or "starting"),
                   ms = math.tointeger(ev.ms), error = ev.error and tostring(ev.error) or nil,
                   part = ev.part == "startup" and "startup" or "desktop" }

  -- Said, as the row is drawn: what this window heard, which is what the
  -- loader shows - the window manager's own line says only what it sent.
  if load.rows[i].state ~= "starting" then
    print(("chooser: heard %s %s"):format(load.rows[i].program, load.rows[i].state))
  end

  return true
end

if START then
  print(("chooser: starting %s, as the setting says"):format(last))
  start()
end

function win:on_frame()
  if not load then return false end

  local now = sys.ticks()

  -- Quiet for a second and a half with something still starting: the
  -- desktop does not wait for it, and neither does this.
  if not load.done and now - load.changed > QUIET * hz and #load.rows > 0 then
    for _, row in ipairs(load.rows) do
      if row.state == "starting" then row.state = "waiting" end
    end

    print("chooser: something is slow, and the desktop opens without it")
    load.done = true
    load.leave = now + math.floor(LINGER * hz)
    return true
  end

  if load.leave and now > load.leave then
    print(("chooser: %s is up"):format(load.name))
    win:close()
  end

  return false
end

win:run()
