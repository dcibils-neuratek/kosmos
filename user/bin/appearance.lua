-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Appearance
-- kosmos: section preferences
-- The look of the desktop: which palette, and what colour the ground is.
--
--   wm appearance
--
-- Haiku keeps this under Preferences and calls it Appearance, which is
-- where the name comes from.
--
-- Everything here is one message to the window manager. It holds the
-- palette because it is the one process already talking to every window;
-- it repaints the desktop and forwards the change, and each application's
-- widget kit updates its own colours without the application knowing. So
-- this program has no drawing code of its own beyond its own window, and
-- changing a colour here changes every window on screen.
--
-- The choice is written to `/home/.appearance` and read back at startup, so
-- it survives the power going off. That file is the first thing in this
-- system that is a *setting* rather than data, and it is stored as an
-- ordinary Lua table through the ordinary filesystem protocol.

local ui = use("/lib/ui.lua")

--
-- **Two columns, 668 by 530** (`docs/appearance.html`, `roadmap.md` 5b).
--
-- It was 380 by 580 with seven groups stacked in one column and lists three
-- rows deep, and Diego said what was wrong with it on 20 September: "it
-- needs a better design, spacing, and ordering of widgets since we added a
-- lot and is too packed and small". Drawn before it was rewritten, argued
-- with on the page, and agreed there: "1 two columns, 2 miniature desktop,
-- 3 yes, 4 nothing".
--
-- Wider and *shorter*: palette, desktop colour and wallpaper down the left,
-- type and the window's shape down the right, every list six rows deep
-- instead of three.
--
local W, H = 668, 530

local PAD    = 12
local GUTTER = 16
local LEFT_X,  LEFT_W = PAD, 286
local RIGHT_X = LEFT_X + LEFT_W + GUTTER
local RIGHT_W = W - RIGHT_X - PAD

local win, err = ui.window{ title = "Appearance", w = W, h = H,
                            x = 160, y = 110 }

if not win then
  print("appearance: " .. tostring(err))
  return
end

local SETTINGS = "/home/.appearance"

-- Desktop colours to choose from.
--
-- A fixed set rather than a full picker: three sliders and a hue wheel is a
-- lot of widget kit for a decision made once, and a row of swatches is how
-- this was done in 1998 anyway. Two rows - greys and blues that sit under a
-- dark palette, then lighter grounds for the light one.
local SWATCHES = {
  0xff1c2530, 0xff11161d, 0xff232b36, 0xff2b2b2b,
  0xff1b2a3a, 0xff223344, 0xff2a3f2a, 0xff3a2a3a,
  0xff336699, 0xff4a7ab0, 0xff6688aa, 0xff8899aa,
  0xff707070, 0xff909090, 0xffb0b0b0, 0xffd0d0d0,
}

local PER_ROW = 8              -- to a row; `SW` is derived below

--
-- The vertical layout, derived rather than typed.
--
-- These were eight numbers written by hand - the list at 30, its label at
-- 10, "Desktop" at 76, the swatches at 98 - and one of them was wrong: the
-- list is 58 tall from y=30, so it ends at 88, and "Desktop" sat at 76,
-- inside it.
--
-- Worth being exact about *why* it was wrong, because it was right when it
-- was written. This window lets you choose the interface font, from 14
-- pixels to 22. A label is one line of that font, so every one of these
-- gaps changes size when you use the thing this window is for - and a
-- layout of constants is a layout that is correct at exactly one font size.
--
--
-- **Measured, and measured again when the face changes.** `gfx.height()`
-- rather than `gfx.font.h`, because this is the window that *changes* the
-- face: a layout built from a number read once at startup is right until
-- somebody uses this panel for what it is for.
--
local function line_h() return gfx.height() end

local GAP     = 10
local ROWS    = 6                     -- how deep every list is
local SIZE_W  = 62                    -- the list of sizes beside the faces

--
-- **As tall as what it draws.** The first number here was a guess - six
-- lines and a bit - and the paragraph came out underneath the terminal
-- line at the sizes the desktop actually had. This is the same sum the
-- preview's drawing makes, in the same order, so the two cannot drift.
--
local function preview_h()
  return 20                                   -- the desktop around it
         + gfx.height("title") + 4 + 8        -- the tab
         + gfx.height("heading") + 6          -- the heading
         + line_h() + 8 + 8                   -- the buttons
         + 2 * line_h() + 4 + 8               -- the list
         + gfx.height("text") + 6             -- the paragraph
         + gfx.height("mono") + 4             -- the terminal
         + 8
end

--
-- **Every number the layout is made of, worked out in one place, and
-- worked out again when the faces or the theme change** (`layout`, at the
-- end of this file, runs it at startup and on every theme event).
--
-- These were constants computed once, when the panel opened: right at the
-- faces it opened with and wrong the moment somebody used it for what it is
-- for. On the ThinkPad, 22 September, Diego chose 16 for the widgets and
-- the terminal, and the role list's last row ran off the bottom of its box
-- while the status line - "not saved", and why - slid under the window
-- titles group, so the one sentence that said the choice had failed could
-- not be read. A list's height is its rows at the fixed layout's 24, which is
-- what showed three themes of seven under Plex.
--
local LH, LIST_H, RESET_H                     -- a label; a list; the button
local PAL_Y, LIST_Y, RESET_Y, DESK_Y          -- the left column
local SW, SWATCH_Y, BAR_Y, BAR_SW_Y, BAR_H_Y, BAR_H_H, WALL_Y, WALL_LIST
local FONTS_Y, ROLE_Y, ROLE_H, LISTS_Y        -- the right column
local FONT_W, PREVIEW_Y, PREVIEW_H
local TITLES_Y, SHAPE_Y, SHAPE_H
local TALL                                    -- the window, top to bottom

