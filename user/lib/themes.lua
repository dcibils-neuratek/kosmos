-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The looks that ship, as text in the format themes are written in.
--
-- Not tables. Text, in exactly the format a `.theme` file on the disk uses,
-- parsed by exactly the parser that reads one - so the format is the thing
-- that ships rather than a thing bolted on beside it, and a bug in the
-- parser is a bug in the desktop rather than a bug in a feature nobody uses.
--
-- **Four looks, and nothing else to choose** (`roadmap.md` 5y). Diego, 22
-- September 2026: "Too many config options make the system vulnerable to
-- changes and complicated", and "Let's just make 3 or 4 good design options
-- in colors and fonts and stick to those". Each is a whole - its colours,
-- its faces, its Deskbar - designed together in `docs/looks.html` and
-- approved there: "Those 4 looks are great". Somebody picks a look, not its
-- parts. They share their faces, IBM Plex at the sizes the fixed layout
-- holds, and differ in colour.
--
-- The four this file held before - Photon, BeOS, Platinum, IRIX, each a
-- system that solved `ui.md` 16.8b's dimensional look its own way - gave
-- way to them the same day. BeOS lives on as Classic, its researched
-- values and notes kept; the others are in the history.
--
-- Where a system used a pinstripe there is a flat colour that reads as it.
-- The chrome - window tabs and menu bars - is shaded from the one colour
-- named here, by `theme.chrome`, so a palette names one surface and gets
-- both ends of it.

local themes = {}

-- The order they are offered in, the default first.
themes.order = { "plex", "plexnight", "classic", "studio", "endeavour" }

-- What each is called where a person reads it.
themes.titles = {
  plex = "Plex", plexnight = "Plex Night", classic = "Classic",
  studio = "Studio", endeavour = "Endeavour",
}

--------------------------------------------------------------------------

themes.plex = [[
# Plex: Kosmos's own, and the only one here that is not somebody else's.
#
# Diego, on 21 September 2026, looking at the mockups this project draws
# its applications in before writing them: "i love the font used in the
# mockups", and then "i want the theme to look exactly as the mockup, same
# fonts same sizes same spacing, same colors". Every value below is one of
# those pages' - `docs/indicators.html` for the desktop, the bar and the
# panels, `docs/video.html` for the window's tab and the dark surfaces -
# and `docs/plex.html` says which, and marks the few no page draws.
#
# Cooler than BeOS's greys and warmer than white: the panels are a faint
# stone, the rules between things a little darker, and the one
# saturated colour is a deep IBM-ish blue for the selection and the ring.
# The yellow tab is BeOS's idea kept, in a softer yellow - on the windows,
# on the Deskbar and on a scrollbar's thumb, which is where a look's tab
# colour goes in every look (`theme.lua`). The mockups drew the Deskbar in
# stone, and it was until Diego, 22 September: "the deskbar tab color should
# be yellow or at least the same color of the acccent color of the theme".

name       = plex
desktop    = #3d63b8
window     = #f4f4f1
raised     = #e7e7e3
sunken     = #ffffff

line       = #777777
line_soft  = #cfcfc9

edge_light = #ffffff
edge_dark  = #9a9a94

text       = #1e1e1e
text_dim   = #6c6c66
text_on    = #ffffff

tab        = #f2c230
tab_idle   = #e7e7e3
tab_text   = #3a2e00
desktop_text = #ffffff
console      = #1c1c1e
console_text = #ececec

accent     = #2a55c9
good       = #2f8a3e
bad        = #b3261e
ring       = #2a55c9
stamp      = #8fa9df

# The mockups' type, as Diego chose it on 22 September: widgets and the
# running text of the pages in Plex Sans at 16 - widgets were 14, as the
# style guide has them, until he saw them on the ThinkPad the same
# morning: "fonts look smaller than on qemu" - the panels' titles in
# SemiBold, the terminal in Plex Mono at 14 - `docs/looks.html`, where all
# four looks share these faces.
font.title   = ibmplexsanscondensed 14
font.ui      = ibmplexsans 16
font.heading = ibmplexsans-semibold 15
font.text    = ibmplexsans 16
font.mono    = ibmplexmono 16
]]

--------------------------------------------------------------------------

themes.plexnight = [[
# **Plex Night**: Plex in the dark, for the evening and for film. Slate
# windows on a midnight desk; the blue lifted so it still reads on a dark
# ground; BeOS's yellow tab dimmed to amber so it is still the tab and not
# the brightest thing on the screen (`docs/looks.html`). An unfocused tab is
# a light grey, not the slate of the windows: the tab's words are dark on
# both, and the first build drew them dark on slate, where they vanished.

name       = plexnight
desktop    = #16213a
window     = #1f2227
raised     = #2c3038
sunken     = #15171b

line       = #0a0b0d
line_soft  = #3a3f47

edge_light = #3a3f48
edge_dark  = #0d0e11

text       = #e6e8eb
text_dim   = #9aa1ab
text_on    = #ffffff

tab        = #d9a92a
tab_idle   = #9aa0a8
tab_text   = #231a00
desktop_text = #e6e8eb
console      = #0d0f12
console_text = #d6dae0

accent     = #4f7dff
good       = #3fb950
bad        = #ff6b5e
ring       = #7aa0ff
stamp      = #3a4a6e

font.title   = ibmplexsanscondensed 14
font.ui      = ibmplexsans 16
font.heading = ibmplexsans-semibold 15
font.text    = ibmplexsans 16
font.mono    = ibmplexmono 16
]]

--------------------------------------------------------------------------

