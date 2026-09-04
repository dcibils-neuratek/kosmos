-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The themes that ship, as text in the format themes are written in.
--
-- Not tables. Text, in exactly the format a `.theme` file on the disk uses,
-- parsed by exactly the parser that reads one - so the format is the thing
-- that ships rather than a thing bolted on beside it, and a bug in the
-- parser is a bug in the desktop rather than a bug in a feature nobody uses.
--
-- `ui.md` 16.8b decided the look is dimensional: raised, sunken, grooved.
-- These four are that decision applied to four systems that each solved it
-- differently, and the differences are the point - Photon and BeOS share a
-- panel grey and look nothing alike, Platinum paints its buttons the colour
-- of the panel, and IRIX paints them *darker* than it.
--
-- **The values are researched, not remembered.** BeOS's come from Haiku's
-- `_kDefaultColors[]`, which is R5's table rather than a recollection of
-- it; Photon's from QNX's own widget reference and lossless screenshots.
-- The first version of this file was written from memory and got the BeOS
-- yellow wrong by four counts, painted Photon's neutral grey warm, and gave
-- both of them blue selections when neither has one. Each note below says
-- what the real thing does.
--
-- Where a system used a pinstripe there is a flat colour that reads as it.
-- Gradients are no longer in that list: the chrome - window tabs and menu
-- bars - is shaded from the one colour named here, by `theme.chrome`, so a
-- palette still names one surface and gets both ends of it.

local themes = {}

-- The order they are offered in, oldest interface idea first.
themes.order = { "photon", "beos", "platinum", "irix" }

--------------------------------------------------------------------------

themes.photon = [[
# QNX Photon microGUI, Neutrino 6.x.
#
# The grey is *neutral*, not warm - the same #d8d8d8 BeOS uses. The
# blue-grey impression everyone remembers comes entirely from the title bars
# and the pale blue ground, not from a tinted chrome. Guessing a warm grey
# here was the first thing this got wrong.
#
# Two details that are the whole tell:
#
#   tab_text is dark navy, never white. White title text instantly reads as
#   Windows, and it is the most misremembered thing about Photon.
#
#   the selection is a desaturated sage grey-green. Not a blue. Photon has
#   almost no saturated colour in its chrome at all.
#
# `line` is a hard dark ring drawn all the way around every control, which
# is what makes Photon look chiselled rather than boxed.

name       = photon
desktop    = #8dc0df
window     = #d8d8d8
raised     = #dcdcdc
sunken     = #f4f4f4

line       = #4b4b4b
line_soft  = #a1a1a1

edge_light = #ffffff
edge_dark  = #b0b0b0

text       = #000000
text_dim   = #7f7f7f
text_on    = #ffffff

tab        = #5786da
tab_idle   = #abbbd2
tab_text   = #000065
desktop_text = #ffffff
console      = #0b0b0b
console_text = #d8d8d8

accent     = #8ea29b
good       = #8d9c88
bad        = #cc1a15
ring       = #464646
stamp      = #7ab3d4
]]

--------------------------------------------------------------------------

themes.beos = [[
# BeOS R5, from Haiku's `_kDefaultColors[]` - which is not a recollection
# of R5's colours, it is R5's colours.
#
# The yellow is #ffcb00, which is 255,203,0: a faintly green-shifted
# saturated yellow, and an exact entry in BeOS's 8-bit palette whose cube
# steps 0/51/102/152/203/254. That is why it is 203 and not a round number,
# and why #ffc700 - which is what this shipped first - is wrong.
#
# Two things that are easy to get backwards:
#
#   the button face is LIGHTER than the panel it sits on. Windows 95 made it
#   the same grey, and copying that is the fastest way to stop looking like
#   BeOS.
#
#   nothing in the interface is blue except the desktop and the focus ring.
#   Selections are GREY with the text still black - a blue selection reads as
#   Windows, or as modern Haiku, but not as R5. The ring is #0000e5, an
#   almost violent blue, and it is the only place that colour appears.

name       = beos
desktop    = #336698
window     = #d8d8d8
raised     = #e8e8e8
sunken     = #ffffff

line       = #606060
line_soft  = #b8b8b8

edge_light = #ffffff
edge_dark  = #989898

text       = #000000
text_dim   = #808080
text_on    = #000000

tab        = #ffcb00
tab_idle   = #e8e8e8
tab_text   = #000000
desktop_text = #ffffff
console      = #0b0b0b
console_text = #d8d8d8

accent     = #bebebe
good       = #009800
bad        = #cb0000
ring       = #0000e5
stamp      = #5d85ad
]]

--------------------------------------------------------------------------

themes.platinum = [[
# Mac OS 8 and 9, "Platinum". Not Aqua, and not anything after it.
#
# Restrained to the point of having almost no colour in its chrome: the
# title bar is grey rather than blue, and even the selection is a pale
# lavender rather than anything saturated. That restraint *is* the look, and
# giving the title bar a colour would break it faster than getting a grey
# slightly wrong.
#
# `raised` and `window` are the same grey on purpose. Platinum buttons are
# the colour of the panel they sit on and are told apart by their bevel
# alone, which is the opposite of BeOS and worth seeing side by side.
#
# The active title bar had horizontal pinstripes; this kit cannot draw them,
# so it is a shade darker instead, which is the reading the stripes gave.

name       = platinum
desktop    = #63639c
window     = #dddddd
raised     = #dddddd
sunken     = #ffffff

line       = #000000
line_soft  = #888888

edge_light = #ffffff
edge_dark  = #999999

text       = #000000
text_dim   = #777777
text_on    = #000000

tab        = #cccccc
tab_idle   = #dddddd
tab_text   = #000000
desktop_text = #ffffff
console      = #0b0b0b
console_text = #d8d8d8

accent     = #ccccff
good       = #008800
bad        = #dd0000
ring       = #6666cc
stamp      = #7777af
]]

--------------------------------------------------------------------------

themes.irix = [[
# SGI IRIX, Indigo Magic, under 4Dwm.
#
# The odd one out, and every value says so. `raised` is DARKER than
# `window` - a control here is a recess in the panel rather than a lump on
# it, which no other system in this file does. The wells are grey rather
# than white. The bevels are the hardest of the four, because a soft bevel
# on this grey reads as a smudge, and that is exactly what makes IRIX
# imitations look wrong.
#
# The active title bar is khaki, not blue. Nobody guesses that, and it is
# the thing that makes a screenshot recognisable as an Indy from across the
# room.

name       = irix
desktop    = #4c719e
window     = #c1c1c1
raised     = #999999
sunken     = #8e8e8e

line       = #000000
line_soft  = #acacac

edge_light = #cccccc
edge_dark  = #4c4c4c

text       = #000000
text_dim   = #808080
text_on    = #000000

tab        = #a59f80
tab_idle   = #808080
tab_text   = #000000
desktop_text = #ffffff
console      = #0b0b0b
console_text = #d8d8d8

accent     = #d5d5d5
good       = #23d223
bad        = #ff0000
ring       = #000000
stamp      = #5680ab
]]

return themes