local function measure()
  local row = ui.metrics.row

  LH        = line_h() + 6
  LIST_H    = ROWS * row + 6
  RESET_H   = line_h() + 10

  PAL_Y     = PAD
  LIST_Y    = PAL_Y + LH
  RESET_Y   = LIST_Y + LIST_H + GAP
  DESK_Y    = RESET_Y + RESET_H + GAP
  SWATCH_Y  = DESK_Y + LH
  SW        = (LEFT_W - 7 * 4) // 8
  BAR_Y     = SWATCH_Y + 2 * (SW + 4) + GAP
  BAR_SW_Y  = BAR_Y + LH
  BAR_H_Y   = BAR_SW_Y + SW + 4 + 2
  BAR_H_H   = line_h() + 8
  WALL_Y    = BAR_H_Y + BAR_H_H + GAP
  WALL_LIST = WALL_Y + LH

  FONTS_Y   = PAD
  ROLE_Y    = FONTS_Y + LH
  ROLE_H    = #ui.theme.roles * (line_h() + 6) + 4
  LISTS_Y   = ROLE_Y + ROLE_H + GAP
  FONT_W    = RIGHT_W - SIZE_W - 8
  PREVIEW_Y = LISTS_Y + LIST_H + GAP
  PREVIEW_H = preview_h()

  -- The two little windows are 6 down and 22 tall, and their captions a
  -- line under them: 52 was that at a 16-pixel bitmap and clipped the
  -- captions at 16-pixel Plex Sans.
  TITLES_Y  = PREVIEW_Y + PREVIEW_H + GAP
  SHAPE_Y   = TITLES_Y + LH
  SHAPE_H   = 6 + 22 + 4 + line_h() + 6

  TALL = math.max(WALL_LIST + LIST_H, SHAPE_Y + SHAPE_H) + GAP + line_h()
         + PAD
end

measure()

--
-- Every theme this machine has: the ones compiled in, plus any `.theme`
-- file it finds on the disk.
--
-- A theme is text - `ui.md` and `theme.lua` describe the format - so
-- "install a theme" means putting a file somewhere, not rebuilding
-- anything. The shipped ones are in `/lib/themes.lua` in exactly the same
-- format, parsed by exactly the same parser, so the format is the thing
-- that ships rather than a thing bolted on beside it.
--
local theme = ui.theme
local complaints = {}

