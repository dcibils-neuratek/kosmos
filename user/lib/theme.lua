-- The palette, in one file, and now more than one of them.
--
-- `ui.md` 16.8b, which was reversed in September 2026: the look is
-- dimensional on purpose. It used to say flat surfaces and one-pixel
-- separators, with weight and spacing doing the work a bevel used to do -
-- and that mistook the bevel for decoration. It is not decoration. Raised
-- means you can press this, sunken means content lives in here, and a
-- groove means these two things are separate: a two-pixel sentence about
-- what a thing does, read without looking straight at it.
--
-- So every palette carries `edge_light` and `edge_dark` as well as its
-- surfaces. A raised control takes the light edge on its top and left and
-- the dark edge on its bottom and right; a sunken one swaps them, which is
-- the whole trick and the reason the two are named rather than derived.
--
-- Derived would have been tempting and wrong: on the dark palette the
-- highlight is *not* simply a lightened surface - a flat lightening reads
-- as fog rather than as an edge - and on the light one the highlight is
-- pure white while the shadow is a mid grey, which no single formula gives.
--
-- There are two palettes, and the second is unashamedly the 1998 grey.
--
-- **The table is mutated in place, never replaced.** Every widget in
-- `ui.lua` reads `theme.text` at the moment it draws, so changing the
-- fields of this one table changes what the next repaint looks like -
-- across every widget, without any of them subscribing to anything. A new
-- table would leave every existing reference pointing at the old one, and
-- the theme would change only for windows opened afterwards. That is the
-- whole mechanism and it is one sentence, which is why it is written down
-- here rather than discovered later.

local theme = {}

--------------------------------------------------------------------------
-- The palettes.
--------------------------------------------------------------------------

theme.palettes = {}

-- Dark: what Kosmos looked like first, and still the default.
theme.palettes.dark = {
  name      = "dark",

  desktop   = 0xff1c2530,
  window    = 0xff161b22,
  raised    = 0xff21262d,
  sunken    = 0xff0d1117,

  line      = 0xff30363d,
  line_soft = 0xff21262d,

  -- The two edges a bevel is made of. Top-left light, bottom-right dark,
  -- and swapped for a sunken well.
  edge_light = 0xff424a55,
  edge_dark  = 0xff05080c,

  text      = 0xffc9d1d9,
  text_dim  = 0xff8b949e,
  text_on   = 0xff0d1117,

  tab       = 0xffffc700,

  -- An unfocused window's whole decoration, so it has to read as grey
  -- against the desktop rather than merge into it. The palette's darker
  -- greys are for surfaces *inside* a window, where there is a window
  -- behind them; this sits on the desktop.
  tab_idle  = 0xffb8b8b8,
  tab_text  = 0xff101010,

  -- Ink for a label lying on the desktop itself, which is not the ink for
  -- a label in a window and cannot be. `text` is chosen to read against
  -- `window`; the desktop is a colour the user picks, and every default
  -- here and in `themes.lua` picks a mid-to-dark blue, so black text on it
  -- is unreadable in exactly the themes that look best. Same reason
  -- `tab_text` is separate: the title bar is not a window surface either.
  desktop_text = 0xffffffff,

  --
  -- A terminal is black, in every theme, and that is not laziness.
  --
  -- It used to draw on `sunken`, which is the colour of a well - white in
  -- every light theme - and a white terminal is not a light-themed terminal,
  -- it is a text editor with a prompt in it. A console has been dark since
  -- it was a phosphor tube, the programs that write into one assume it, and
  -- the one thing a terminal must not do is make its own output hard to
  -- read.
  --
  -- Tokens rather than a literal in `terminal.lua`, so a theme that really
  -- does want a paper-white console can say so - but it has to say so.
  --
  console      = 0xff0b0b0b,
  console_text = 0xffd8d8d8,

  accent    = 0xff1f6feb,
  good      = 0xff3fb950,
  bad       = 0xffda3633,
  ring      = 0xff58a6ff,

  stamp     = 0xff3d4a58,
}

