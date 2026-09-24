-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Devices
-- kosmos: section preferences
-- kosmos: needs network
-- One place to configure Kosmos, divided by part.
--
--   wm preferences
--   wm preferences:sound                 opened on one category
--   wm preferences:--theme plexnight     a look, as a press on its swatch
--   wm preferences:--scale 150           a size, as the dropdown's choice
--
-- **And the look, since 24 September**: the Appearance panel's three
-- settings - the look, the wallpaper and the scale - are this window's
-- Appearance page now, and the panel is gone (`roadmap.md` 5zp). Two
-- windows setting the same three things was the sharpest instance of what
-- Diego called "a half baked UI now with old parts and new parts".
--
-- Diego asked for it on 23 September 2026 with GNOME's Settings beside it:
-- "one place to configure all kosmos". `docs/preferences.html` is the page
-- it was drawn from and `roadmap.md` 5zh the agreement.
--
-- **It knows nothing about any particular setting.** Every row on every page
-- comes out of `user/lib/settings.lua`, which says what there is, which file
-- it lives in and what kind of control it wants. This file is the drawing;
-- that one is the design. Adding a setting to the system means adding a line
-- there, and it appears here.
--

local ui = use("/lib/ui.lua")
local settings = use("/lib/settings.lua")
local hardware = use("/lib/hardware.lua")
local audio = use("/lib/audio.lua")
local clock = use("/lib/clock.lua")
local backlight_ok, backlight = pcall(use, "/lib/backlight.lua")
local theme = ui.theme

--
-- **The looks, parsed here, because choosing one means sending it.**
--
-- `themes.lua` ships each look as *text* in the format a `.theme` file on
-- the disk uses, and `theme.read` is the parser the window manager reads
-- one with. A look is a table of colours and faces only after that has
-- run - which is why this window could not apply one until it did. The
-- Appearance panel had these six lines from the day it was written, and it
-- folded into this window on 24 September (`roadmap.md` 5zp).
--
local LOOKS = use("/lib/themes.lua")

for _, name in ipairs(LOOKS.order) do
  local palette, said = theme.read(LOOKS[name], "dark")

  theme.install(name, palette)

  for _, why in ipairs(said) do
    print("preferences: " .. name .. ": " .. why)
  end
end

--
-- **Every number here is `docs/preferences.html`'s, measured off the
-- drawing rendered at one pixel to one pixel** (`roadmap.md` 5zp) - Diego,
-- 24 September 2026: "pixel perfect as the html mockups". They were chosen
-- by eye for the first two versions of this window, which is how it came to
-- be 720 wide beside a drawing of 840, with a sidebar of 176 beside one of
-- 216.
--
--   W, H        the drawn window: every group of the first page in view
--   SIDE        the sidebar, its one-pixel rule at SIDE - 1
--   HEAD        both headers; the page's has its rule at HEAD - 1
--   BODY_*      the page's padding, and the column it centres in
--   GROUP_LINE  a group's name: 12.5 at a line height of 1.55
--   GROUP_CARD  from that line's top to its card's
--   CARD_NEXT   from a card's bottom to the next group's line
--   ROW_*       11 above and below, 14 in from each side, and 48 at least
--   LINE_*      a row's name (13.5 at 1.55) and its note (12 at 1.4)
--
-- A row is 11, then whatever is tallest of its words and its control, then
-- 11 - which is why the drawing's rows are 60, 53 and 48 and never one
-- height: a name and a note, a name beside a dropdown, a name beside a
-- switch.
--
--
-- **740 by 680 since 24 September**, where the drawing was 840 by 920:
-- Diego, using it, "the entire preferences app looks too big with a lot of
-- whitespace unused". The column stays the drawing's 470 and the page is
-- now exactly that with its margins, and a window as tall as its tallest
-- page - Appearance - once the space between groups is the 20 every other
-- page of cards uses rather than the drawing's 42.
--
local W, H       = 740, 680
local SIDE       = 216
local HEAD       = 46
local BODY_TOP   = 22
local BODY_SIDE  = 26
local BODY_W     = 470
local GROUP_LINE = 19
local GROUP_CARD = 26
local CARD_NEXT  = 20
local CARD_R     = 10
local ROW_PAD    = 11
local ROW_IN     = 14
local ROW_MIN    = 48
local LINE_LABEL = 21
local LINE_NOTE  = 17

