-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Devices
-- kosmos: name Preferences
-- kosmos: section preferences
-- kosmos: needs network
-- One place to configure Kosmos, divided by part.
--
--   wm preferences
--   wm preferences:sound                 opened on one category
--   wm preferences:--theme plexnight     a look, as a press on its swatch
--   wm preferences:--scale 150           a size, as the dropdown's choice
--   wm preferences:filetypes --find mp4  File types, found as by typing
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

local ui = use("/Kosmos/Libraries/ui.lua")
local settings = use("/Kosmos/Libraries/settings.lua")
local hardware = use("/Kosmos/Libraries/hardware.lua")
local audio = use("/Kosmos/Libraries/audio.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local types = use("/Kosmos/Libraries/filetypes.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
local backlight_ok, backlight = pcall(use, "/Kosmos/Libraries/backlight.lua")
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
local LOOKS = use("/Kosmos/Libraries/themes.lua")

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
                            x = 150, y = 100, header = true }

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
--
-- **It follows every edge** (Diego, 8 October, on the M700: "when resizing
-- the preferences app it breaks the layout"): without a `follow` it kept
-- the size it opened at, so a taller window drew no rows below the old
-- bottom and their labels were cut at it. `win.on_resize` lays the page
-- out again at the new size.
--
local page = ui.view{ x = SIDE, y = 0, w = W - SIDE, h = H,
                      follow = { "left", "right", "top", "bottom" } }

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
-- face, on the white the drawing gives a header, over a one-pixel rule -
-- the kit's header, which draws exactly that.
--
-- **And the window's title bar, in a look with none** (`roadmap.md` 6zj):
-- the drawing's three at its right end, where this used to say they were
-- left out because the tab above had them. It was drawn by the page until
-- then, which is why it is a view of its own now: the three need a header
-- to leave room in, and its empty band is what moves the window.
--
local header = ui.header{ x = SIDE, y = 0, w = W - SIDE, title = "",
                          title_bar = true }

function page:draw(g)
  local x, width = column(self.w)

  g:fill(0, 0, self.w, self.h, theme.window)

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

-- How far down the page is scrolled, and how tall it is (`page:wheel`).
local scroll, page_h = 0, 0

-- Taken hold of like the header beside it, in a look with no title bars:
-- one band across the top of the window, as `docs/nochrome.html` draws it.
local side_head = ui.view{ x = 0, y = 0, w = SIDE, h = HEAD,
                           moves_window = true }

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
    showing, scroll = id, 0
    rebuild()
  end,
}

-- The sidebar's own ground, under the list and the header both, down to the
-- bottom of the window.
local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = H,
                             follow = { "left", "top", "bottom" } }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
end