themes.classic = [[
# **Classic**: BeOS R5 as it was, in Plex type. The colours are from
# Haiku's `_kDefaultColors[]` - which is not a recollection of R5's colours,
# it is R5's colours. R5 drew its Deskbar grey; here it is the tab's yellow,
# as the Deskbar is in every look.
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

name       = classic
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
# The faces all four looks share (`docs/looks.html`).
font.title   = ibmplexsanscondensed 14
font.ui      = ibmplexsans 16
font.heading = ibmplexsans-semibold 15
font.text    = ibmplexsans 16
font.mono    = ibmplexmono 16
]]

--------------------------------------------------------------------------

themes.studio = [[
# **Studio**: for the media applications - Video, Music, the solar system.
# Near-black and low-glare, so a picture is the brightest thing on the
# screen, with one warm amber for what is selected and in focus, and the
# tab in the same amber so the focused window is found at a glance
# (`docs/looks.html`).

name       = studio
desktop    = #0e0f12
window     = #18191d
raised     = #24262c
sunken     = #0f1013

line       = #000000
line_soft  = #2c2f36

edge_light = #30333a
edge_dark  = #08090a

text       = #e8e6e3
text_dim   = #8d8a86
text_on    = #1a0f05

tab        = #e8833a
tab_idle   = #8a8c90
tab_text   = #1a0f05
desktop_text = #d9d6d2
console      = #08090a
console_text = #d8d4ce

accent     = #e8833a
good       = #3fb950
bad        = #ff6b5e
ring       = #f0a060
stamp      = #2a2c31

font.title   = ibmplexsanscondensed 14
font.ui      = ibmplexsans 16
font.heading = ibmplexsans-semibold 15
font.text    = ibmplexsans 16
font.mono    = ibmplexmono 16
]]

themes.endeavour = [[
# Endeavour: a white view on a very light ground, and one blue.
#
# Diego, 23 September 2026, with two screenshots of a GNOME desktop beside
# him: "can we make a new theme called endeavor and has the same colors and
# asthetics as the screenshots i uploaded from linux". Named as he named
# it, spelled the way this project spells everything else.
#
# **What it takes from them is the palette and the flatness, and nothing
# else** (`roadmap.md` 5zk, 5zi). A quieter surface is a shading decision
# and `beos.md` has always said those are ours to make fresh; replicants,
# the Deskbar and the right button reaching the application are how this
# desktop *works*, and a screenshot is not asking about them.
#
# The difference from every look before it is where the light is. Plex,
# Classic and Studio are dimensional - a raised surface is lighter than its
# window and a sunken one darker, which is how a 1995 desktop said "this is
# a control". Here the view is the *lightest* thing and the window around it
# a shade down, which is how a 2015 one says "this is the content". So
# `raised` and `sunken` are close together and the separation is carried by
# `line_soft` instead: a hairline rather than a bevel.
#
# **The tab is the thing this look forces a decision about**, and it is the
# one place the screenshots could not simply be copied.
#
# One token has carried the focused title bar, the Deskbar and a scrollbar's
# thumb since 0.10.112, because Diego asked for one accent rather than two
# that drift. The screenshots' title bar is nearly white, and three things
# go wrong if this one is: a nearly-white Deskbar is hard to find, a nearly
# white thumb on a white track is invisible, and `tab_text` has to read on
# the *unfocused* tab as well, which is a light grey.
#
# So the tab is the accent at low saturation - a pale blue that is plainly
# visible on the blue desktop, plainly visible as a thumb on a white track,
# and dark enough for near-black words. It is the same idea the screenshots
# have, one colour saying what has your attention, in the one token this
# system already has. Splitting that token is a decision of its own and not
# one a new look should take on its way past (`roadmap.md` 5zk).

name       = endeavour

# **Flat, which is the whole of what makes this look itself.**
#
# Diego, 23 September 2026: "the endeavor theme uses flat shading and our
# theme uses bevels in the deskbar and else, lets use flat shading like the
# mockups". Every place this system draws two edges to say "this sticks
# out" draws one hairline instead - the kit's raised and sunken, the window
# manager's tab controls, and the title bar's gradient, which becomes a
# fill.
#
# The four looks before it are dimensional on purpose and stay so
# (`ui.md` 16.8b): a bevel is how a 1995 desktop said "this is a control",
# and copying that decision is what Classic is for. This one says it with
# a line, and the palette is built for that - `line` and `line_soft` are a
# shade apart rather than the deep grey a moulded edge needs.
flat       = yes

desktop    = #2e5cb8
window     = #f6f5f4
raised     = #ffffff
sunken     = #ffffff

# A hairline, not a bevel: `line` is what a list's frame and a pane's edge
# are drawn with, and `line_soft` the rules between rows.
line       = #cdc7c2
line_soft  = #e3e0dd

# Both near the surface, so nothing looks moulded. They cannot be equal to
# it - `theme.chrome` shades from them - but they are within a step.
edge_light = #ffffff
edge_dark  = #d8d4d0

text       = #241f31
text_dim   = #77767b
text_on    = #ffffff

tab        = #a7c7ee
tab_idle   = #ddd9d5
tab_text   = #1b3557
desktop_text = #ffffff

console      = #241f31
console_text = #f6f5f4

accent     = #3584e4
good       = #2ec27e
bad        = #e01b24
ring       = #3584e4
stamp      = #99c1f1

# The same faces as the other four, at the same sizes: a look is colour
# here, and the fixed layout (`roadmap.md` 5x) holds because of it.
font.title   = ibmplexsanscondensed 14
font.ui      = ibmplexsans 16
font.heading = ibmplexsans-semibold 15
font.text    = ibmplexsans 16
font.mono    = ibmplexmono 16
]]

return themes
