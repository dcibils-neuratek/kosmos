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
-- differently, and the differences are the point. Photon and BeOS are light
-- greys with soft edges; Platinum is lighter still and almost colourless in
-- its chrome; IRIX is a *darker* grey with hard edges, which is why an SGI
-- looked like a workstation and a PC looked like a PC.
--
-- Every value here is a decision about a real system. Where one used a
-- gradient or a pinstripe - Photon's title bar, Platinum's - there is a
-- flat colour that reads as it, because this kit draws one-pixel bevels and
-- no gradients.

local themes = {}

-- The order they are offered in, oldest interface idea first.
themes.order = { "photon", "beos", "platinum", "irix" }

--------------------------------------------------------------------------

themes.photon = [[
# QNX Photon microGUI, as it looked on Neutrino 6.
#
# Warm light grey chrome, a steel-blue title bar, and a pale blue ground.
# The chrome is warmer than BeOS's neutral grey, which is the difference a
# person notices without being able to name it.

name       = photon

desktop    = #8fb4dc
window     = #d6d3ce
raised     = #dedbd6
sunken     = #ffffff

line       = #9a958e
line_soft  = #c5c1bb

edge_light = #ffffff
edge_dark  = #85817b

text       = #000000
text_dim   = #5a564f
text_on    = #ffffff

tab        = #4a7ab5
tab_idle   = #a8a49d
tab_text   = #ffffff

accent     = #316ac5
good       = #2e8b2e
bad        = #c02020
ring       = #4a7ab5
stamp      = #6f93bb
]]

--------------------------------------------------------------------------

themes.beos = [[
# BeOS R5.
#
# The yellow is the whole point. It is the single most identifiable colour
# of any desktop of its generation, and it is on the *focused* window only -
# which is what made a BeOS screen readable at a glance across a room.
#
# The panel grey is neutral, not warm: BeOS and Photon are both "grey" and
# do not look alike.

name       = beos

desktop    = #336698
window     = #d8d8d8
raised     = #e0e0e0
sunken     = #ffffff

line       = #9c9c9c
line_soft  = #c6c6c6

edge_light = #ffffff
edge_dark  = #858585

text       = #000000
text_dim   = #545454
text_on    = #ffffff

tab        = #ffc700
tab_idle   = #cccccc
tab_text   = #000000

accent     = #2e5fa3
good       = #2f8f3f
bad        = #c62828
ring       = #2e5fa3
stamp      = #6d8fb5
]]

--------------------------------------------------------------------------

themes.platinum = [[
# Mac OS 8 and 9, "Platinum". Not Aqua, and not anything after it.
#
# Restrained to the point of having almost no colour in its chrome at all:
# the title bar is grey, not blue, and the only saturated thing on screen is
# the selection. That restraint is the look, and giving the title bar a
# colour would break it faster than getting a grey slightly wrong.
#
# The active title bar had horizontal pinstripes. This kit cannot draw them,
# so it is a shade darker than the window instead - which is the reading the
# stripes gave.

name       = platinum

desktop    = #5f7f9f
window     = #dddddd
raised     = #eeeeee
sunken     = #ffffff

line       = #999999
line_soft  = #cccccc

edge_light = #ffffff
edge_dark  = #888888

text       = #000000
text_dim   = #666666
text_on    = #ffffff

tab        = #bbbbbb
tab_idle   = #dddddd
tab_text   = #000000

accent     = #4a7fd0
good       = #2f8f3f
bad        = #cc3333
ring       = #4a7fd0
stamp      = #8fa7bf
]]

--------------------------------------------------------------------------

themes.irix = [[
# SGI IRIX, Indigo Magic, under 4Dwm.
#
# The odd one out, deliberately. Everything else here is a light grey box;
# IRIX is a *mid* grey with hard edges, and its wells are grey rather than
# white. That is why an Indy looked like a workstation and a beige PC
# running the same widgets did not.
#
# The bevels are the strongest of the four. A soft bevel on this grey would
# read as a smudge, which is exactly what makes IRIX imitations look wrong.

name       = irix

desktop    = #1e3a52
window     = #a8a8a8
raised     = #b4b4b4
sunken     = #8e8e8e

line       = #666666
line_soft  = #909090

edge_light = #e4e4e4
edge_dark  = #4a4a4a

text       = #000000
text_dim   = #3c3c3c
text_on    = #ffffff

tab        = #5a6f96
tab_idle   = #8a8a8a
tab_text   = #ffffff

accent     = #4f6fa8
good       = #3f7f3f
bad        = #a83030
ring       = #4f6fa8
stamp      = #46617a
]]

return themes