-- Light: the 1998 one, on purpose.
--
-- The panel grey is BeOS's own #d8d8d8 rather than something near it, and
-- the yellow is the same yellow the dark palette uses - it was always the
-- inherited colour and it does not need to change to sit on grey.
--
-- Text is black rather than a dark grey. On a light panel a "softer" near
-- black is the thing that reads as a smudge, which is the opposite of what
-- it does on a dark one.
theme.palettes.light = {
  name      = "light",

  desktop   = 0xff336699,          -- the classic desktop blue
  window    = 0xffd8d8d8,          -- BeOS panel grey
  raised    = 0xffe4e4e4,
  sunken    = 0xffffffff,

  line      = 0xff8c8c8c,
  line_soft = 0xffbfbfbf,

  -- Pure white against a mid grey, which is what makes the grey read as
  -- moulded rather than merely shaded.
  edge_light = 0xffffffff,
  edge_dark  = 0xff707070,

  text      = 0xff000000,
  text_dim  = 0xff5a5a5a,
  text_on   = 0xffffffff,

  tab       = 0xffffc700,

  -- Darker than the panel grey it sits next to, or an unfocused window
  -- has no edge at all on a light desktop.
  tab_idle  = 0xffb0b0b0,
  tab_text  = 0xff101010,

  desktop_text = 0xffffffff,

  -- Dark here too. See the note in the dark palette: a terminal is a
  -- terminal in every theme.
  console      = 0xff0b0b0b,
  console_text = 0xffd8d8d8,

  accent    = 0xff2d5faf,
  good      = 0xff1a7f37,
  bad       = 0xffbb2222,
  ring      = 0xff2d5faf,

  stamp     = 0xff97b4d1,
}

--------------------------------------------------------------------------
-- Themes as text.
--
-- GTK's idea, and the useful half of it. GTK2 kept themes in `gtkrc` files
-- naming *semantic roles* - `bg[NORMAL]`, `fg[ACTIVE]` - so a theme was
-- data a person could write rather than code somebody had to compile. That
-- is exactly the shape wanted here, and this file already had the hard
-- half: the tokens below are named for what they mean rather than for what
-- they colour, which is `ui.md` 16.9 arriving at GTK's answer independently.
--
-- What is deliberately *not* taken is GTK3's CSS - selectors, a cascade and
-- specificity, which is a constraint solver's worth of machinery, and
-- `ui.md` 16.4 already refuses one of those for layout. A theme here is a
-- flat list of key and value with no rules about which one wins, because
-- there is only ever one.
--
-- Also not taken: GTK's *engines*. A GTK theme could ship drawing code as a
-- shared object the toolkit loaded. `layout.md` records that Kosmos has no
-- dynamic linking, and more to the point the drawing vocabulary is the
-- kit's: `gc:raised`, `gc:sunken`, `gc:groove`. A theme picks the colours,
-- never the algorithm.
--
-- The format, in full:
--
--   # a comment
--   name       = Photon
--   desktop    = #a4b8cc
--   edge_light = #ffffff
--
-- A file may set as few tokens as it likes; anything it does not mention is
-- inherited from the palette it is based on. That is what makes "the dark
-- theme but with a green desktop" a three-line file instead of a copy of
-- twenty values that then drifts.
--------------------------------------------------------------------------

-- Every token a palette has, so a typo in a theme file can be *told* rather
-- than silently ignored - which is the failure mode that makes text
-- configuration miserable everywhere it is miserable.
theme.tokens = {
  "name", "desktop", "window", "raised", "sunken", "line", "line_soft",
  "edge_light", "edge_dark", "text", "text_dim", "text_on",
  "tab", "tab_idle", "tab_text", "desktop_text",
  "console", "console_text",
  "accent", "good", "bad", "ring", "stamp",
}

local known = {}

for _, k in ipairs(theme.tokens) do known[k] = true end

--
-- `#rrggbb` or `#aarrggbb` to the 0xAARRGGBB this system draws with.
--
-- Opaque unless the file says otherwise: a theme that writes six digits
-- means a colour, not a colour that is invisible because alpha defaulted to
-- zero. That is a one-character mistake with a completely blank window as
-- its symptom.
--
local function colour(v)
  local hex = v:match("^#(%x+)$")

  if not hex then return nil end

  if #hex == 6 then
    return 0xff000000 | tonumber(hex, 16)
  elseif #hex == 8 then
    return tonumber(hex, 16)
  end

  return nil
