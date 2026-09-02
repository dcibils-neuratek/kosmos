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

  accent    = 0xff2d5faf,
  good      = 0xff1a7f37,
  bad       = 0xffbb2222,
  ring      = 0xff2d5faf,

  stamp     = 0xff97b4d1,
}

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

-- The bitmap font until something says otherwise. It is exact, it costs
-- nothing, and it is what every display test was written against.
--
-- Three of them, because the three places text appears do not want the same
-- face: the titlebar and the widgets want whatever the desk looks like, a
-- paragraph wants something to read, and a terminal wants a fixed width or
-- its columns stop lining up. One setting for all three could only ever be
-- wrong for two of them.
theme.fonts = {
  ui   = { font = "spleen", px = 16 },
  text = { font = "spleen", px = 16 },
  mono = { font = "spleen", px = 16 },
}

theme.apply("dark")

return theme