do
  local shipped = use("/lib/themes.lua")

  for _, name in ipairs(shipped.order) do
    local palette, said = theme.read(shipped[name], "dark")

    theme.install(name, palette)

    for _, why in ipairs(said) do
      complaints[#complaints + 1] = name .. ": " .. why
    end
  end

  -- And whatever is on the disk. A machine with no disk simply finds
  -- nothing, which is why this is not an error.
  for _, file in ipairs(fs.list("/system/themes") or {}) do
    if file:match("%.theme$") then
      local palette, said = theme.load("/system/themes/" .. file, "dark")

      if palette then
        local name = palette.name or file:gsub("%.theme$", "")

        theme.install(name, palette)

        for _, why in ipairs(said or {}) do
          complaints[#complaints + 1] = name .. ": " .. why
        end
      end
    end
  end
end

--
-- **The four looks, and only them** (`roadmap.md` 5y), in `themes.lua`'s
-- order and by the titles a person reads - not every palette this process
-- happens to hold, which listed the kit's own `dark` and `light` and any
-- file on the disk beside them. Diego: "Let's just make 3 or 4 good design
-- options in colors and fonts and stick to those".
--
local LOOKS = use("/lib/themes.lua")

local function theme_names()
  local names = {}

  for _, name in ipairs(LOOKS.order) do
    names[#names + 1] = LOOKS.titles[name] or name
  end

  return names
end

-- A look's name from the title its row shows, and back.
local function look_of(title)
  for _, name in ipairs(LOOKS.order) do
    if (LOOKS.titles[name] or name) == title then return name end
  end

  return title
end

local chosen_palette = "dark"
local chosen_desktop = nil     -- nil means "whatever the palette says"
local chosen_bar     = nil     -- the Deskbar's; nil is the theme's own
local chosen_bar_h   = nil     -- its height, likewise

-- Declared here rather than beside the font lists because `send` is written
-- before them and closes over both.
local chosen                   -- what each role is set to
local ROLES                    -- the four of them, defined with the lists

--
-- Which role the three lists are showing.
--
-- A *function* rather than a variable, and that is the fix for a real bug
-- rather than a preference. It was a variable initialised to "ui" while the
-- list beside it started on row 1, and row 1 stopped being "ui" the moment
-- window titles became a role of their own. So the panel showed "Window
-- titles" selected, and choosing a font set the widget font - the display
-- and the state disagreed from the first frame, before anybody clicked
-- anything, and each was internally consistent.
--
-- Two copies of one fact is the shape of that bug. There is one now, in the
-- list, and this reads it.
--
local role_list                -- defined with the other two, below
local reflect                  -- the lists catching up; defined with them

local function role()
  return ROLES[role_list.selected or 1].key
end
local chosen_font    = "spleen"
local chosen_px      = 16

-- What there is to choose from, asked for rather than listed: `gfx.fonts`
-- knows because the fonts are embedded beside it, and a list written here
-- would be a second place to keep in step.
local FONTS = gfx.fonts()

local status = ui.label{ x = PAD, y = H - 30, w = W - 2 * PAD,
                         text = "", color = "text_dim" }

-- The picture behind everything, or nil for none.
local chosen_wallpaper = nil

-- The title's shape: "beos", a tab as wide as the title, or "full", a bar
-- across the whole window - the window manager's `tabs`.
local chosen_tabs = "beos"

local function send()
  -- Both return values. `fs.send` answers `nil, reason` when the server
  -- said no, so a caller that looks only at the first one reports "no
  -- reply" for every refusal and throws away what was actually wrong.
  -- The palette *table*, not its name: the window manager forwards what it
  -- is given to every window, and a window cannot look up a theme that only
  -- ever existed as a file on this machine's disk.
  --
  -- The colours alone: a theme's table carries its faces as well, and they
  -- go as `fonts` below - once, as chosen, rather than twice in one
  -- 2048-byte message.
  --
  -- **And the spacing inside a widget with them** (`theme.spacing`), which
  -- this left out when spacing became part of a theme: Plex chosen here
  -- came with its colours and faces and without its padded rows, and only
  -- a restart - where the window manager reads the whole theme file - put
  -- them right. The panel's own relayout check found it by coming out
  -- shorter in Plex than in the harness's bitmap.
  --
  local colours = {}
  local chosen_theme = theme.palettes[chosen_palette] or {}

  for _, k in ipairs(theme.tokens) do colours[k] = chosen_theme[k] end
  for _, k in ipairs(theme.spacing) do colours[k] = chosen_theme[k] end

  local reply, why = fs.send("/app/wm", { type = "theme",
                                          palette = colours,
                                          desktop = chosen_desktop,
                                          bar = chosen_bar,
                                          bar_h = chosen_bar_h,
                                          fonts = chosen,
                                          tabs = chosen_tabs })

  if not reply then
    status.text = "refused: " .. tostring(why)
    return nil, why
  end

  -- Written only after the window manager accepted it, so the file cannot
  -- come to hold an appearance the system never managed to apply.
  local ok, werr = fs.write(SETTINGS, { palette = chosen_palette,
                                        desktop = chosen_desktop,
                                        bar = chosen_bar,
                                        bar_h = chosen_bar_h,
                                        wallpaper = chosen_wallpaper,
                                        fonts = chosen,
                                        tabs = chosen_tabs })

  --
  -- **A choice that could not be written down is said in the log as well.**
  -- The status line is the only other place it appeared, and on the
  -- ThinkPad on 22 September that line read "applied heading =
  -- ibmplexsans-semibold 16, not saved" with the reason under another
  -- group - so the one fact that explained why nothing survived a restart
  -- could not be read. `log appearance` finds this.
  --
  if not ok then
    print("appearance: not saved to " .. SETTINGS .. ": " .. tostring(werr))
  end

  status.text = ok and ("saved: " .. chosen_palette .. ", "
                        .. role() .. " = " .. chosen[role()].font .. " "
                        .. chosen[role()].px)
                or ("applied " .. role() .. " = " .. chosen[role()].font
                    .. " " .. chosen[role()].px .. ", not saved: "
                    .. tostring(werr))

  return reply
end

local theme_label = ui.label{ x = LEFT_X, y = PAL_Y, w = LEFT_W, text = "Theme" }
win:add(theme_label)

--
-- A list rather than a button per theme. Two buttons fitted while there
-- were two themes; there is no number of themes for which a row of buttons
-- is right, and a list is the widget that already knows how to be any
-- length.
--
--
-- **Choosing a theme chooses its faces too** (`roadmap.md` 5s). A theme
-- names a face and a size for every role, and picking one sets all five -
-- Diego: "a theme is a complete color scheme + font selection". The role
-- list below then says which roles somebody has changed since.
--
local function take_theme_faces(name)
  local p = theme.palettes[name]

  if not (p and p.fonts) then return end

  for _, r in ipairs(ROLES) do
    local f = p.fonts[r.key]

    if f then chosen[r.key] = { font = f.font, px = f.px } end
  end
end

local palette_list = ui.list{
  x = LEFT_X, y = LIST_Y, w = LEFT_W, h = LIST_H,
  items = theme_names(),
  on_select = function(_, item)
    chosen_palette = look_of(item)
    take_theme_faces(chosen_palette)
    reflect()
    send()
  end,
}

win:add(palette_list)

local desk_label = ui.label{ x = LEFT_X, y = DESK_Y, w = LEFT_W, text = "Desktop" }
win:add(desk_label)

-- The swatches, as a view that draws itself and answers a click.
--
-- Not sixteen buttons: a button is a bevel and a label and a focus ring,
-- and what this wants is a colour and nothing else. `gfx.md`'s rule holds
-- either way - the fills are C, and what Lua decides is where they go.
local swatches = ui.view{
  x = LEFT_X, y = SWATCH_Y, w = LEFT_W, h = 2 * (SW + 4),

  draw = function(self, g)
    for i, colour in ipairs(SWATCHES) do
      local col = (i - 1) % PER_ROW
      local row = (i - 1) // PER_ROW
      local x, y = col * (SW + 4), row * (SW + 4)

      g:fill(x, y, SW, SW, colour)

      if colour == chosen_desktop then
        g:frame(x, y, SW, SW, ui.theme.ring)
        g:frame(x + 1, y + 1, SW - 2, SW - 2, ui.theme.ring)
      else
        g:frame(x, y, SW, SW, ui.theme.line)
      end
    end
  end,

  on_click = function(self, x, y)
    local col = x // (SW + 4)
    local row = y // (SW + 4)
    local i = row * PER_ROW + col + 1

    if SWATCHES[i] then
      chosen_desktop = SWATCHES[i]
      send()
    end
  end,
}

win:add(swatches)

--
-- **The Deskbar's colour** (`roadmap.md` 5u). Diego, 22 September, on
-- seeing Plex's stone bar where BeOS's is yellow: "is that a setting?", and
-- then "keep the deskbar user selectable color". A theme says where it
-- starts; a colour here is kept over it and survives a restart, and the
-- words on the bar follow it (`theme.ink_on`), so none of these can leave
-- them unreadable. The first eight are the bars the themes that ship paint
-- - Plex's stone, BeOS's yellow, Plex's yellow, Photon's blue, Platinum's
-- grey, IRIX's khaki - and two darks. "Back to this theme" gives the
-- theme's own back.
--
local BAR_SWATCHES = {
  0xffe7e7e3, 0xffffcb00, 0xfff2c230, 0xff5786da,
  0xffcccccc, 0xffa59f80, 0xff2b2b2b, 0xff223344,
}

local bar_label = ui.label{ x = LEFT_X, y = BAR_Y, w = LEFT_W, text = "Deskbar" }
win:add(bar_label)

local bar_swatches = ui.view{
  x = LEFT_X, y = BAR_SW_Y, w = LEFT_W, h = SW + 4,

  draw = function(self, g)
    for i, colour in ipairs(BAR_SWATCHES) do
      local x = (i - 1) * (SW + 4)
      local on = colour == (chosen_bar or ui.theme.bar)

      g:fill(x, 0, SW, SW, colour)

      if on then
        g:frame(x, 0, SW, SW, ui.theme.ring)
        g:frame(x + 1, 1, SW - 2, SW - 2, ui.theme.ring)
      else
        g:frame(x, 0, SW, SW, ui.theme.line)
      end
    end
  end,

  on_click = function(self, x, _)
    local colour = BAR_SWATCHES[x // (SW + 4) + 1]

    if colour then
      chosen_bar = colour
      send()
    end
  end,
}

win:add(bar_swatches)

--
-- **And its height** (`roadmap.md` 5v): 36, as it has always been, and two
-- taller. Not shorter yet - its icons are 32 pixels and the compositor does
-- not scale a picture, so a shorter bar would cut them.
--
local BAR_HEIGHTS = { 36, 44, 52 }

local bar_heights = ui.view{
  x = LEFT_X, y = BAR_H_Y, w = LEFT_W, h = BAR_H_H,

  draw = function(self, g)
    local each = (self.w - 8 * (#BAR_HEIGHTS - 1)) // #BAR_HEIGHTS
    local now = chosen_bar_h or ui.theme.bar_h

    for i, px in ipairs(BAR_HEIGHTS) do
      local x = (i - 1) * (each + 8)
      local on = px == now
      local label = px .. " pixels"

      g:fill(x, 0, each, self.h, on and "accent" or "raised")
      g:frame(x, 0, each, self.h, "line")
      g:text(x + (each - gfx.measure(label)) // 2, (self.h - line_h()) // 2,
             label, on and "text_on" or "text")
    end
  end,

  on_click = function(self, x, _)
    local each = (self.w - 8 * (#BAR_HEIGHTS - 1)) // #BAR_HEIGHTS
    local px = BAR_HEIGHTS[math.min(#BAR_HEIGHTS, x // (each + 8) + 1)]

    if px then
      chosen_bar_h = px
      send()
    end
  end,
}

win:add(bar_heights)

--------------------------------------------------------------------------
-- Fonts, by role.
--
-- Three, because a titlebar, a paragraph and a terminal want different
-- faces and one setting for all of them was always going to be wrong: a
-- terminal's *must* be fixed-width whatever the other two are. Three is the
-- number of decisions somebody actually has.
--
-- Three lists rather than a grid of buttons. A list costs the same space
-- whatever is in it, scrolls when there is more, and answers the arrow
-- keys - and there are five fonts now because dropping one into
-- `assets/fonts/` is all it takes to add one.
--------------------------------------------------------------------------

ROLES = {
  { key = "title",   label = "Window titles" },
  { key = "ui",      label = "Widgets" },
  { key = "heading", label = "Headings" },
  { key = "text",    label = "Regular text" },
  { key = "mono",    label = "Terminal" },
}

local SIZES = { 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28 }

--
-- What each role is set to, starting from what the kit has in force rather
-- than from a list of constants: the panel's first frame should say what is
-- true, and `theme.fonts` is the answer the desktop gave this process when
-- its window opened.
--
chosen = {}

for _, r in ipairs(ROLES) do
  local have = theme.fonts[r.key] or {}

  chosen[r.key] = { font = have.font or "spleen", px = have.px or 16 }
end

--------------------------------------------------------------------------
-- The right column: type, and the window's shape.
--------------------------------------------------------------------------


--
-- **Every role, with the face it is set to.**
--
-- The list used to say `Widgets` and nothing else, so the panel could not
-- answer the question anybody opens it with - *what is my widget font?* -
-- without clicking each role in turn and reading the status line. Five rows
-- that each report are worth more than five rows that each label.
--
-- A view rather than `ui.list`, because a list row is one string and this
-- row is two: a name at the left and, quieter and right-aligned, the face.
--
--
-- **And whether it is the theme's.** A face somebody picked after choosing
-- a theme is theirs, and the row says so - otherwise a panel that sets all
-- five faces from the theme would hide which of them it did not set.
--
local function yours(key)
  local p = theme.palettes[chosen_palette]
  local t = p and p.fonts and p.fonts[key]
  local c = chosen[key]

  return t ~= nil and (t.font ~= c.font or t.px ~= c.px)
end

local function face_of(key)
  local c = chosen[key]
  local name = c.font:gsub("^ibmplex", "Plex "):gsub("sanscondensed", "Sans Condensed")
                     :gsub("^Plex sans", "Plex Sans"):gsub("^Plex mono", "Plex Mono")
                     :gsub("%-semibold", " SemiBold")
                     :gsub("%-bold", " Bold"):gsub("%-italic", " Italic")

  return name .. " " .. c.px .. (yours(key) and ", yours" or "")
end

role_list = ui.view{
  x = RIGHT_X, y = ROLE_Y, w = RIGHT_W, h = ROLE_H, selected = 1,

  draw = function(self, g)
    local row = line_h() + 6

    g:fill(0, 0, self.w, self.h, "sunken")
    g:frame(0, 0, self.w, self.h, "line")

    for i, r in ipairs(ROLES) do
      local y = 2 + (i - 1) * row
      local on = (i == self.selected)

      if on then g:fill(1, y, self.w - 2, row, "accent") end

      g:text(7, y + 3, r.label, on and "text_on" or "text")

      -- Right-aligned, and measured to get there.
      local said = face_of(r.key)

      g:text(self.w - 7 - gfx.measure(said), y + 3, said,
             on and "text_on" or "text_dim")
    end
  end,

  on_click = function(self, x, y)
    local at = y // (line_h() + 6) + 1

    if ROLES[at] then
      self.selected = at
      if self.on_select then self:on_select() end
    end
  end,
}


local font_list = ui.list{ x = RIGHT_X, y = LISTS_Y, w = FONT_W,
                           h = LIST_H, items = FONTS }
local size_list = ui.list{ x = RIGHT_X + FONT_W + 8, y = LISTS_Y,
                           w = SIZE_W, h = LIST_H, items = {} }

for i, px in ipairs(SIZES) do size_list.items[i] = tostring(px) end

--
-- **The display catches up with the state, in one place.**
--
-- The palette row is set here and nowhere else, and that is a fix rather
-- than tidiness: it used to be selected from `chosen_palette` where the
-- list was built, which is *before* the saved settings are read - so a
-- machine with `beos` saved opened this panel with `dark` highlighted and
-- the status line underneath saying `in force: beos`. Two copies of one
-- fact, which is the same bug the note above `role()` describes, in the
-- widget beside it.
--
function reflect()
  local c = chosen[role()]

  for i, title in ipairs(palette_list.items) do
    if look_of(title) == chosen_palette then palette_list.selected = i end
  end

  --
  -- **By the rule the face was found by**, an exact name and then a prefix
  -- (`gfx.c`'s `font_asset`). A theme names Plex's title face
  -- `ibmplexsanscondensed`, which is the start of the one file there is,
  -- `ibmplexsanscondensed-semibold`; matched exactly, nothing was
  -- highlighted and the list kept whatever row it had - `spleen`, for a
  -- title drawn in Plex (22 September, the first picture of Plex).
  --
  local exact, prefix

  for i, f in ipairs(FONTS) do
    if f == c.font then exact = i end
    if not prefix and f:sub(1, #c.font) == c.font then prefix = i end
  end

  font_list.selected = exact or prefix or font_list.selected

  for i, px in ipairs(SIZES) do
    if px == c.px then size_list.selected = i end
  end
end

role_list.on_select = function()
  -- Nothing to assign: `role()` reads the selection. Only the two lists
  -- beside it have to catch up.
  reflect()
end

font_list.on_select = function(self, item)
  chosen[role()].font = item
  send()
end

size_list.on_select = function(self, item)
  chosen[role()].px = tonumber(item) or chosen[role()].px
  send()
end

local fonts_label = ui.label{ x = RIGHT_X, y = FONTS_Y, w = RIGHT_W, text = "Fonts" }
win:add(fonts_label)
win:add(role_list)
win:add(font_list)
win:add(size_list)

--------------------------------------------------------------------------
-- **The preview: a desktop in miniature.**
--
-- Diego chose this over a line of sample text, knowing what it cost - "2
-- miniature desktop" - and it is the right choice for a reason the cheaper
-- one cannot reach: a palette and a face are chosen *together*, and the
-- question is never "what does 15-pixel Plex Sans Condensed look like", it
-- is "what does my desktop look like now". So this draws one: a window with
-- a title, a heading, two buttons, a list, a paragraph and a terminal line,
-- in the colours and the faces in force.
--
-- It needs no state of its own and no "apply" button, because **this panel
-- applies as you choose** - every list here sends the change the moment it
-- is selected. So the miniature is drawn with the live theme and is
-- therefore never a promise about something that has not happened.
--------------------------------------------------------------------------


local preview = ui.view{
  x = RIGHT_X, y = PREVIEW_Y, w = RIGHT_W, h = PREVIEW_H,

  draw = function(self, g)
    -- The ground: the desktop's own colour, which is the thing being
    -- chosen two groups to the left.
    g:fill(0, 0, self.w, self.h, chosen_desktop or theme.desktop)

    local x, y = 10, 10
    local w, h = self.w - 20, self.h - 20

    g:fill(x, y, w, h, "window")
    g:frame(x, y, w, h, "line")

    -- A tab as wide as its title, or a bar across: whichever is chosen.
    local title = "Tracker"
    local tw = (chosen_tabs == "full") and w
               or (gfx.measure(title, "title") + 20)
    local th = gfx.height("title") + 4

    g:fill(x, y, tw, th, "tab")
    g:frame(x, y, tw, th, "line")
    g:text(x + 8, y + 2, title, "tab_text", nil, "title")

    local iy = y + th + 8

    g:text(x + 8, iy, "Library", "text", nil, "heading")
    iy = iy + gfx.height("heading") + 6

    -- Two buttons, in the widget face.
    local bw = gfx.measure("New folder") + 18
    local bh = line_h() + 8

    g:raised(x + 8, iy, bw, bh, theme.raised)
    g:text(x + 17, iy + 4, "New folder", "text")
    g:raised(x + 8 + bw + 6, iy, gfx.measure("Delete") + 18, bh, theme.raised)
    g:text(x + 8 + bw + 15, iy + 4, "Delete", "text")
    iy = iy + bh + 8

    -- A list with a row selected in it.
    local lh = 2 * line_h() + 4

    g:fill(x + 8, iy, w - 16, lh, "sunken")
    g:frame(x + 8, iy, w - 16, lh, "line")
    g:fill(x + 9, iy + 2, w - 18, line_h(), "accent")
    g:text(x + 13, iy + 2, "Deskbar", "text_on")
    g:text(x + 13, iy + 2 + line_h(), "magicword-clip.mp4", "text")
    iy = iy + lh + 8

    -- A paragraph, and a line of terminal.
    g:text(x + 8, iy, "A paragraph, in the reading face.", "text", nil, "text")
    iy = iy + gfx.height("text") + 6

    g:fill(x + 8, iy, w - 16, gfx.height("mono") + 4, 0xff101010)
    g:text(x + 12, iy + 2, "kosmos> play film.mp4", 0xffc8e6c9, nil, "mono")
  end,
}

win:add(preview)

--
-- **Back where it belongs.** This resets the desktop's colour to whatever
-- the palette says, and it sat under the *fonts* - three groups from the
-- thing it affects - with a name, "Palette default", that did not say what
-- pressing it would do.
--
--
-- And since a theme names its faces as well, "back to the theme" means
-- both: the ground it paints and the five faces it sets.
--
local reset_button = ui.button{
  x = LEFT_X, y = RESET_Y, w = LEFT_W, h = RESET_H,
  text = "Back to this theme",
  on_click = function()
    chosen_desktop = nil
    chosen_bar = nil
    chosen_bar_h = nil
    take_theme_faces(chosen_palette)
    reflect()
    send()
  end,
}

win:add(reset_button)
win:add(status)

-- What is in force now, so the window opens saying the truth rather than a
-- guess. The file is the record of what was chosen; a machine with no disk
-- simply has not chosen anything.
local saved = fs.read(SETTINGS)

if type(saved) == "table" then
  chosen_palette = saved.palette or chosen_palette
  chosen_desktop = saved.desktop
  chosen_bar = math.type(saved.bar) == "integer" and saved.bar or nil
  chosen_bar_h = math.type(saved.bar_h) == "integer" and saved.bar_h or nil
  chosen_wallpaper = saved.wallpaper
  chosen_tabs = (saved.tabs == "full") and "full" or "beos"
  if type(saved.fonts) == "table" then
    for _, r in ipairs(ROLES) do
      local c = saved.fonts[r.key]

      if type(c) == "table" then
        chosen[r.key].font = c.font or chosen[r.key].font
        chosen[r.key].px   = c.px or chosen[r.key].px
      end
    end
  end

  reflect()
  status.text = "in force: " .. tostring(chosen_palette)
else
  status.text = "in force: dark (nothing saved yet)"
end

--------------------------------------------------------------------------
-- The wallpaper.
--
-- Whatever pictures are in `/home`, by name, then the ones the image carries
-- (`assets/wallpapers/`, in a `FULL=1` image), by the photographer who took
-- each - and "none" to go back to the flat colour above.
--
-- **Centred, never stretched**, which the window manager does and this only
-- names: an image the size of the screen lands exactly, a smaller one sits
-- in the middle on the desktop colour, and a larger one is cropped to its
-- middle. Scaling would put every wallpaper through a resampler to serve
-- the ones that do not fit, and lose sharpness on the ones that do.
--
-- PNG and JPEG. A photograph is the one thing on this machine that is
-- genuinely a photograph, and the format is the difference between three
-- megabytes and three hundred kilobytes for the same picture - which on a
-- 32 MB disk is the difference between a handful of wallpapers and a
-- library of them.
--------------------------------------------------------------------------

-- In the left column, under the ground's colour: the picture that goes
-- on it. `WALL_Y` and `WALL_LIST` are worked out with the rest of that
-- column, at the top of this file.

local wall_label = ui.label{ x = LEFT_X, y = WALL_Y, w = LEFT_W, text = "Wallpaper" }
win:add(wall_label)

-- What each line of the list stands for: a label, and the name the window
-- manager is sent - a path in `/home`, or `wallpaper/<file>` in the image.
local wall_path = { none = false }

-- `alexander-slattery-LI748t0BK8w.jpg` is Alexander Slattery: the words
-- before Unsplash's eleven-character photo id, each capitalised unless it
-- has a digit in it, which is how a username like `v2osk` is written.
local UNSPLASH_ID = string.rep("[%w_%-]", 11)

local function photographer(name)
  local who = name:match("^wallpaper/(.+)%-" .. UNSPLASH_ID .. "%.jpg$")

  if not who then return nil end

  local words = {}

  for word in who:gmatch("[^%-]+") do
    words[#words + 1] = word:find("%d") and word
                        or (word:sub(1, 1):upper() .. word:sub(2))
  end

  return table.concat(words, " ")
end

local function wallpapers()
  local out = { "none" }

  for _, name in ipairs(fs.list("/home") or {}) do
    local suffix = name:lower():match("%.([%a]+)$")

    if suffix == "png" or suffix == "jpg" or suffix == "jpeg" then
      out[#out + 1] = name
      wall_path[name] = "/home/" .. name
    end
  end

  for _, name in ipairs(sys.asset() or {}) do
    local who = photographer(name)

    if who and not wall_path[who] then
      out[#out + 1] = who
      wall_path[who] = name
    end
  end

  return out
end

local wall_list = ui.list{
  x = LEFT_X, y = WALL_LIST, w = LEFT_W, h = LIST_H,
  items = wallpapers(),
  on_select = function(_, item)
    chosen_wallpaper = wall_path[item] or nil

    local reply, why = fs.send("/app/wm", { type = "wallpaper",
                                            path = chosen_wallpaper })

    if not reply then
      status.text = "wallpaper: " .. tostring(why)
      return
    end

    -- Saved through the same door as everything else here, so one file is
    -- the record of the whole appearance.
    send()
  end,
}

for i, n in ipairs(wall_list.items) do
  if chosen_wallpaper and chosen_wallpaper == wall_path[n] then
    wall_list.selected = i
  end
end

win:add(wall_list)

--
-- **Window titles: BeOS's tab, or a bar across the whole window.** Diego,
-- 18 September: "can we have a appearance setting to switch between full
-- tab like windows or linux or beos". The tab is the default, being the
-- one he prefers; the window manager draws either (`tabs` in `wm.lua`).
--
--
-- **Drawn, not described.** It was two sentences in a list - "A tab as wide
-- as the title, as BeOS drew it" - which is a picture explained in words to
-- somebody who can see. Two small windows say it in no words at all, and
-- the mockup Diego agreed shows them side by side.
--
local TITLES = {
  { key = "beos", text = "A tab, as BeOS drew it" },
  { key = "full", text = "A bar across the window" },
}

local titles_label = ui.label{ x = RIGHT_X, y = TITLES_Y, w = RIGHT_W,
                               text = "Window titles" }
win:add(titles_label)

local shapes = ui.view{
  x = RIGHT_X, y = SHAPE_Y, w = RIGHT_W, h = SHAPE_H,

  draw = function(self, g)
    local each = (self.w - 8) // 2

    for i, t in ipairs(TITLES) do
      local x = (i - 1) * (each + 8)
      local on = (chosen_tabs == t.key)

      g:fill(x, 0, each, self.h, "sunken")
      g:frame(x, 0, each, self.h, on and ui.theme.ring or ui.theme.line)

      if on then g:frame(x + 1, 1, each - 2, self.h - 2, ui.theme.ring) end

      -- A window, an inch tall: its ground, and a tab or a bar on it.
      local wx, wy, ww, wh = x + 8, 6, each - 16, 22

      g:fill(wx, wy, ww, wh, "window")
      g:frame(wx, wy, ww, wh, "line")
      g:fill(wx, wy, (t.key == "full") and ww or (ww // 3), 7, "tab")

      g:text(x + 8, wy + wh + 4, t.text, on and "text" or "text_dim")
    end
  end,

  on_click = function(self, x, _)
    local each = (self.w - 8) // 2
    local t = TITLES[(x > each) and 2 or 1]

    if t and t.key ~= chosen_tabs then
      chosen_tabs = t.key
      send()
    end
  end,
}
win:add(shapes)

--
-- **And the window is as tall as the two columns turned out to be.**
--
-- `H` above is a starting size, because `ui.window` is asked for one before
-- this process has been told what the desktop's faces are - the reply to
-- that request is what carries them. Everything below it is measured, so
-- the real height is only known here, and a panel whose own window is the
-- wrong size for the font it is setting would be a poor advertisement.
--
-- This is also why it is a *resize* rather than a better constant: pick a
-- 24-pixel widget font in this window and the columns grow, and the window
-- has to grow with them. **Wired up on 22 September**, which was
-- `roadmap.md` 5b's remaining half: `win.on_theme`, which the kit calls
-- after it has applied a theme event, lays the panel out again.
--
local said_layout = false

local function layout()
  measure()

  theme_label.y = PAL_Y
  palette_list.y, palette_list.h = LIST_Y, LIST_H
  reset_button.y, reset_button.h = RESET_Y, RESET_H
  desk_label.y = DESK_Y
  swatches.y, swatches.h = SWATCH_Y, 2 * (SW + 4)
  bar_label.y = BAR_Y
  bar_swatches.y, bar_swatches.h = BAR_SW_Y, SW + 4
  bar_heights.y, bar_heights.h = BAR_H_Y, BAR_H_H
  wall_label.y = WALL_Y
  wall_list.y, wall_list.h = WALL_LIST, LIST_H

  fonts_label.y = FONTS_Y
  role_list.y, role_list.h = ROLE_Y, ROLE_H
  font_list.y, font_list.h, font_list.w = LISTS_Y, LIST_H, FONT_W
  size_list.x, size_list.y, size_list.h = RIGHT_X + FONT_W + 8, LISTS_Y,
                                          LIST_H
  preview.y, preview.h = PREVIEW_Y, PREVIEW_H
  titles_label.y = TITLES_Y
  shapes.y, shapes.h = SHAPE_Y, SHAPE_H

  -- Under both columns, the whole width: nothing sits beside it now that
  -- the columns' heights are the ones in force.
  status.y = TALL - line_h() - PAD + 2

  if TALL ~= win.h then win:resize(W, TALL) end

  --
  -- Said out loud, because the interesting part is invisible: a panel whose
  -- height *follows the faces* looks exactly like one whose height is a
  -- constant that happens to fit. The line is what a test can hold to, and
  -- it is how `display`'s `appearance` phase knows this laid itself out
  -- rather than guessed (`testing.md`). Once, at the start; a relayout
  -- says so in the same words, so a test can watch one happen.
  --
  -- **The window's own size, not the sum that asked for it.** Printing
  -- `TALL` would say the arithmetic happened, which is not the thing worth
  -- knowing: the regression this guards against is a resize that never
  -- reached the window manager, and after one that did, `win.h` is what the
  -- window manager agreed to.
  print(("appearance: %s%dx%d, %d roles, %s"):format(
        said_layout and "laid out again, " or "", win.w, win.h, #ROLES,
        chosen_palette))
  said_layout = true
end

layout()

-- And every time the desktop's faces or theme change - including the
-- change this panel has just asked for.
function win:on_theme() layout() end

--
-- **`wm appearance:--theme plex` - a theme chosen from the command line**,
-- by exactly the path a click on its row takes: its colours, its five
-- faces, the window manager told, and `/home/.appearance` written. So a
-- script can set the look, and a test can choose a theme without aiming
-- the pointer at a row in a list that scrolls.
--
-- The line it prints is the window manager's answer rather than the
-- request: `held` is what that process actually loaded, so a theme that
-- names a face the image does not carry says so here instead of looking
-- right in the panel and drawing in the previous face.
--
--
-- **`--bar-height 44`**, its height the way a click chooses it.
--
do
  local px = tonumber((args or ""):match("%-%-bar%-height%s+(%d+)"))

  if px then
    chosen_bar_h = px

    local reply, why = send()

    print(("appearance: bar height %d %s"):format(px,
          reply and "applied" or ("refused: " .. tostring(why))))
  end
end

--
-- **`--bar rrggbb`**, the Deskbar's colour the way a click on a swatch
-- chooses it, for the same two reasons.
--
do
  local hex = (args or ""):match("%-%-bar%s+#?(%x%x%x%x%x%x)")

  if hex then
    chosen_bar = 0xff000000 | tonumber(hex, 16)

    local reply, why = send()

    print(("appearance: bar %06x %s"):format(chosen_bar & 0xffffff,
          reply and "applied" or ("refused: " .. tostring(why))))
  end
end

do
  local want = (args or ""):match("%-%-theme%s+(%S+)")

  if want and not theme.palettes[want] then
    print("appearance: no theme called " .. want)
  elseif want then
    chosen_palette = want
    take_theme_faces(want)
    reflect()

    local reply, why = send()
    local held = {}

    for _, r in ipairs(ROLES) do
      local f = reply and reply.held and reply.held[r.key]

      held[#held + 1] = r.key .. "=" .. (f and (f.font .. "/" .. f.px) or "?")
    end

    print(("appearance: theme %s %s, held %s"):format(want,
          reply and "applied" or ("refused: " .. tostring(why)),
          table.concat(held, " ")))
  end
end

win:run()