end

--
-- Read a theme from text. Returns a palette table, plus a list of
-- complaints - lines that were not understood and keys that are not
-- tokens. The caller decides whether to care; `appearance` shows them,
-- because a theme that silently half-loaded is worse than one that says
-- which line it could not read.
--
function theme.read(text, base)
  local out = {}
  local said = {}
  local n = 0

  for k, v in pairs(theme.palettes[base or "dark"] or {}) do out[k] = v end

  for line in tostring(text or ""):gmatch("([^\n]*)\n?") do
    n = n + 1

    -- Comments and blank lines, and a comment may follow a value.
    local body = line:gsub("#%s.*$", ""):match("^%s*(.-)%s*$")

    if body ~= "" then
      local key, value = body:match("^([%w_]+)%s*=%s*(.-)$")

      if not key then
        said[#said + 1] = ("line %d: not `key = value`"):format(n)
      elseif not known[key] then
        said[#said + 1] = ("line %d: no token called `%s`"):format(n, key)
      elseif key == "name" then
        out.name = value
      else
        local c = colour(value)

        if c then
          out[key] = c
        else
          said[#said + 1] =
            ("line %d: `%s` is not #rrggbb"):format(n, value)
        end
      end
    end
  end

  return out, said
end

--
-- Read one from the namespace, so a theme can be a file on the disk that
-- nobody rebuilt anything to install.
--
function theme.load(path, base)
  local text = fs.read(path)

  if type(text) ~= "string" then
    return nil, tostring(path) .. ": no such theme"
  end

  return theme.read(text, base)
end

--
-- The palette in force, as a flat table of just the tokens.
--
-- This is what crosses to every window when the appearance changes, and it
-- has to be the *values* rather than the name. A name only works while both
-- sides already hold the same palettes - which was true when there were two
-- of them compiled in, and stopped being true the moment a theme could be a
-- file somebody wrote. An application cannot look up a theme it has never
-- read.
--
-- Twenty numbers and a string, which is nothing against a 2048-byte
-- message, and it means a theme loaded from a disk works in every window
-- without any of them knowing the file existed.
--
function theme.current()
  local out = {}

  for _, k in ipairs(theme.tokens) do out[k] = theme[k] end

  return out
end

--
-- Install a palette under a name, which is what makes a loaded file appear
-- in `appearance` beside the ones that ship.
--
function theme.install(key, palette)
  theme.palettes[key] = palette

  return palette
end

--------------------------------------------------------------------------
-- The one that is in force.
--
-- Fields are copied into this table rather than the table being swapped,
-- for the reason at the top of the file.
--------------------------------------------------------------------------

function theme.apply(palette)
  if type(palette) == "string" then
    palette = theme.palettes[palette]
  end

  if type(palette) ~= "table" then
    return nil, "no such palette"
  end

  for k, v in pairs(palette) do
    theme[k] = v
  end

  return theme
end

-- Whatever a caller hands over on top of a palette: the desktop colour is
-- chosen separately from the palette it sits with, so a light theme with a
-- dark desktop is a thing somebody can have.
function theme.override(fields)
  if type(fields) ~= "table" then return theme end

  for k, v in pairs(fields) do
    if type(v) == "number" then theme[k] = v end
  end

  return theme
end

--------------------------------------------------------------------------
-- Chrome that is not flat.
--
-- The kit drew one-pixel bevels and no gradients, and said so in
-- `themes.lua`, on the grounds that a flat colour reading *as* a gradient
-- was honest and cheap. The bevels stay - they are the sentence about what
-- a control does - but the flat chrome underneath them was the one place
-- the look read as unfinished rather than as restrained.
--
-- **This is Lua painting a row at a time, and that is not the pixel loop
-- the rules forbid.** A row is a `fill`, which is a C span; a 20-row tab is
-- twenty crossings of the boundary and twenty tight loops on the far side
-- of it. What Lua does here is choose twenty colours, which is arithmetic
-- on twenty numbers. Moving that into C would buy twenty subtractions and
-- cost a primitive.
--------------------------------------------------------------------------

local function clamp(v)
  return (v < 0) and 0 or ((v > 255) and 255 or v)
end

--
-- The same colour, carried towards white or towards black.
--
-- Per channel and unweighted, which is wrong for a perceptual lightening
-- and exactly right here: over the ten or so counts a chrome gradient
-- moves, the hue does not visibly shift, and anything cleverer would need
-- a colour space this kit has no other use for.
--
function theme.lift(c, amount)
  return 0xff000000
         | (clamp(((c >> 16) & 0xff) + amount) << 16)
         | (clamp(((c >>  8) & 0xff) + amount) <<  8)
         |  clamp(( c        & 0xff) + amount)
end

--
-- The two ends of a piece of chrome, from the one colour a theme names.
--
-- Derived rather than named, and this is the opposite call from the one
-- `edge_light` and `edge_dark` got at the top of this file - so it is worth
-- saying why they differ. An edge is a single pixel doing a *semantic* job,
-- and on the dark palette a flatly-lightened highlight reads as fog: there
-- is no formula, so the palette names both. A gradient is twenty pixels
-- doing an *atmospheric* job, and over ten counts a flat lift is
-- indistinguishable from anything a formula could do better. Naming two
-- more colours per palette would be four more numbers in every theme, and
-- every one of them would be the base plus ten.
--
-- Light from above, which is where every bevel in this kit already puts it.
--
function theme.chrome(base)
  return theme.lift(base, 13), theme.lift(base, -9)
end

--
-- A vertical gradient, one span per row.
--
-- **The ramp is measured over `[y0, y0 + span)`, not over the rectangle
-- being painted**, and that is why this takes nine arguments rather than
-- seven. The window manager composes clipped to the damage rectangle, so a
-- title bar arrives here in slices - ten rows of it when ten rows were
-- disturbed. A ramp that ran over each slice would restart at every damage
-- boundary, and the bar would break into bands that moved as you dragged
-- it. Passing the band's own extent separately is what makes a partial
-- repaint indistinguishable from a whole one.
--
-- `dst` is anything with `fill(x, y, w, h, colour)`, which is both a
-- surface and a `gc`. The window manager composes onto one and every widget
-- draws through the other, and neither needs to know which this is.
--
function theme.vgradient(dst, x, y, w, h, top, bottom, y0, span)
  if w <= 0 or h <= 0 then return end

  y0   = y0 or y
  span = span or h

  -- The last row of the ramp, and never zero: a one-row band is entirely
  -- its own top, and dividing by `span - 1` would be a divide by zero.
  local last = (span > 1) and (span - 1) or 1

  local tr, tg, tb = (top >> 16) & 0xff, (top >> 8) & 0xff, top & 0xff
  local dr = ((bottom >> 16) & 0xff) - tr
  local dg = ((bottom >>  8) & 0xff) - tg
  local db = ( bottom        & 0xff) - tb

  for i = 0, h - 1 do
    -- Where this row sits in the ramp, clamped: a caller may paint rows
    -- outside the band, and they take the nearest end rather than a colour
    -- off the end of it.
    local t = y + i - y0

    if t < 0 then t = 0 elseif t > last then t = last end

    dst:fill(x, y + i, w, 1,
             0xff000000
             | ((tr + dr * t // last) << 16)
             | ((tg + dg * t // last) <<  8)
             |  (tb + db * t // last))
  end
end

-- The bitmap font until something says otherwise. It is exact, it costs
-- nothing, and it is what every display test was written against.
--
-- Four of them, because the four places text appears do not want the same
-- face: a title bar is a label on chrome and can carry a face with some
-- character in it, a widget font has to still work at every size in every
-- list in the system, a paragraph wants something to read, and a terminal
-- wants a fixed width or its columns stop lining up. One setting for all
-- four could only ever be wrong for three of them.
--
-- The title was part of `ui` until somebody chose a display face for their
-- title bars and got it in every list as well, which is the whole argument
-- for splitting it: the settings you *want* to make are the roles.
theme.fonts = {
  ui    = { font = "spleen", px = 16 },
  title = { font = "spleen", px = 16 },
  text  = { font = "spleen", px = 16 },
  mono  = { font = "spleen", px = 16 },
}

theme.apply("dark")

return theme
