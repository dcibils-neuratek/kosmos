-- The palette, in one file.
--
-- `ui.md` 16.8b: BeOS's structure, not BeOS's skin. The tab that is only as
-- wide as its title is a decision about behaviour and is copied exactly.
-- The 1998 grey bevels and the hard `#FFCC00` are decisions about shading
-- and are made fresh here.
--
-- What that means concretely: dark ground, flat surfaces, one-pixel
-- separators instead of two-pixel chamfers, and weight and spacing doing
-- the work a bevel used to do. A bevel says "this is a raised physical
-- control", which stopped being a useful lie once everybody knew what a
-- button was.
--
-- One table so that changing the look is one file and not a search for
-- hex literals.

return {
  -- Ground and surfaces.
  desktop   = 0xff1c2530,
  window    = 0xff161b22,
  raised    = 0xff21262d,
  sunken    = 0xff0d1117,

  -- A single hairline, everywhere something needs an edge.
  line      = 0xff30363d,
  line_soft = 0xff21262d,

  -- Text.
  text      = 0xffc9d1d9,
  text_dim  = 0xff8b949e,
  text_on   = 0xff0d1117,      -- on an accent

  -- The one inherited colour, warmed and darkened out of 1998.
  tab       = 0xffffc700,
  tab_idle  = 0xff484f58,

  accent    = 0xff1f6feb,
  good      = 0xff3fb950,
  bad       = 0xffda3633,

  -- Focus is shown by a ring, never by a colour change: a control that
  -- changes colour when focused reads as a different control.
  ring      = 0xff58a6ff,
}