win:add(side_ground)
win:add(side_head)
win:add(side)
win:add(page)
win:add(header)

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
  -- How many notifications the history keeps, told to the server the moment
  -- it changes - it is the server that drops the oldest (`notifyproto.h`).
  --
  keep = function(n)
    if notify.keep(tonumber(n) or 0) then return true end

    return false, "the notification server did not answer"
  end,

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

    return fs.send("/Running/wm", { type = "theme", palette = colours,
                                fonts = look.fonts })
  end,

  scale = function(pct)
    return fs.send("/Running/wm", { type = "scale", pct = pct })
  end,

  wallpaper = function(path)
    return fs.send("/Running/wm", { type = "wallpaper",
                                path = (path ~= "") and path or nil })
  end,

  wallpaper_fit = function(fit)
    return fs.send("/Running/wm", { type = "wallpaper_fit", fit = fit })
  end,

  -- One field each, and the manager ignores a field it does not know - so
  -- another of these is one line.
  corner = function(on)
    return fs.send("/Running/wm", { type = "theme", corner = on })
  end,

  shadow = function(on)
    return fs.send("/Running/wm", { type = "theme", shadow = on })
  end,

  -- Where the bar is, and how wide the dock: told to the Deskbar, which
  -- starts itself again there. A machine with no Deskbar running keeps the
  -- setting all the same, for the next one to read.
  -- The dock's transparency, told as the slider moves: the Deskbar draws its
  -- pill again and starts nothing.
  dock_size = function(size)
    local ok, why = fs.write("/Running/Deskbar/size", tostring(size))

    if not ok then print("preferences: the Deskbar was not told: " .. tostring(why)) end

    return true
  end,

  dock_transparency = function(percent)
    local ok, why = fs.write("/Running/Deskbar/transparency", tostring(percent))

    if not ok then print("preferences: the Deskbar was not told: " .. tostring(why)) end

    return true
  end,

  bar = function(where)
    local ok, why = fs.write("/Running/Deskbar/bar", tostring(where))

    if not ok then print("preferences: the Deskbar was not told: " .. tostring(why)) end

    return true
  end,

  dock = function(how)
    local ok, why = fs.write("/Running/Deskbar/dock", tostring(how))

    if not ok then print("preferences: the Deskbar was not told: " .. tostring(why)) end

    return true
  end,

  -- What the power button and the Super key do: the manager acts on both
  -- in its key path and holds them rather than reading a file there.
  button = function(what)
    return fs.send("/Running/wm", { type = "keys", power = what })
  end,

  super = function(what)
    return fs.send("/Running/wm", { type = "keys", super = what })
  end,

  -- Focus following the pointer, and after how long: the manager acts on
  -- both in its pointer pass, every pass, and holds them.
  focus_follows = function(on)
    return fs.send("/Running/wm", { type = "keys", focus_follows = on == true })
  end,

  focus_delay_ms = function(ms)
    return fs.send("/Running/wm", { type = "keys", focus_delay_ms = ms })
  end,

  -- The Mouse page (`roadmap.md` 6zi): the speed set on the board and the
  -- double-click span told to every window, both by the manager, which
  -- reads the same file when it starts.
  speed = function(units)
    return fs.send("/Running/wm", { type = "mouse", speed = units })
  end,

  double_click_ms = function(ms)
    return fs.send("/Running/wm", { type = "mouse", double_click_ms = ms })
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
local row_room = 400                -- a whole-row control's width, per page
--
-- **The looks, as a gallery of small desks with their names** - Diego, 8
-- October, at eleven looks: "the themes lost their names so i cant tell
-- which name is which", "we need a way to see names of the themes as well
-- to remember". It was a row of swatches, each look's one colour 26 across
-- (`docs/preferences.html`), which said how a look looks and never what it
-- is called - fine at four, a row of guesses at eleven.
--
-- Each look a tile under the row's words, across the whole card: its desk,
-- a little window on it in its window colour with its title strip, its one
-- colour as a pill and its dim text as a line - drawn from the look's own
-- palette - and its name under it. The one in force ringed in the accent.
--
-- Left and right move along and choose, as the sidebar's arrows do,
-- because the first thing a person does with looks is try them; up and
-- down move a row.
--
local GALLERY_COLS, GALLERY_GAP = 4, 10
local PREVIEW_H, NAME_H = 58, 24
local EDGE = 0x1f000000                  -- the drawing's rgba(0,0,0,.12)