local win, err = ui.window{ title = "Preferences", w = W, h = H,
                            x = 150, y = 100 }

if not win then
  print("preferences: " .. tostring(err))
  return
end

--
-- Which category the argument asked for, if any. `wm preferences:sound`.
--
-- `args` is the string after the colon, not a list: `wm preferences:sound`
-- arrives as "sound". Indexing it gave nil and every launch opened on the
-- first category, which looked exactly like a window with no argument.
local function wanted()
  local a = (tostring(args or "")):match("^%s*(%S*)")

  for _, c in ipairs(settings.CATEGORIES) do
    if c.id == a then return c.id end
  end

  return settings.CATEGORIES[1].id
end

local showing = wanted()

--
-- The page: cards of rows, drawn by the view and filled with controls.
--
-- **The card is drawn here rather than being a widget**, because it is a
-- rectangle behind things that are already placed and nothing else needs one
-- yet. The moment a second application wants grouped rows it becomes
-- `ui.group` and this loses twenty lines; until then a widget in the kit
-- would be a guess at what the second caller wants.
--
local page = ui.view{ x = SIDE, y = 0, w = W - SIDE, h = H }

local cards = {}            -- { y, h, rules } for each card, page coordinates

--
-- The column of cards: `BODY_W` wide, centred in what the padding leaves.
--
local function column(w)
  local room = w - 2 * BODY_SIDE
  local width = math.min(BODY_W, room)

  return BODY_SIDE + (room - width) // 2, width
end

--
-- **The page's header**: the category's name at the left, in the title's
-- face, on the white the drawing gives a header, over a one-pixel rule.
--
-- The drawing has a close box at the right of it as well, and it is left
-- out on purpose: this is a window with a title tab, and the tab already
-- has one. Two close boxes a few pixels apart would be the one place the
-- mockup and the machine disagree about how a window is built.
--
local heading_text = ""

function page:draw(g)
  local x, width = column(self.w)

  g:fill(0, 0, self.w, self.h, theme.window)

  g:fill(0, 0, self.w, HEAD - 1, theme.sunken)
  g:fill(0, HEAD - 1, self.w, 1, theme.line_soft)
  g:text(18, (HEAD - 1 - gfx.height("title")) // 2, heading_text,
         theme.text, nil, "title")

  for _, c in ipairs(cards) do
    g:fill_round(x, c.y, width, c.h, theme.sunken, CARD_R)
    g:frame_round(x, c.y, width, c.h, theme.line_soft, CARD_R)

    -- The lines between rows, not around them: one card, several rows. They
    -- stop a pixel short so a rule does not run into the arc.
    for _, at in ipairs(c.rules) do
      g:fill(x + 1, at, width - 2, 1, theme.line_soft)
    end
  end
end

--
-- **The sidebar is added first, and that is a keyboard decision rather than
-- a drawing one.** They do not overlap - the sidebar is `0..SIDE` and the
-- page is `SIDE..W` - so the order changes nothing about the picture. What
-- it changes is `root:focusables()`, which walks the tree in the order
-- things were added, and therefore the order Tab visits them in and which
-- control holds the keyboard when the window opens.
--
-- The page went in first for a version, so the *first* thing to receive a
-- key was the Theme dropdown and the sidebar was last. A person opening
-- Preferences and pressing Down moved nothing at all: the navigation of
-- the window could not be reached from the keyboard until you had tabbed
-- past every control on the page you were trying to leave.
--
-- Found by the display harness, which had believed the opposite - its
-- Preferences phase says in its own words that "the sidebar is the first
-- focusable thing in the window, so Down moves it", and it was not.
--
-- The sidebar is the navigation. It goes first.
--

--
-- The sidebar: the categories, with the icons and the gaps
-- `settings.CATEGORIES` asks for, under a header with the window's name.
--
-- **The header draws no icons**, and the drawing has two - a search and a
-- menu. They would be two controls that do nothing, and a control that does
-- nothing is exactly the half-built feeling Diego named on the M700 ("usable
-- but does nothing to the system"). The name is at the left, where every
-- other window's title is.
--
local rebuild                      -- forward, so the list can call it

local side_head = ui.view{ x = 0, y = 0, w = SIDE, h = HEAD }

function side_head:draw(g)
  local face = "title"
  local word = "Preferences"

  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)

  --
  -- **At the left, 18 in, as every header's title is** - and so over the
  -- sidebar's icons, which start 18 in. The drawing centred it; Diego, 24
  -- September: "the preferences title in the app looks out of place, it
  -- should be aligned to the left to the content as the rest of the apps".
  --
  g:text(ui.layout.head_in, (self.h - 1 - gfx.height(face)) // 2, word,
         theme.text, nil, face)