local function swatches(it)
  local names = LOOKS.order
  local rows = (#names + GALLERY_COLS - 1) // GALLERY_COLS
  local tile_w = (row_room - (GALLERY_COLS - 1) * GALLERY_GAP) // GALLERY_COLS
  local tile_h = PREVIEW_H + NAME_H
  local v = ui.view{ w = row_room, h = rows * tile_h + (rows - 1) * GALLERY_GAP }

  v.focusable = true
  v.below = true                         -- under the row's words, not beside them
  v.value = settings.get(it)

  local function pick(self, name)
    if name == self.value then return end

    if live(it, name) then
      settings.set(it, name)
      self.value = name
    end

    win:paint()
  end

  local function at(i)
    local col, row = (i - 1) % GALLERY_COLS, (i - 1) // GALLERY_COLS

    return col * (tile_w + GALLERY_GAP), row * (tile_h + GALLERY_GAP)
  end

  function v:draw(g)
    for i, name in ipairs(names) do
      local x, y = at(i)
      local look = theme.palettes[name] or {}
      local desk = look.desktop or theme.desktop

      if name == self.value then
        g:frame_round(x, y, tile_w, PREVIEW_H, theme.accent, 10)
        g:frame_round(x + 1, y + 1, tile_w - 2, PREVIEW_H - 2, theme.accent, 9)
      end

      -- The desk, and a window on it.
      g:fill_round(x + 3, y + 3, tile_w - 6, PREVIEW_H - 6, desk, 8)
      g:frame_round(x + 3, y + 3, tile_w - 6, PREVIEW_H - 6, EDGE, 8)

      local wx, wy, ww, wh = x + 14, y + 12, tile_w - 28, PREVIEW_H - 18

      g:fill_round(wx, wy, ww, wh, look.window or theme.window, 5)
      g:fill(wx + 5, wy + 9, ww - 10, 1, look.line_soft or theme.line_soft)
      g:fill_round(wx + ww - 22, wy + 3, 4, 4, 0xffff5f57, 2)
      g:fill_round(wx + ww - 15, wy + 3, 4, 4, 0xfffebc2e, 2)
      g:fill_round(wx + ww - 8, wy + 3, 4, 4, 0xff28c840, 2)
      g:fill_round(wx + 6, wy + 15, math.min(34, ww - 12), 8, look.accent or theme.accent, 4)
      g:fill(wx + 6, wy + 28, math.min(52, ww - 12), 3, look.text_dim or theme.text_dim)

      -- Its name, under it, centred.
      local title = LOOKS.titles[name] or name
      local face = (name == self.value) and "label" or "ui"

      g:text(x + (tile_w - gfx.measure(title, face)) // 2,
             y + PREVIEW_H + (NAME_H - gfx.height(face)) // 2, title,
             (name == self.value) and theme.text or theme.text_dim, nil, face)
    end

    if self.focused then
      g:fill(0, self.h - 1, self.w, 1, theme.ring)
    end
  end

  function v:key(c)
    local i = 1

    for k, name in ipairs(names) do
      if name == self.value then i = k end
    end

    local to = (c == -4 and i - 1) or (c == -3 and i + 1)
               or (c == -1 and i - GALLERY_COLS) or (c == -2 and i + GALLERY_COLS) or nil

    if not to then return false end

    if to >= 1 and to <= #names then pick(self, names[to]) end

    return true
  end

  -- A tile is put in force on the release over the one it was pressed on
  -- (`ui.click`; Diego, 9 October: a click is a press and a release, "in
  -- all kosmos"), and let go anywhere else the look in force stays.
  local function tile_at(x, y)
    for i, name in ipairs(names) do
      local tx, ty = at(i)

      if x >= tx and x < tx + tile_w and y >= ty and y < ty + tile_h then
        return name, function() pick(v, name) end
      end
    end

    return nil
  end

  function v:mouse(action, x, y)
    ui.click(self, action, x, y, tile_at)

    return true
  end

  return v
end

local now_label = nil               -- the clock's row, which keeps time

local function control_for(it, x, y, changed)
  if it.key == "palette" then return swatches(it) end

  -- The wallpapers are whatever `/Home` and the image hold at the moment the
  -- page is drawn, so the list is made here rather than in the schema.
  if it.key == "wallpaper" then
    it = setmetatable({ choices = settings.wallpapers() }, { __index = it })
  end
  if it.kind == "switch" then
    local sw = ui.switch{ x = x, y = y, on = settings.get(it) == true,
                          on_change = function(_, on)
                            if live(it, on) then settings.set(it, on) end
                            if it.sender then
                              print(("preferences: %s notifications %s"):format(
                                    it.sender, on and "on" or "off"))
                            end
                            if changed then changed() end
                          end }

    sw.sender = it.sender
    return sw
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

  --
  -- **A percentage, as a slider and its number** - the dock's transparency.
  -- Written and applied as it moves, as a switch is when it is pressed.
  --
  if it.kind == "percent" then
    local box = ui.view{ x = x, y = y, w = 256, h = 20 }
    local shown = ui.label{ x = 212, y = (20 - gfx.height()) // 2, w = 44,
                            text = ("%d%%"):format(settings.get(it) or 0),
                            color = theme.text_dim, role = "ui" }
    local slider = ui.slider{ x = 0, y = 0, w = 200, max = 100,
                              value = tonumber(settings.get(it)) or 0,
                              on_change = function(_, v)
                                shown.text = ("%d%%"):format(v)
                                if live(it, v) then settings.set(it, v) end
                                if changed then changed() end
                              end }

    box:add(slider)
    box:add(shown)
    return box
  end

  --
  -- **The Mouse page's two speeds, each a slider of steps** with its ends
  -- named under it (`docs/preferences.html`, `settings.POINTER_SPEEDS` and
  -- `DOUBLE_CLICK_MS`): the step nearest what the file says is shown, and the
  -- step's own number is what is written and applied as it moves. A tablet
  -- has no speed - it says where it is - so its row has words instead.
  --
  if it.kind == "pointer_speed" or it.kind == "double_click" then
    local steps = it.kind == "pointer_speed" and settings.POINTER_SPEEDS
                  or settings.DOUBLE_CLICK_MS

    if it.kind == "pointer_speed" and sys.pointer_speed() == 0 then return nil end

    local now, at = tonumber(settings.get(it)) or it.default, 1

    for i, v in ipairs(steps) do
      if math.abs(v - now) < math.abs(steps[at] - now) then at = i end
    end

    local lh = gfx.height()
    local box = ui.view{ x = x, y = y, w = 200, h = 20 + lh }
    local function seconds(ms) return ("%g s"):format(ms / 1000) end
    -- The span chosen, centred under the slider and centred again as it
    -- changes: a label is as wide as its words when it is given no width.
    local middle = ui.label{ x = 0, y = 20, text = "", color = theme.text_dim, role = "ui" }
    local function show(ms)
      middle.text = seconds(ms)
      middle.x = 100 - gfx.measure(middle.text, "ui") // 2
    end

    if it.kind == "double_click" then show(steps[at]) end

    local slider = ui.slider{ x = 0, y = 0, w = 200, max = #steps - 1, value = at - 1,
                              on_change = function(_, v)
                                local value = steps[v + 1]

                                if it.kind == "double_click" then show(value) end
                                if live(it, value) then settings.set(it, value) end
                                print(("preferences: %s %d"):format(it.key, value))
                                if changed then changed() end
                              end }

    box:add(slider)
    box:add(ui.label{ x = 0, y = 20, w = 60, text = "Slow", color = theme.text_dim, role = "ui" })
    box:add(middle)
    box:add(ui.label{ x = 200 - gfx.measure("Fast", "ui"), y = 20, text = "Fast",
                      color = theme.text_dim, role = "ui" })
    return box
  end

  --
  -- **Try it**: a folder that opens on a double click at the speed chosen -
  -- the kit's own span, as every window has it from the manager - so the
  -- speed is felt rather than read.
  --
  if it.kind == "try_double_click" then
    local box = ui.view{ x = x, y = y, w = 120, h = 34 }
    local said = ui.label{ x = 42, y = (34 - gfx.height()) // 2, w = 72, text = "Closed",
                           color = theme.text_dim, role = "ui" }
    local folder = ui.view{ x = 4, y = 2, w = 30, h = 30 }
    local open, last = false, nil

    function folder:draw(g)
      g:line_icon(0, 0, "folder", open and theme.accent or theme.text_dim, 30)
    end

    function folder:mouse(action)
      if action ~= "press" then return true end

      local t = sys.ticks()

      if last and t - last < ui.double_click_ticks() then
        open, last = not open, nil
        said.text = open and "Opened" or "Closed"
        print("preferences: the folder " .. (open and "opened" or "closed"))
      else
        last = t
      end

      return true
    end

    box:add(folder)
    box:add(said)
    return box
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
                        fs.send("/Running/wm", { type = "launch",
                                             program = it.program })
                      end }
  end

  --
  -- **The time zone, as a stepper**: thirty-seven offsets are a menu taller
  -- than the screen, so a choice among them is a step either way. Date &
  -- Time's page is the only place it is set since Date & Time, a window
  -- that listed the same offsets, went on 5 October (`roadmap.md`, *One
  -- kit, one door*); `wm preferences:datetime` opens it.
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
                         -- tick, with the new offset. A refusal reaches the
                         -- log, as it reached Date & Time's own window when
                         -- that was where this was set.
                         local ok, why = settings.set(it, v)

                         if not ok then
                           print("preferences: the time zone was not kept: "
                                 .. tostring(why))
                         end
                         if changed then changed() end
                       end }
  end

  --
  -- **What opens with the desktop**: every application, ticked or not,
  -- eight rows showing and the rest a scroll away, across the whole card.
  -- This page is the only place it is chosen since Startup Apps, a window
  -- that did the same, went on 5 October (`roadmap.md`, *One kit, one
  -- door*); `wm preferences:startup` opens it.
  --
  -- What the Deskbar can start, from where the Deskbar reads it
  -- (`deskbarmenu.programs`): a list that offered an application the
  -- Deskbar will not open at login - one with `section none`, as Info is -
  -- was a tick that did nothing.
  --
  if it.kind == "startup" then
    local names, ticked = {}, {}

    for short in pairs(use("/Kosmos/Libraries/deskbarmenu.lua").programs(fs)) do
      names[#names + 1] = short
    end

    table.sort(names)

    for _, name in ipairs(use("/Kosmos/Libraries/startup.lua").items()) do
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

                      local ok, why = use("/Kosmos/Libraries/prefs.lua").write(
                        settings.STARTUP, { items = items })

                      -- A refusal reaches the log, as it reached Startup
                      -- Apps' own window when that was where this was set.
                      if not ok then
                        print("preferences: what opens with the desktop was "
                              .. "not kept: " .. tostring(why))
                      end
                    end }
  end

  if it.kind == "choice" and it.choices then
    return ui.dropdown{ x = x, y = y, choices = it.choices,
                        value = settings.get(it),
                        on_change = function(_, v)
                          if live(it, v) then
                            local ok, why = settings.set(it, v)

                            -- What a file type now opens with, said: the
                            -- display harness chooses one and reads this.
                            if it.tag then
                              print(("preferences: %s opens with %s%s"):format(
                                    it.tag, tostring(v),
                                    ok and "" or (" - not kept: " .. tostring(why))))
                            end
                          end
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
-- machine calls itself and `/Devices/memory` for its size - the same three doors
-- `neofetch` reads, so the two programs cannot disagree about what this
-- machine is. A second way of asking would be a second answer eventually.
--
local function facts()
  local b = sys.build() or {}
  local mem = fs.read("/Devices/memory") or {}
  local info = sys.info() or {}
  local screen = fs.read("/Devices/screen") or {}
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
  local net = fs.net_info and fs.net_info("/Network") or nil

  -- An address as a person writes it, or "none" for one not given yet.
  local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
  local card = net and net.card

  out.net_card = driven[1] and driven[1].name or "No card found"
  out.net_address = card and ipv4.given(net.address) and ipv4.text(net.address) or "none"
  out.net_gateway = card and ipv4.given(net.gateway) and ipv4.text(net.gateway) or "none"

  return out
end

local fact = facts()

local function value_text(it)
  -- The option's name is in the note; this is the part that is not obvious
  -- from reading the row, and the part a long note must not squeeze out.
  if it.kind == "fact" then return fact[it.fact] or "-" end

  -- A file type only one application opens: its name, not a choice.
  if it.kind == "value" then return tostring(it.value or "") end
  if it.kind == "volume" or it.kind == "mute" then return "No sound device" end
  if it.kind == "brightness" then return "Not on this screen" end
  if it.kind == "pointer_speed" then return "Not for a tablet" end

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
-- **The page scrolls when it is taller than the window**, as File types is -
-- a row for every type an application opens (`roadmap.md` 6z). `scroll` is
-- how far down it is, and every row is placed that much higher; the header
-- is drawn after the page, so what goes up passes under it.
--
function page:wheel(n)
  local most = math.max(0, page_h - self.h)
  local to = math.max(0, math.min(most, scroll - n * ui.WHEEL_ROWS * 16))

  if to == scroll then return false end

  scroll = to
  rebuild()

  return true
end

--
-- **File types' Find**, the drawing's field above its groups: the rows whose
-- type, name or application has the words in them. Kept across rebuilds, so
-- what is typed and where the caret is survive each one.
--
local TAG_W = 62                   -- the drawing's column for `.mp4`
local filter = tostring(args or ""):match("%-%-find%s+(%S+)") or ""
local find = ui.field{ w = BODY_W, text = filter,
                       hint = "Find a type - mp4, photo, zip", icon = "search" }

find.on_change = function(_, text)
  filter, scroll = text, 0
  rebuild()
  win:focus_on(find)
end

--
-- Build the page for `showing`.
--
rebuild = function()
  for i = #page.children, 1, -1 do page.children[i] = nil end
  cards = {}

  local made_by_applications, made_by_notifications = false, false

  for _, c in ipairs(settings.CATEGORIES) do
    if c.id == showing then
      header.title = c.name
      made_by_applications = c.from_applications
      made_by_notifications = c.from_notifications
    end
  end

  local cx, width = column(page.w)
  local y = HEAD + BODY_TOP - scroll

  fact = facts()
  row_room = width - 2 - 2 * ROW_IN
  now_label = nil

  local groups

  if made_by_applications then
    find.x, find.y, find.w = cx, y, width
    page:add(find)
    y = y + find.h + 16                 -- the drawing's margin under it

    groups = types.page(nil, nil, filter)
  else
    groups = settings.groups(showing)
  end

  -- And every application that has said something, a switch each.
  if made_by_notifications then
    local apps = settings.notifiers(notify.all(0), notify.who, notify.key)

    if #apps.items > 0 then groups[#groups + 1] = apps end
  end

  for gi, group in ipairs(groups) do
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

      -- A control that goes under the row's words, across the card - the
      -- looks' gallery - rather than beside them at the right.
      local below = c and c.below

      if below then
        h = 2 * ROW_PAD + words + ROW_PAD + c.h
        taken = 0
      end

      if below then
        c.x = cx + 1 + ROW_IN
        c.y = y + ROW_PAD + words + ROW_PAD
        page:add(c)
      elseif c then
        c.x = right - c.w
        c.y = y + (h - c.h) // 2
        page:add(c)

        -- A slider's place, in the window's points, for a harness - which
        -- can press and cannot aim.
        if it.kind == "percent" then
          print(("preferences slider: %s at %d,%d, %d wide"):format(
                it.key, SIDE + c.x, c.y + c.h // 2, 200))
        elseif it.kind == "pointer_speed" or it.kind == "double_click" then
          print(("preferences slider: %s at %d,%d, %d wide"):format(
                it.key, SIDE + c.x, c.y + 10, 200))
        elseif it.kind == "try_double_click" then
          print(("preferences folder at %d,%d"):format(SIDE + c.x + 19, c.y + 17))
        end
      elseif taken > 0 then
        local shown = ui.label{ x = right - taken, y = y + (h - ch) // 2,
                                w = taken + 2, text = value_text(it),
                                color = theme.text_dim, role = "ui" }

        page:add(shown)

        -- The clock's row is kept right on the window's tick, from the
        -- right edge it was placed against.
        if it.fact == "now" then now_label, shown.right = shown, right end
      end

      -- A file type's own column before its name, in the fixed face.
      local lx = cx + 1 + ROW_IN

      if it.tag then
        page:add(ui.label{ x = lx, y = y + (h - LINE_LABEL) // 2
                                   + (LINE_LABEL - gfx.height("mono")) // 2,
                           w = TAG_W, text = it.tag, role = "mono" })
        lx = lx + TAG_W
      end

      local room = right - (taken > 0 and taken + ROW_IN or 0) - lx

      --
      -- The name and its note as one block, centred in the row: each in a
      -- line of the drawing's height, the face centred in its line.
      --
      local top = below and (y + ROW_PAD) or (y + (h - words) // 2)

      -- The row's name in `label`: the drawing's 13.5 at weight 500.
      if it.label ~= "" then
        page:add(ui.label{
          x = lx,
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
          x = lx,
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

  --
  -- Where File types' choices are kept, under the last card, as the drawing
  -- says it: only what differs from the default, so a home carried to
  -- another machine takes its choices and nothing else.
  --
  --
  -- The applications' switches, where each is in the window's points: for
  -- a harness, which can press and cannot aim.
  --
  if made_by_notifications then
    local said = {}

    for _, c in ipairs(page.children) do
      if c.sender then
        said[#said + 1] = ("%s %d,%d %s"):format(c.sender, SIDE + c.x + c.w // 2,
                                                 c.y + c.h // 2, c.on and "on" or "off")
      end
    end

    print(("preferences: notifications, %d applications%s%s"):format(#said,
          #said > 0 and ": " or "", table.concat(said, "; ")))
  end

  if made_by_applications then
    --
    -- How many rows, and where the first choice is, in the window's points:
    -- for the display harness, which can type and cannot aim.
    --
    local n, first = 0, nil

    for _, g in ipairs(groups) do n = n + #g.items end

    for _, c in ipairs(page.children) do
      if not first and c.choices then first = c end
    end

    print(("preferences: file types, %d rows%s"):format(n,
          first and (", a choice at %d,%d"):format(
            SIDE + first.x + first.w // 2, first.y + first.h // 2) or ""))

    local words = use("/Kosmos/Libraries/prefs.lua").path(types.CHOICES) .. " - only the choices that differ from "
                  .. "the default"

    if #groups == 0 then words = "No type is called that." end

    page:add(ui.label{ x = cx + 4, y = y + 6, w = width,
                       text = words, color = theme.text_dim, role = "ui" })
    y = y + 6 + gfx.height()
  end

  page_h = y + scroll + BODY_TOP

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

-- A new size: the cards again at it, the scroll held inside what there is.
win.on_resize = function()
  scroll = math.max(0, math.min(scroll, page_h - page.h))
  rebuild()
  print(("preferences: resized to %dx%d"):format(page.w, page.h))
end

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