end

local items = {}

for _, c in ipairs(settings.CATEGORIES) do
  items[#items + 1] = { id = c.id, name = c.name, icon = c.icon }
  if c.gap_after then items[#items + 1] = { gap = true } end
end

--
-- The list starts 2 below the header, and its column is the sidebar's
-- width less the rule - which is where the drawing's rows sit.
--
local side = ui.sidebar{
  x = 0, y = HEAD + 2, w = SIDE - 1, h = H - HEAD - 2,
  items = items, selected = showing,
  on_select = function(_, id)
    showing = id
    rebuild()
  end,
}

-- The sidebar's own ground, under the list and the header both, down to the
-- bottom of the window.
local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = H }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
end

win:add(side_ground)
win:add(side_head)
win:add(side)
win:add(page)

--
-- One row's control, whichever kind it is.
--
-- Everything that is not stored is shown rather than hidden, which is the
-- decision `settings.lua` records: a machine you can only configure by
-- rebuilding it is what this exists to end, so `smp=N` and the rest are here
-- with what they are set to and a line saying a restart is needed.
--
--
-- **A setting the window manager draws with, told at once.**
--------------------------------------------------------------------------
-- Applying a setting, rather than only writing it down.
--
-- **This window wrote files and changed nothing, and that is what it felt
-- like.** Diego, on the ThinkCentre M700 running 0.10.146: "the preferences
-- pane that is usable but does nothing to the system". He is right, and it
-- was two faults at once:
--
--   - `control_for`'s **dropdown never called this at all**, so a row could
--     be marked live and still do nothing - which is every choice in the
--     window, the look among them.
--   - and the look, the scale and the wallpaper were not marked live
--     either, because the first version of this could only send *one field*
--     in a `theme` request. A look is not one field: it is the resolved
--     colour table and the faces together, which is what the Appearance
--     panel built before it sent.
--
-- So an apply is a **function per setting** rather than a field name, and
-- the table is keyed by the setting's own `key` - there is no second list
-- to keep in step, and a row that has no entry here is one that genuinely
-- takes effect at the next restart.
--
-- The window manager is the only thing that can do any of these: it
-- composes the desktop, it owns the wallpaper, and the scale is its
-- arithmetic. The requests are the ones it already takes - the ones the
-- Appearance panel sent, until its three settings folded into this window.
--------------------------------------------------------------------------

local APPLY = {
  --
  -- A look is a *whole*: the colours the tokens name, and the faces beside
  -- them. Sent exactly as the Appearance panel sends it, because a machine
  -- that kept faces from an older panel should get the look's back the
  -- moment a look is chosen.
  --
  palette = function(name)
    local look = theme.palettes[name] or {}
    local colours = {}

    for _, k in ipairs(theme.tokens) do colours[k] = look[k] end

    return fs.send("/app/wm", { type = "theme", palette = colours,
                                fonts = look.fonts })
  end,

  scale = function(pct)
    return fs.send("/app/wm", { type = "scale", pct = pct })
  end,

  wallpaper = function(path)
    return fs.send("/app/wm", { type = "wallpaper",
                                path = (path ~= "") and path or nil })
  end,

  -- One field each, and the manager ignores a field it does not know - so
  -- another of these is one line.
  corner = function(on)
    return fs.send("/app/wm", { type = "theme", corner = on })
  end,

  shadow = function(on)
    return fs.send("/app/wm", { type = "theme", shadow = on })
  end,

  -- What the power button and the Super key do: the manager acts on both
  -- in its key path and holds them rather than reading a file there.
  button = function(what)
    return fs.send("/app/wm", { type = "keys", power = what })
  end,

  super = function(what)
    return fs.send("/app/wm", { type = "keys", super = what })
  end,
}

--
-- **Applied first, written second**, which was the Appearance panel's order and
-- the right one: a file that holds an appearance the system refused is a
-- file that lies about the machine. When the manager says no, the setting
-- is not stored and the reason reaches the log.
--
local function live(it, value)
  local apply = APPLY[it.key]

  if not apply then return true end

  local ok, why = apply(value)

  if not ok then
    print("preferences: " .. tostring(it.key) .. ": " .. tostring(why))
  end

  return ok, why
end

--
-- **Both controls go through `live` and then `settings.set`, in that
-- order.** The dropdown did neither for a version: it stored the value and
-- returned, so every choice in this window - the look above all - wrote a
-- file and changed nothing anybody could see.
--
--
-- **The look, as a row of swatches** - `docs/preferences.html`'s Theme row,
-- which the first version drew as a dropdown of names. A look is chosen by
-- how it looks, and a dropdown of words asks a person to remember that.
--
-- Each look's own `swatch`, 26 across with a radius of 6 and a faint edge,
-- 7 apart; the one in force ringed in the accent, two pixels wide and two
-- pixels out - the drawing's `outline: 2px solid; outline-offset: 2px`,
-- which follows the square's rounding. The drawing's four are Plex, Plex
-- Night, Classic and Studio in that order - yellow, blue, grey, near black -
-- and Endeavour, which came after it, is its own light blue tab.
--
-- Left and right move along the row and choose, as the sidebar's arrows do,
-- because the first thing a person does with a row of looks is try them.
--
local SWATCH, SWATCH_GAP, RING = 26, 7, 4
local EDGE = 0x1f000000                  -- the drawing's rgba(0,0,0,.12)

local function swatches(it)
  local names = LOOKS.order
  local v = ui.view{ w = #names * SWATCH + (#names - 1) * SWATCH_GAP + 2 * RING,
                     h = SWATCH + 2 * RING }

  v.focusable = true
  v.value = settings.get(it)

  local function pick(self, name)
    if name == self.value then return end

    if live(it, name) then
      settings.set(it, name)
      self.value = name
    end

    win:paint()
  end

  local function at(i) return RING + (i - 1) * (SWATCH + SWATCH_GAP) end

  function v:draw(g)
    for i, name in ipairs(names) do
      local x = at(i)
      local look = theme.palettes[name] or {}

      if name == self.value then
        g:frame_round(x - RING, 0, SWATCH + 2 * RING, SWATCH + 2 * RING,
                      theme.accent, 6 + RING)
        g:frame_round(x - RING + 1, 1, SWATCH + 2 * RING - 2,
                      SWATCH + 2 * RING - 2, theme.accent, 6 + RING - 1)
      end

      g:fill_round(x, RING, SWATCH, SWATCH, look.swatch or theme.text_dim, 6)
      g:frame_round(x, RING, SWATCH, SWATCH, EDGE, 6)
    end

    if self.focused then
      g:fill(RING, self.h - 1, self.w - 2 * RING, 1, theme.ring)
    end
  end

  function v:key(c)
    local i = 1

    for k, name in ipairs(names) do
      if name == self.value then i = k end
    end

    if c == -4 and i > 1 then pick(self, names[i - 1]) return true end
    if c == -3 and i < #names then pick(self, names[i + 1]) return true end

    return c == -3 or c == -4
  end

  function v:mouse(action, x)
    if action ~= "press" then return true end

    for i, name in ipairs(names) do
      if x >= at(i) and x < at(i) + SWATCH then pick(self, name) end
    end

    return true
  end

  return v
end

local row_room = 400                -- a whole-row control's width, per page
local now_label = nil               -- the clock's row, which keeps time

local function control_for(it, x, y, changed)
  if it.key == "palette" then return swatches(it) end

  -- The wallpapers are whatever `/home` and the image hold at the moment the
  -- page is drawn, so the list is made here rather than in the schema.
  if it.key == "wallpaper" then
    it = setmetatable({ choices = settings.wallpapers() }, { __index = it })
  end
  if it.kind == "switch" then
    return ui.switch{ x = x, y = y, on = settings.get(it) == true,
                      on_change = function(_, on)
                        if live(it, on) then settings.set(it, on) end
                        if changed then changed() end
                      end }
  end

  --
  -- **The rows that act on the machine now rather than on a file**: the
  -- master volume and its mute on the audio server, the brightness on the
  -- backlight. Nothing is written - the server holds the level, as the
  -- Mixer and the volume keys find it - and a machine without the device
  -- gets words instead of a control that moves nothing.
  --
  if it.kind == "volume" or it.kind == "mute" then
    local _, st = audio.streams()

    if not st or audio.format().period == 0 then return nil end

    if it.kind == "volume" then
      return ui.slider{ x = x, y = y, w = 200, max = 256,
                        value = st.master or 256,
                        on_change = function(_, v) audio.set{ master = v } end }
    end

    return ui.switch{ x = x, y = y, on = st.master_muted == true,
                      on_change = function(_, on)
                        audio.set{ master_muted = on }
                      end }
  end

  if it.kind == "brightness" then
    local now = backlight_ok and backlight.get() or nil

    if not now then return nil end

    return ui.slider{ x = x, y = y, w = 200, max = 256, value = now,
                      on_change = function(_, v) backlight.set(v) end }
  end

  -- A button that opens the window a row is about.
  if it.kind == "open" then
    return ui.button{ x = x, y = y, text = "Open",
                      on_click = function()
                        fs.send("/app/wm", { type = "launch",
                                             program = it.program })
                      end }
  end

  --
  -- **The time zone, as a stepper**: thirty-seven offsets are a menu taller
  -- than the screen, so a choice among them is a step either way.
  --
  if it.kind == "stepper" then
    local choices = {}

    for _, m in ipairs(clock.OFFSETS) do
      choices[#choices + 1] = { m, clock.offset_name(m) }
    end

    return ui.stepper{ x = x, y = y, choices = choices,
                       value = settings.get(it) or 0,
                       on_change = function(_, v)
                         -- The clock's row reads the time again on its next
                         -- tick, with the new offset.
                         settings.set(it, v)
                         if changed then changed() end
                       end }
  end

  --
  -- **What opens with the desktop**, as the Startup window lists it: every
  -- application, ticked or not, eight rows showing and the rest a scroll
  -- away, across the whole card.
  --
  if it.kind == "startup" then
    local names, ticked = {}, {}

    for _, file in ipairs(fs.list("/bin") or {}) do
      local attrs = fs.getattr("/bin/" .. file)

      if attrs and attrs.kind == "application" then
        local short = file:gsub("%.lua$", "")

        if short ~= "deskbar" then names[#names + 1] = short end
      end
    end

    table.sort(names)

    for _, name in ipairs(use("/lib/startup.lua").items()) do
      ticked[tostring(name)] = true
    end

    return ui.list{ x = x, y = y, w = row_room, h = 4 + 8 * ui.metrics.row,
                    items = names, checks = ticked, bare = true,
                    -- A checklist is read down its boxes; a row lit as
                    -- chosen would be a second, meaningless state.
                    selected = 0,
                    on_toggle = function()
                      local items = {}

                      for _, name in ipairs(names) do
                        if ticked[name] then items[#items + 1] = name end
                      end

                      fs.write(settings.STARTUP, { items = items })
                    end }
  end

  if it.kind == "choice" and it.choices then
    return ui.dropdown{ x = x, y = y, choices = it.choices,
                        value = settings.get(it),
                        on_change = function(_, v)
                          if live(it, v) then settings.set(it, v) end
                          if changed then changed() end
                        end }
  end

  return nil
end

--
-- What a row shows on the right when it has no control: a value, a hint, or
-- the word a `boot` setting needs.
--
--
-- What the machine is, read once: the About rows and nothing else.
--
-- `sys.build()` for the version, `hardware.name(sys.info())` for what the
-- machine calls itself and `/dev/memory` for its size - the same three doors
-- `neofetch` reads, so the two programs cannot disagree about what this
-- machine is. A second way of asking would be a second answer eventually.
--
local function facts()
  local b = sys.build() or {}
  local mem = fs.read("/dev/memory") or {}
  local info = sys.info() or {}
  local screen = fs.read("/dev/screen") or {}
  local out = {
    version = tostring(b.version or "?"),
    -- The firmware's name for it, or the board's where there is no
    -- firmware table to ask - as About and `neofetch` fall back.
    machine = tostring(hardware.name(info) or b.platform or "this machine"),
    memory = ("%d MB, %d free"):format(mem.total_mb or 0, mem.free_mb or 0),
  }

  out.resolution = (screen.width and screen.width > 0)
                   and ("%d × %d"):format(screen.width, screen.height)
                   or "no screen"

  local fmt = audio.format()

  out.sound = (fmt.period == 0) and "No sound device"
              or ("%d Hz · %s"):format(fmt.rate, (fmt.channels == 2)
                                        and "stereo"
                                        or (fmt.channels .. " channels"))

  local present = info.cpus_present or info.cpus or 1
  local using = info.cpus or 1

  out.processors = (present == using)
                   and ("%d, all given work"):format(present)
                   or ("%d of %d given work"):format(using, present)

  out.now = clock.now() and ("%s · %s"):format(clock.date_string(clock.now()),
                                                clock.time_string(clock.now()))
            or "this machine has no clock"

  --
  -- The card as the bus names it, and its address and gateway as the stack
  -- says them - the same two questions `neofetch` asks, for the reason it
  -- gives: a card the stack does not answer for is not "no card".
  --
  local driven = hardware.network(sys.bus())
  local net = fs.net_info and fs.net_info("/net") or nil

  local function dotted(a)
    if type(a) == "string" and #a == 4 and a ~= "\0\0\0\0" then
      return ("%d.%d.%d.%d"):format(a:byte(1, 4))
    end

    return nil
  end

  out.net_card = driven[1] and driven[1].name or "No card found"
  out.net_address = net and net.card and dotted(net.address) or "none"
  out.net_gateway = net and net.card and dotted(net.gateway) or "none"

  return out
end

local fact = facts()

local function value_text(it)
  -- The option's name is in the note; this is the part that is not obvious
  -- from reading the row, and the part a long note must not squeeze out.
  if it.kind == "fact" then return fact[it.fact] or "-" end
  if it.kind == "volume" or it.kind == "mute" then return "No sound device" end
  if it.kind == "brightness" then return "Not on this screen" end

  --
  -- **A row with nothing on the right is a row that looks broken**, and two
  -- kinds drew one: a `level` and a `text`, which have no control in the
  -- kit that fits a settings row yet. Each says so now.
  --
  -- It is the difference between a window that is unfinished and a window
  -- that is wrong, and a person cannot tell which from a blank. Diego, on
  -- the M700: "the preferences pane that is usable but does nothing to the
  -- system" - half of that feeling was the looks not applying, and half was
  -- these (`roadmap.md` 5zh).
  --

  return tostring(settings.get(it) or "")
end

--
-- Build the page for `showing`.
--
rebuild = function()
  for i = #page.children, 1, -1 do page.children[i] = nil end
  cards = {}

  for _, c in ipairs(settings.CATEGORIES) do
    if c.id == showing then heading_text = c.name end
  end

  local cx, width = column(page.w)
  local y = HEAD + BODY_TOP

  fact = facts()
  row_room = width - 2 - 2 * ROW_IN
  now_label = nil

  for gi, group in ipairs(settings.groups(showing)) do
    if gi > 1 then y = y + CARD_NEXT end

    -- The group's name, 3 in from the card's edge as the drawing sets it,
    -- centred in its line.
    page:add(ui.label{ x = cx + 3,
                       y = y + (GROUP_LINE - gfx.height("heading")) // 2,
                       w = width, text = group.name, role = "heading" })

    y = y + GROUP_CARD

    local card = { y = y, h = 0, rules = {} }
    local n = #group.items

    y = y + 1                           -- the card's top edge

    for i, it in ipairs(group.items) do
      local right = cx + width - 1 - ROW_IN
      local c = control_for(it, 0, 0, nil)
      local taken, ch = 0, 0

      if c then
        taken, ch = c.w, c.h
      else
        local t = value_text(it)

        if t ~= "" then
          taken, ch = gfx.measure(t), gfx.height()
        end
      end

      --
      -- **As tall as its tallest part plus 11 each side, and 48 at the
      -- least** - where the last row's 48 is all its own and every other
      -- row gives one of its pixels to the rule under it, which is how the
      -- drawing's border-box rows come out.
      --
      local words = LINE_LABEL + (it.note and LINE_NOTE or 0)
      local least = (i == n) and ROW_MIN or (ROW_MIN - 1)
      local h = math.max(least, 2 * ROW_PAD + math.max(words, ch))

      if c then
        c.x = right - c.w
        c.y = y + (h - c.h) // 2
        page:add(c)
      elseif taken > 0 then
        local shown = ui.label{ x = right - taken, y = y + (h - ch) // 2,
                                w = taken + 2, text = value_text(it),
                                color = theme.text_dim, role = "ui" }

        page:add(shown)

        -- The clock's row is kept right on the window's tick, from the
        -- right edge it was placed against.
        if it.fact == "now" then now_label, shown.right = shown, right end
      end

      local room = right - (taken > 0 and taken + ROW_IN or 0)
                   - (cx + 1 + ROW_IN)

      --
      -- The name and its note as one block, centred in the row: each in a
      -- line of the drawing's height, the face centred in its line.
      --
      local top = y + (h - words) // 2

      -- The row's name in `label`: the drawing's 13.5 at weight 500.
      if it.label ~= "" then
        page:add(ui.label{
          x = cx + 1 + ROW_IN,
          y = top + (LINE_LABEL - gfx.height("label")) // 2,
          w = room, text = it.label, role = "label" })
      end

      --
      -- **The note is a control's size, not a label's**: the drawing's 12
      -- against its 13.5 for the name. It was drawn in `text` for one build
      -- and read as a second label.
      --
      if it.note then
        page:add(ui.label{
          x = cx + 1 + ROW_IN,
          y = top + LINE_LABEL + (LINE_NOTE - gfx.height("ui")) // 2,
          w = room, text = it.note, color = theme.text_dim, role = "ui" })
      end

      y = y + h

      if i < n then
        card.rules[#card.rules + 1] = y
        y = y + 1
      end
    end

    y = y + 1                           -- the card's bottom edge
    card.h = y - card.y
    cards[#cards + 1] = card
  end

  win:paint()
end

rebuild()

--
-- **The time, kept**: twice a second the clock's row says it again, placed
-- from its right edge so a narrower minute does not leave it adrift.
--
local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function ticker:tick()
  if not now_label then return end

  local now = clock.now()

  if not now then return end

  local text = ("%s · %s"):format(clock.date_string(now),
                                   clock.time_string(now))

  if text ~= now_label.text then
    local w = gfx.measure(text)

    now_label.text = text
    now_label.x, now_label.w = now_label.right - w, w + 2
  end
end

win:add(ticker)

--------------------------------------------------------------------------
-- The command line: a choice made the way a click makes it.
--
--   wm preferences:--theme plexnight     a look, as a press on its swatch
--   wm preferences:--scale 150           a size, as the dropdown's choice
--
-- **Inherited from the Appearance panel with the panel's job** (`roadmap.md`
-- 5zp): it had these two for the display harness, which can type and cannot
-- aim, and the path they exercise is the one a person's press takes - the
-- same `live` and the same `settings.set`, so what is tested is this window
-- rather than a side door into the window manager.
--
-- Each says what happened, in words the harness waits for: the look with
-- the faces the manager says it *holds*, which is what it loaded rather
-- than what it was asked for.
--
local function item_for(key)
  for _, it in ipairs(settings.ITEMS) do
    if it.key == key and it.file == settings.APPEARANCE then return it end
  end
end

do
  local a = tostring(args or "")
  local look = a:match("%-%-theme%s+(%S+)")
  local pct = tonumber(a:match("%-%-scale%s+(%d+)") or "")

  if look then
    local it = item_for("palette")

    -- `live` hands back what the manager answered: its reply, which carries
    -- `held`, or nothing and why.
    local reply, why = live(it, look)

    if reply then
      settings.set(it, look)

      local held = {}

      for _, role in ipairs(theme.roles) do
        local f = type(reply) == "table" and (reply.held or {})[role]

        if f then held[#held + 1] = ("%s=%s/%s"):format(role, f.font, f.px) end
      end

      print("preferences: theme " .. look .. " applied, held "
            .. table.concat(held, " "))
    else
      print("preferences: theme " .. look .. " refused: " .. tostring(why))
    end
  end

  if pct then
    local it = item_for("scale")
    local ok, why = live(it, pct)

    if ok then settings.set(it, pct) end

    print(("preferences: scale %d %s"):format(pct,
          ok and "applied" or ("refused: " .. tostring(why))))
  end

  -- The page was drawn before the choice, so it is drawn again to show it.
  if look or pct then rebuild() end

  --
  -- And what it is, as it opens: its size and how many looks it offers,
  -- which is what the display harness holds the drawing to.
  --
  print(("preferences: %dx%d, %d looks, %s"):format(W, H, #LOOKS.order,
        tostring(settings.get(item_for("palette")))))
end

win:run()
