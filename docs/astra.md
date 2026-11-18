<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Astra, looked at whole: what is uneven, what would make it nicer, what it lacks

Written 17 November 2026, for Diego to decide. Nothing here is built.

Diego, the same day: "analuze the current astra desktop UI and see what
improvements we can do, what inconsistencies there are, what things can we
make it nicer like transparencies, glass effects, modern featues that most
os should have".

Read from the desktop as it is: the dated gallery shots
(`docs/screenshots/2026-11-17-*.png`), the M700 at 0.11.435
(`build/m700-verge.png`), the dock and the Shortcuts window in Night at
0.11.463 (`build/x86_64/dock-shortcuts.png`), the launcher pad
(`build/dock/launcher.png`) and Mail (`build/mail/mail.png`); and from the
code that draws them - `user/lib/theme.lua`, `user/themes/*.theme`,
`user/lib/ui.lua`, `user/lib/pixelkit.lua`, `user/bin/apps/deskbar.lua`,
`user/lib/dock.lua`, `user/bin/apps/launchpad.lua`,
`user/bin/apps/notifications.lua`, `user/lib/settings.lua`,
`user/bin/programs/wm.lua`, `user/lib/wm/`, `user/kits/gfx/scene_core.c` and
`shadow.c`. It builds on `docs/review.md` (section 7, *Consistency*, is about
applications' commands, find, tabs, undo; this is about how Astra *looks*,
and does not repeat it) and on what is already agreed in `docs/roadmap.md`,
`docs/dock.html`, `docs/switcher.html`, `docs/notifications.html`,
`docs/astra-display.md` and `docs/m700-2d.md`.

The mockup is `docs/astra.html`; where each effect lives is
`docs/astra-architecture.html`. Liquid Glass, a look of its own, is drawn

**AGREED on 17 November 2026** - Diego, of the drawing: "Love the Liquid Glass!". The decisions below are taken as recommended; it is built after Starlight's first steps, LG1 to LG7.
in `docs/liquidglass.html`.

---

## Identity

Added the same day, after Diego had seen the first mockup: "i like the
nicer astra, but it looks like macos exactly the same. no real
personality. we should have this nicer ideas but we need to have some
identity like beos had or sgi irix had. so we should create some specific
visual cue that we are running kosmos and not a clone of macos".

**He was right, and the review shows why.** Each of its effects was the
modern default taken one at a time - neutral frosted glass, a pill dock
centred at the bottom, three coloured lights, pill tiles in quick
settings, a scale-and-fade on opening - and together they are macOS.
Astra already wears two of its cues today: the lights are macOS's
colours (`wm.lua` `OUT.LIGHTS`, Diego's choice on 24 September from a
picture of them) and the dock is a rounded bar of 18.

BeOS was known in one glance by a yellow tab off to one side and the
Deskbar in a corner; IRIX by Indigo Magic's blue-grey, 4Dwm's
decorations and icons drawn as vectors. Neither was a set of effects.
Each was **a few shapes nobody else used, repeated everywhere**. So an
identity here is a short list of signature cues, each drawn the same way
on every surface - and each cheap, because Astra composes on the
processor (`scene_core.c`, 3.4 ns a composed pixel on the M700,
`testing.md` 18.657).

`docs/astra.html` switches between the three directions below and the
earlier proposal, so the difference can be seen rather than described.

### The three directions

**Starlight** (recommended) - Astra means stars, Nebula the cloud they are
born in, Kosmos the order. And `ui.md` 16.8b already holds the sentence
it is built on: "the focused one is the yellow one".

| Cue | What | Cost in the scene |
|---|---|---|
| **The plate** | The title of the window in front sits on an amber plate at the header's left, full header height, its right edge cut at 45 degrees; on the windows behind, the plate is the header's own tone. The window's top-left corner is square where the plate meets it; the other three keep the window radius. It lives *inside* the full-width header Diego chose (16.25) - not the BeOS tab back, which he turned down twice (16.8b), but its sentence. | A trapezoid fill, ~9 kpx at 1920x1080, when the window in front changes: two plates, ~0.03 ms (**estimate**). Nothing a frame. |
| **Colour** | Flare amber in `tab` - the token that is already the focused title's and the Deskbar's (22 September) - on night indigo, with an astral blue for selection. A light look keeps the amber and inks in indigo. | Tokens: nothing. |
| **Material: nebula glass** | Wallpaper glass (4.3 G1) with a twist: the tint is a vertical gradient rather than a flat grey, a faint star-dust grain sits in it, and a one-pixel amber *horizon* runs along the top of every glass surface. | Gradient and grain are baked into the blurred wallpaper copy once, when the wallpaper changes (+1-2 ms on the 8-10 ms blur, **estimate**): 0 a frame. The horizon is one row of pixels. Under G2 the grain is one add a glass pixel. |
| **The cut** | Every floating surface - the dock, menus, banners, quick settings, tiles, the lit row - has two opposite corners cut at 45 degrees (top-left and bottom-right). Never a pill, never a squircle. | Cheaper than a rounded corner: a start offset a row and one pixel of coverage on the diagonal, where `gfx_round_cover` looks up a table (`SC_CUT`, beside `SC_ROUNDED`). |
| **Deskbar and dock** | The strip opens on a Kosmos plate - the same amber trapezoid, with a four-point star as the Kosmos mark. The dock is a cut bridge; a running application is marked by a *glint*, a four-point star, amber for the one in front, where macOS has a dot. | A 9 px sprite a cell, on a change of running or front. |
| **The three** | Diamonds - points of a star - in the look's ink, in today's slots; close filled with the flare on the window in front, glyphs under the pointer as today. Replaces the three coloured lights. | Four triangles each; `OUT.light` already draws with `back:triangle`. |
| **Icons** | Haiku's manner, which Astra already has (`tools/appicons.py`), made the rule: an application icon is a free-standing object with a heavy outline and light from the upper left, never set on a tile. Line icons for controls as now. | Nothing. |
| **Motion: unfurl** | A window opens as its plate first, and the body unrolls downward beneath it in 150 ms; a close rolls back up into the plate in 120 ms (Mac OS 9's window shade, as a gesture). Minimise keeps Diego's outline to the dock (3 October), ending in one twinkle of the glint. Focus: the plate's amber sweeps in over 90 ms. | A clip, not a scale: only the rows revealed are composed and nothing is blended - less than the 2.8 ms a frame 4.4's scale-and-fade cost (**estimate**). |

**Observatory** - Indigo Magic's manner, and `ui.md` 16.8b's dimensional
principle kept literally. Slate blue-grey everywhere and one deep teal;
a bevelled title bar, slate behind and teal in front, with a window-menu
square at its left and square bevelled controls; no glass at all, a hard
offset shadow; a bevelled console strip with a Toolchest button and a dock
of raised keys; line icons in one colour; motion as zoom rectangles, six
outlines from the dock to the window. **Cost**: the lowest of the three -
a gradient a row, two 1 px edges an edge, four thin rectangles a frame of
motion, and no blur ever. **Against it**: it is Classic's road, which Diego
left for Endeavour and Plex; it reads 1996 before it reads Kosmos.

**Aurora** - every application its own colour, as a planetarium labels a
sky. Each application declares a hue (`-- kosmos: hue`, a new line in its
opening comment) and wears it as a 3 px band across its window's top; the
one in front is 4 px and glows; the strip's lower edge takes the front
application's hue; running marks in the dock are hue bars; line icons in
the hue; on focus or opening the band sweeps across. **Cost**: a 3 px
line; the glow a 16 px falloff from the table `shadow.c` keeps; the hue a
tint G1 already applies. **Against it**: forty hues to choose and keep
apart, and a thin band is a quiet mark - at a glance it can still read as
a dark macOS.

### Recommendation: Starlight

- **It is recognisable in one glance and in a thumbnail**: an amber
  trapezoid at one corner of one window, the cut on everything that
  floats, the star. No other desktop draws any of them.
- **It is BeOS's lineage, carried forward rather than revived.** The
  focused thing is the yellow thing, as 16.8b says - in the header Diego
  chose, not in the tab he turned down.
- **It keeps everything the review proposed** and changes only how it is
  shaded, and it is cheaper than what it replaces: the cut is cheaper than
  rounding, the unfurl cheaper than the scale-and-fade, and the grain and
  gradient are paid once a wallpaper.
- **It reuses tokens that exist**: `tab` is already the one accent for the
  focused title and the Deskbar; the plate is that token given a shape.

### What is kept from the review, and what changes

**Kept, unchanged**: the token sheet (4.1) and every inconsistency fix in
section 3; shadows on in two layers (4.2); glass in three steps, G1 as the
default (4.3); hover, press and the focus ring (4.5); the person's own
accent, never touching `tab` (4.6); every feature in section 5; the order
in section 6.

**Changed by Starlight**:

- **4.1**: the radius scale gains a `cut` (control 6, surface 14, the
  plate's slant 22 at 46 high) beside `radius`; floating surfaces use the
  cut, windows keep `radius` on three corners.
- **4.3**: the glass tint is a gradient with grain and a horizon, per look,
  rather than the window colour at a strength. Over G1 it costs nothing
  more a frame.
- **4.4**: opening and closing are the unfurl and the roll, a clip rather
  than a scale and a fade; durations unchanged; *Reduce motion* makes them
  cuts as before.
- **4.7**: free-standing icons are the rule, in the kit and in the dock.
- **3.12 and decision 10**: the window buttons change shape, from three
  coloured discs to diamonds, in the same slots - which reverses a choice
  Diego made on 24 September, so it is his to make again (I3 below).
- **Quick settings** (5, item 1, and `dock.html`): its tiles and sliders
  wear the cut rather than pills; the tiles that are on take the flare.
- **Who draws the plate**: the window manager, as it draws the three
  (16.25). The header leaves room at its left as it leaves room at its
  right, and says where its title is; the window manager draws the plate
  and the title on it - as it drew the tab and its title until 8 October -
  so which window is in front shows the moment it changes, from what the
  window manager knows, and no application can draw it wrong. The kit's
  `ui.header` and Write's `pk.header` stop drawing their own title.

### Identity decisions for Diego

**DECIDED on 17 November 2026.** Diego: "yes to starlight but dont replace
the current astra one, just add starlight to the desktop styles in
preferences", and "i dont like the 3 buttons for close, minimize and
maximise, those should be rounded buttons", "like today". So:

- **Starlight is a look of its own**, added to Appearance's looks beside
  the eleven, and not the default: the current Astra stays as it is,
  and nothing in the other looks changes. I7 is answered that way - the
  night-indigo look *is* Starlight, chosen in Preferences - and I2, I4,
  I5, I6 and I8 hold *inside Starlight only*: the plate, the cut corners,
  the nebula glass, the unfurl and the star are what choosing it brings.
- **I3 is no**: the three window buttons stay round, as today, in every
  look, Starlight included - drawn in Starlight's colours.
- **Softened, the same day**: "we should tone down the geometric look" -
  "a little bit" - and "have a good mix between geometric look and some
  more pleasant rounded or soften style". So Starlight keeps a few
  geometric signatures and rounds the rest:
  - **Stay geometric**: the plate's slanted right edge, with its two ends
    eased (5 px) and its top-left following the window's radius; the
    Kosmos plate on the strip, in the same shape; the four-point star,
    less spiky - its sides curved, its points rounded; the glint under a
    running application.
  - **Became soft**: windows rounded at all four corners, as today (I4's
    square top-left is no); the three buttons round; rows, tool buttons,
    the strip's entries, the Kosmos button, quick settings' tiles and
    sliders and banners' actions rounded at the control radius; banners
    and quick settings rounded whole.
  - **The cut, used sparingly**: one small cut at the top-left of the dock
    (12 px) and of a menu (8 px), its two edges eased by a 3 px radius,
    the other corners rounded; nowhere else. The amber horizon along the
    top of glass stays, as the accent line.

**How it is built: Starlight is a desktop, as Pewter is.** "The desktop
styles in preferences" are Appearance's *Desktop* row - Astra, Pewter -
and a desktop is already a file that says how windows are dressed
(`chrome`). So Starlight is `user/desktops/Starlight.desktop`: Astra's
programs, `chrome = starlight` and `look = starlight` - the look it wears
(`user/themes/Starlight.theme`), which a desktop can now name. Astra is
untouched and stays the default; Starlight's look is also offered among
the looks for anyone who wants its colours on Astra.

**S1, the plate, by the window manager** (I2): a header under Starlight's
chrome leaves the room at its left for the plate and tells the window
manager how wide (`window:place_plate`, beside `place_lights`); the
window manager draws the plate - the amber trapezoid and the title on it
for the window in front, the header's tone behind - as a small picture of
its own, and the compositor lays it over the window's header
(`scene:plate`, one more picture a window, blended for its slanted
edge). A focus change repaints two plates and nothing else. **Built in
0.11.468** (`testing.md` 18.868), with one addition the first picture
asked for: a look may name the Deskbar's colours apart from its tab
(`strip`, `strip_text`), so Starlight's bar is night and only its Kosmos
button stands on an amber plate. Two things left for later: Preferences'
header is split, so its plate says the page's name over the right pane
rather than the window's at its corner; and Appearance's Theme row still
marks the look saved there, not the one a desktop put on. S2 the
softened cut on the dock and menus; S3 the nebula glass's grain and its
amber horizon; S4 the unfurl; S5 the star.

What follows is the list as it was put to him.

1. **I1 - Which direction.** *Recommended: Starlight*, with Observatory's
   bevels kept as what Classic and Studio already are, and Aurora's
   per-application hue left aside.
2. **I2 - The plate, drawn by the window manager.** *Recommended: yes* -
   the header leaves room for it as for the three, and the title moves into
   it.
3. **I3 - Diamonds instead of the three coloured lights** (Diego's 24
   September choice). *Recommended: yes* - the lights are the single most
   macOS thing on the screen; the slots, the glyphs under the pointer and
   close outermost all stay.
4. **I4 - The cut on floating surfaces, and the window's top-left corner
   square where the plate meets it.** *Recommended: yes*, both.
5. **I5 - Nebula glass: gradient, grain and the amber horizon** over
   wallpaper glass. *Recommended: yes*, baked once a wallpaper.
6. **I6 - The unfurl and the roll instead of the scale-and-fade.**
   *Recommended: yes*: cheaper, and nobody else's.
7. **I7 - A default look and wallpaper to match**: a night-indigo look
   with the flare (and a light partner, sharing the amber), and a nebula
   wallpaper drawn for Kosmos. *Recommended: yes*; the existing eleven
   looks keep their own `tab` and gain the plate, the cut and the diamonds
   in their own colours.
8. **I8 - The Kosmos mark is a four-point star**, on the strip's plate,
   the dock's button and the glint. *Recommended: yes*; the Kosmos
   button's picture today is Haiku's.

---

## Liquid Glass, a look of its own

Written 17 November 2026, for Diego to decide. Nothing here is built.

Diego, the same day: "Can we try another desktop style called glass
inspired by this repo https://github.com/zakisheriff/Liquid-Glass", "It's
the current astra but full glass", then "Let's call it Liquid Glass", and
"It should have identity cues but feel like Liquid Glass".

**Drawn in `docs/liquidglass.html`**, over the wallpapers Kosmos ships,
light and dark, frosted and clear, with the refraction map computed in the
page the way it would be computed here. The glass rim is real only in a
Chromium browser; in others the page says so and falls back to a plain
blur.

**The inspiration**: *Liquid Glass* by Zaki Sheriff, a React library of
iOS 26-style controls, MIT. What it shows is that glass is not a blur:
**the glass bends light**. Each surface ray-traces a refraction map
through a curved bevel at its rim by Snell's law, computes it once per
size, caches it, and hands it to the browser as a displacement map
(`backdrop-filter: url(#filter)` in Chrome). Its knobs are a variant
(*regular*, frosted, or *clear*, see-through), a tint, an intensity of
refraction, a bevel thickness of 1 to 4, and a colour split at the rim.
Nothing of it is vendored: the method is what is taken, re-derived in C
below. The material's name is Apple's, from iOS 26; the repo says it is not
affiliated with Apple.

### What Liquid Glass is

**The current Astra, made of glass.** The default look's layout, unchanged:
the strip along the top with the Kosmos button, the running windows, the
tray and the clock; the floating dock; windows with the header as the
title bar and the three round buttons at its right; menus, banners and
quick settings. What changes is the material, everywhere:

- **Every surface that is chrome is glass** - a window's header, sidebar,
  toolbar, status line and the ground between its rows; the strip; the
  dock; menus; banners and the history; quick settings; the switcher and
  the launcher pad.
- **What a person reads or watches is not.** Write's page, JetSki's page,
  a film, Cafesa3D's viewport, a game, Maps' map: opaque, always - iOS 26
  draws the same line (glass is the navigation layer, never the content).
  A row of words sits on glass, which is why the glass behind words is
  frosted and tinted enough to read.
- **The rim is where it is liquid**: a bevel a few pixels wide around every
  glass surface bends what is behind it inward, so the backdrop seems to
  pour over the edge; a specular line lit from the upper left; and the
  colour split a little at the very edge.
- **Two materials**: *frosted* (blurred and tinted, for anything with words
  on it) and *clear* (unblurred, barely tinted, only the rim bends - the
  dock and the strip over a quiet wallpaper, as iOS 26's clear look).
- **Light and dark**: the light one over Endeavour's colours (the default
  look today), the dark one over Night's; each paired with a wallpaper
  from `assets/wallpapers/` - *Glass wave* and *Dusk ribbon* in the mockup.

### Kosmos's cues, and the glass's

The glass itself - refraction at a bevel, frost, a tint, a specular rim,
the colour split - is the material iOS 26 and the repo share, and it is
what makes the look *feel* like Liquid Glass. On its own it would read as
Apple's. So it wears Kosmos's cues, from Starlight's agreed list, and three
that only glass can have:

| Cue | Whose | In Liquid Glass |
|---|---|---|
| Refraction at the rim, frost, tint, specular edge, colour split | the glass's | as the repo and iOS 26 |
| **The amber plate** on the window in front - the title on it, its right edge slanted and eased | Kosmos's (Starlight, agreed) | made of amber glass, with a specular edge of its own; behind, it is the header's glass |
| **The four-point star** as the Kosmos mark, on the strip's Kosmos plate and the dock's button | Kosmos's (Starlight, I8) | unchanged, in the plate's ink |
| **The glint** under a running application in the dock, amber for the one in front | Kosmos's (Starlight) | a small star of light rather than iOS's dot |
| **Round window buttons**, today's colours, at the right | Kosmos's (Diego: I3 no) | glass beads - the colour is the bead's tint, a highlight at its upper left; grey glass on windows behind |
| **A warm rim on the window in front** | new, the glass's own signature | the front window's specular line is amber-white, every other rim cool: focus is shown by the light in the glass |
| **The star specular** | new | where the front window's rim highlight peaks, at its top-left corner, it ends in a small four-point star of light |
| **Amber and indigo, not a rainbow** | new | the colour split pulls the warm channels further than the cool ones, so an edge fringes amber on one side and indigo on the other - Kosmos's two colours - rather than red, green and blue |

Not taken from Starlight: the cut corners and the unfurl. Glass is liquid
because it is round, and a 45-degree cut through a bevel would bend light
along a crease; the roll is Starlight's motion. Liquid Glass opens with
4.4's fade and scale, and *Reduce motion* makes it a cut as everywhere.

### How each part would be built, and what it costs

Numbers are the M700 (i7-6700) at 1920x1080, from the measured 3.4 ns a
composed pixel (`testing.md` 18.657); everything else is **estimate**, and
the Pi 5 is taken as two to three times slower a pixel, as in 4. A frame is
16.7 ms. Under QEMU a composed pixel is 42 ns, so a glass desktop there is
a picture of the idea, not of its cost.

**1. Wallpaper glass - the backdrop of every frosted surface, on the
processor.** The window manager resamples the wallpaper once, in C, when it
or its fit changes. It makes a blurred copy then, and a glass surface shows
the part of that copy under it - **windows included**: a glass window shows
the wallpaper's blur behind it, not the windows behind it. This is G1 of
4.3 carried from the strip and the dock to windows, and it is the one
honest difference from real glass.

- *Once a wallpaper*: three box passes over 2.07 Mpx, 8-10 ms (**estimate**,
  4.3); 8.3 MB for the copy (19.8 MB at 3440x1440). Clear glass reads the
  sharp copy the window manager already keeps.
- *A frame*: a window's ground is translucent, so its pixel is blended over
  the blurred wallpaper rather than copied - two reads and a blend for one
  copy, about +1.5 ns a pixel (**estimate**). Paid only where damage is
  redrawn: for a caret or a row, nothing worth naming; for a whole
  1000x700 window - a drag, a resize, a full repaint - about **+1 ms** on
  the 2.4 ms it costs opaque; the strip +0.1 ms, the dock +0.2. A still
  desktop pays nothing.
- *What is under a glass window is never composed under it*: it reads the
  wallpaper, not the windows below, so overlap costs what it costs today.

**2. The rim - refraction, specular and the split - from a table.** The
repo's map, computed here once, in C, and kept:

- *Where it comes from*: a ray straight down meets a surface whose height
  across the bevel is a quarter circle; Snell's law at n = 1.5 bends it,
  and where it lands on the backdrop a glass-depth below is the pixel the
  rim shows. The specular comes from the same normal against a light at
  the upper left. In the `gfx` kit (`lens.c`), in doubles, when the table
  is made - never in a frame.
- *Smaller than the repo's cache*: the repo keeps a map per size. A rounded
  rectangle's rim is the same all along each straight edge and differs only
  in its four corners, so the table here is **per radius and bevel, not per
  size**: a profile of `bevel` entries for the edges and one corner
  quadrant of (radius + bevel) squared - for 18 and 12, 900 entries of
  three bytes (dx, dy, specular), 2.7 KB, made in about 0.05 ms
  (**estimate**) the first time a look meets that radius at that scale.
- *A frame*: only the rim's band, only where it is redrawn. A rim pixel is
  two gathers from the backdrop - the warm channels at the table's offset
  times 1 + split, the cool ones times 1 - split - and the specular added:
  about 6 ns (**estimate**). A 1000x700 window with a 12 px bevel has
  41 kpx of rim: **0.25 ms** when all of it is redrawn; the dock's 17 kpx
  0.1 ms; a menu's 0.05 ms.
- *One care*: under live glass (3) a rim gathers from inside its own
  rectangle, so the band and its reach are copied to a scratch before the
  surface is written over them - about 0.3 ns a pixel of the band.

**3. Live backdrop glass - what is really behind, blurred.** G2 of 4.3:
`SC_GLASS` in the scene; the back buffer at that moment *is* the backdrop;
the rectangle and the blur's reach are blurred from it into a quarter-size
scratch and the surface blended over that. Back to front, so a menu over a
glass window blurs the glass window, as it should. At about 3 ns a glass
pixel (**estimate**, 4.3), **paid whenever anything under the surface, or
the surface itself, changes**:

| Surface | Pixels with the reach | M700 | Pi 5 |
|---|---|---|---|
| The strip | 61 k | 0.2 ms | 0.4-0.6 ms |
| The dock | 120 k | 0.4-0.6 ms | 1-1.8 ms |
| A menu | 100 k | 0.3 ms | 0.6-0.9 ms |
| A banner | 45 k | 0.15 ms | 0.3-0.45 ms |
| Quick settings | 190 k | 0.6 ms | 1.2-1.8 ms |
| A 1000x700 window | 780 k | **2.3 ms** | 5-7 ms |
| A maximised window | 2.07 M | **6.2 ms** | 12-19 ms |
| Three windows overlapping | ~2.3 M | **7 ms** | 14-21 ms |

- *Dragging a window of live glass* redraws all of it each frame: 2.3 ms of
  blur on 2.4 ms of composing and 0.25 of rim - about **5 ms a frame on the
  M700**, 10-15 ms on a Pi (**estimate**): inside a frame on one, at its
  edge on the other.
- *A film under a window of live glass* pays its blur every frame of the
  film.
- *Clear live glass has no blur*: the interior is a blend (about 1 ns a
  pixel more) and only the rim gathers - the dock 0.1 ms, a whole window
  about 1 ms. Clear is cheap live; frosted is what costs.
- So, on the processor: **live glass for the floating surfaces, which are
  small; wallpaper glass for windows.**

**4. On the GPU** (`m700-2d.md` 7d). On the render engine the blur is two
passes of a shader over a quarter-size texture and the rim is one lookup in
the window's own shader, its table a small texture: the processor pays
nothing, and live glass for every window becomes affordable - the render
engine's time for a quarter-size blur of the whole screen should be a few
tenths of a millisecond (**estimate**; nothing measured). What it takes is
7d itself, not built, and programs on the execution units beyond 7c's
fixed-function blend - a sampling shader, of the kind `gpu kc` already
runs. Where there is no GPU - QEMU, the Pi today - 1 to 3 above.

**5. The cues** cost what Starlight's do: the plate a trapezoid when the
window in front changes; the warm rim a second specular table, swapped on
a focus change with the two rims redrawn (0.25 ms for a large window,
once); the star specular a 9 px sprite; the beads the lights' slots drawn
as a picture (`SC_LIGHTS`); the glints a sprite a cell.

### What applications have to do

Almost nothing, if they draw with the kit - which is what the kit is for.

- **The kit** (`user/lib/ui.lua`): the grounds it paints - the window,
  `ui.header`, `ui.sidebar`, toolbars, status lines, list grounds, menus -
  are painted with the look's glass tokens rather than `theme.window`,
  `raised` and `sunken`: a colour with alpha (`theme.lua` already reads
  `#AARRGGBB`). A window in a glass look is flagged `SC_BLEND` and
  `SC_GLASS` by the window manager; the application never sees it.
- **Words on a translucent ground**: the kit's text has to raise the
  destination's alpha as it writes coverage (source-over, premultiplied).
  **To be checked** in `gfx`'s text path first; if it does not, that is a
  `gfx` change, and every application gets it at once.
- **Content stays opaque by saying so**: a kit call that marks a rectangle
  as content (`ui.content`, or the canvas and page widgets declaring it),
  painted with `sunken` or the document's own white. Write's page, JetSki,
  the film, Cafesa3D and the games draw their own pixels and are opaque
  already; nothing changes for them.
- **Applications that paint their own ground by hand** - those 3.4 and 3.5
  list, and Write's `pk.header` - show as opaque islands in a glass window
  until A2 moves them to the kit. That is a reason for A2 to come first,
  not new work.
- **The look is a file**: `user/themes/LiquidGlass.theme` and its dark
  partner, with new keys - `glass` (`frosted` or `clear`), `glass_tint`,
  `glass_blur`, `bevel`, `refraction`, `split`, `specular`, `rim_front` -
  read by `theme.lua` and absent from every other look. **One door**: the
  blur and the lens are the `gfx` kit's, the same blur Starlight's nebula
  glass (I5) and every look's G1 use; nothing about glass is written twice.

### The order, each step with its test

Each step touches the kit, the scene or the window manager, so each runs
the whole gate. It stands on A1 (tokens) and A2 (applications on the kit)
of section 6.

1. **LG1 - the look files and their tokens.** The two `.theme` files, the
   glass keys in `theme.lua`, Appearance listing Liquid Glass. Opaque at
   first: the tints as solid colours. *Test*: `tools/test_theme.lua` holds
   both looks to every token and the glass keys to their ranges.
2. **LG2 - the blur, and the wallpaper's blurred copy.** `blur.c` in `gfx`,
   vector types for NEON and SSE2 with the scalar loop as the reference;
   the window manager makes the copy when it resamples. *Test*:
   `tools/test_blur.c`, vector against scalar, natively and through
   Rosetta; the same step as A4's.
3. **LG3 - the lens.** `lens.c`: the Snell table per radius, bevel and
   scale, and the rim pass in the scene. *Test*: `tools/test_lens.c` holds
   every entry to a ray traced in doubles (within a pixel), checks that the
   edge profile equals the corner's along its axes, and that a rim with
   refraction 0 is the backdrop exactly.
4. **LG4 - wallpaper glass everywhere: floating surfaces first, then
   windows.** `SC_GLASS` reading the blurred copy; the strip, dock, menus,
   banners, quick settings; then windows' grounds through the kit. *Test*:
   a display suite in Liquid Glass reads a sidebar pixel as the blurred
   wallpaper under the tint, a rim pixel as the wallpaper at the table's
   offset, and a pixel of Write's page as opaque white; `frames` dragging a
   window on the M700 under 8 ms a pass.
5. **LG5 - the cues.** The amber glass plate, the beads, the warm rim and
   the star specular on the window in front, the star on the strip and the
   dock, the glints. *Test*: a focus change redraws only the two plates
   and the two rims (`frames`' damage), and the gallery shot in Liquid
   Glass.
6. **LG6 - live glass for the floating surfaces** (G2), frosted and clear,
   behind Appearance's choice. *Test*: `x86-scene` with and without
   `SC_GLASS` over 1500 passes, against a reference blur of the same back
   buffer; `frames` with a film under the dock, under 1 ms a pass more.
7. **LG7 - live glass for windows, on the GPU**, with 7d; on the processor
   only as a choice that says what it costs. *Test*: the GPU's glass
   compared pixel for pixel with the processor's, as 7c's blend was.

### Liquid Glass decisions for Diego

1. **L1 - Two looks, light and dark**: *Liquid Glass* and *Liquid Glass
   Night*, beside the eleven and Starlight, not the default. *Recommended:
   yes* - the light one on Endeavour's colours, since it is "the current
   astra".
2. **L2 - Windows on wallpaper glass** on the processor: a window's glass
   shows the wallpaper behind it, not the windows behind it.
   *Recommended: yes* - it is the only way a whole window is glass at no
   cost a frame; live glass for windows waits for the GPU.
3. **L3 - Content stays opaque**: documents, pages, films, viewports and
   games are never glass. *Recommended: yes*, as iOS 26 does; a
   Terminal's ground is the one place to try frosted content, as a choice
   of its own.
4. **L4 - Frosted for everything with words, clear for the dock and the
   strip.** *Recommended: yes*, with a choice in Appearance to make all of
   it frosted.
5. **L5 - Kosmos's cues**: the amber glass plate, the star, the glints, and
   the beads (I3 holds: round, today's colours). *Recommended: yes*, all
   four.
6. **L6 - The glass's own signatures**: the warm rim on the window in front,
   the star specular, the amber-and-indigo split. *Recommended: yes* to
   the warm rim and the split; the star specular yes, on the window in
   front only, so it stays a mark and not a sparkle everywhere.
7. **L7 - Live glass (G2) for the dock, menus, banners and quick
   settings**, behind Appearance's choice. *Recommended: yes*, the default
   on a machine where `frames` says it fits.
8. **L8 - Reduce transparency**: a choice that makes every glass surface
   its tint, solid. *Recommended: yes* - iOS and the repo both honour it,
   and it is also the switch for a slow machine.
9. **L9 - Where it goes in the queue**: after A1-A4, since it stands on the
   tokens, applications on the kit, and the blur. *Recommended: yes*;
   LG1-LG3 are shared with A4 and Starlight's I5, so nothing is done
   twice.

---

## 1. In short

*Written before Identity above; read with it.*

- **Astra is already most of a modern desktop.** Windows with rounded
  corners and soft shadows composed in C, a floating dock, an app switcher,
  window snapping (Magnet), notifications with a history and Do Not
  Disturb, eleven looks, a scale up to 200 per cent, and a kit every window
  is drawn with. The bones are right.
- **What makes it feel unfinished is unevenness, not missing effects.**
  Nine different corner radii, nine type sizes chosen app by app because
  every look sets all five text roles to the same 18 pixels, three ways of
  showing the chosen row in a sidebar, a switch drawn by hand in the
  notification centre, and the dock's corners told they match the windows'
  when they do not. Each is small; together they are what the eye notices.
- **The drawings promised glass and the desktop has smoke.** The dock, the
  switcher, the notifications and quick settings were all agreed with a
  blur behind them (`dock.html`, `switcher.html`, `notifications.html`). The
  dock that was built is a see-through fill with nothing blurred, and
  shadows are *off* by default.
- **Glass is affordable on the processor if it is done in the right
  order.** First blur the *wallpaper*, once, when it changes, and let the
  strip, dock, menus and banners show that (what Windows 11 calls Mica):
  nothing per frame. Then real blur of what is behind those few small
  surfaces, only when what is behind them changes - about half a
  millisecond on the M700, estimated. Whole see-through windows wait for the
  GPU compositor (`m700-2d.md` 7d), which was always the road for them.
- **Motion is the cheapest big win left**: a window that grows in when it
  opens, the minimise outline Diego asked for on 3 October, menus that fade.
  A few frames each, never in the way of a key.
- **What is missing that every desktop has**: quick settings (agreed in
  October, not built), a search for files (wanted 8 October), an overview of
  every window, workspaces, light and dark by the time of day, a night
  light, a lock screen, clipboard history, screen recording, and the
  accessibility basics.
- **Recommended**: first one sheet of tokens and the inconsistencies fixed
  against it (a week), shadows on; then glass in two steps, motion and quick
  settings; then the missing features in the order of section 5.

---

## 2. Where Astra stands

**Strong, and seen:**

- **One kit, one look across forty applications.** Files, Preferences,
  Passwords, Servers and Maps in the gallery share the header, the sidebar,
  the row pitch and the faces (`ui.header`, `ui.sidebar`, `ui.list`). The
  header is the title bar (`ui.md` 16.25), which is the modern shape.
- **A fixed layout that holds in every look** (`theme.lua`, `theme.metrics`:
  row 32, button and field 31, Deskbar 32, gap 12), held by
  `tools/test_theme.lua`. Looks change colour without moving a widget.
- **Composing is C, and every window draws itself** (`astra-display.md`
  D2, D3a): the scene in `user/kits/gfx/scene_core.c` composes damaged
  rectangles, rounds corners (`SC_ROUNDED`), casts shadows (`SC_SHADOW`,
  `shadow.c`, SIMD darkening) and blends strips (`SC_BLEND`). On the M700:
  3.4 ns a pixel, a full-screen drag 7.5 ms a pass (`testing.md` 18.657),
  and a window dragged in circles 1.37 ms of composing a frame (18.661).
- **Modern pieces already there**: Magnet snapping with keys
  (`wm.lua` 2749, Preferences' Magnet page), Super+Tab's switcher
  (`user/lib/wm/switcher.lua`), notifications with history and Do Not
  Disturb (`notifications.lua`, `notifyd`, `settings.lua` 375), a floating
  dock with sizes and a transparency (`dock.lua`, `deskbar.lua` 1740),
  a launcher pad with Recently used and a power row, a scale of 100 to 200
  per cent (`settings.lua` 241), focus follows the pointer, a level display
  that fades (`user/lib/wm/osd.lua`), and a hardware cursor on virtio-gpu
  and the M700.
- **Icons with a character**: Haiku's set for applications, Lucide line
  icons for controls, and Kosmos's own drawn in Haiku's manner
  (`tools/appicons.py`).

---

## 3. Inconsistencies

Each with where it is. Ordered by how much the eye notices it.

### 3.1 Corners: nine radii for four kinds of thing

- A window and a menu: `theme.CORNER = 10` (`user/lib/theme.lua` 310), the
  same in every look since 8 October.
- The dock: `dock.RADIUS = 18` at every size (`user/lib/dock.lua` 33,
  `dock.SIZES` 52-57), and its comment says "a window's (`theme.CORNER`)" -
  true for one morning on 8 October, when the corner was 18, and stale
  since that afternoon's revert to 10.
- `theme.lua`'s own comment above `theme.metrics` still says "a window's
  corner 12 (it was 8)".
- A control: `CONTROL_R = 7` (`ui.lua` 732), `pixelkit.lua`'s `R = 7`
  (line 37), the sidebar's `SIDE_R = 7` (`ui.lua` 3004).
- Cards and panels, literal in each application: 8 (`calc.lua` 226,
  `preferences.lua` 588), 9 (`mixer.lua` 181, `machine.lua` 656), 10
  (`launchpad.lua` 383, `tracker.lua` 1610), 11 and 12 (`ui.lua` 3826-3827),
  12 and 14 (`launchpad.lua` 315, 355), 18 (`notifications.lua` 519, 557;
  `tracker.lua` 1503).

There is no radius token for anything but a window. **Fix**: a scale of
three - control 7, card 12, window and dock 12 or 10 - in `theme.metrics`,
and the literals replaced.

### 3.2 Type: one size in every role, so every application makes its own

Every one of the eleven looks sets `font.title`, `font.ui`,
`font.heading`, `font.text` and `font.label` to IBM Plex Sans at **18**
and `font.mono` to 16 (`user/themes/*.theme`). Headings differ from text
only by weight. So applications that need a hierarchy reach for
`ui.sized` with their own numbers: **12, 13, 14, 15, 18, 19, 21, 22 and
24**, across 16 files (`notifications.lua` 139-141 are 13 and 15 bold;
`grep -rn 'ui.sized(' user/bin/apps user/lib`).

**Fix**: a type scale in the look - caption 13, small 15, body 18, title 18
semibold, heading 22, display 28 - five sizes that cover what the
applications already chose, as named roles, held to the fixed layout by
`tools/test_theme.lua` as the faces are today.

### 3.3 The chosen row in a sidebar, three ways

- **The kit's** (`ui.sidebar`, `ui.lua` 2988-3005, and `ui.md` 16.21): a
  quiet rounded fill in `line_soft`, the words still dark, the icon in the
  accent - "a quiet selection, because the accent-filled bar a file list
  uses shouts in a list whose job is only to say where you are". Files,
  Preferences, Passwords, Servers and Shortcuts use it.
- **Sandboxes'** (`sandboxes.lua` 415 and 480): its own sidebar, the chosen
  row filled with `theme.accent`, white words, 30 pixels high rather than
  the kit's 35-pixel pitch, and the same sandbox picture on every row
  (gallery: All, Running, Stopped, Tasks).
- **The launcher pad's** (`launchpad.lua` 313-314): a lighter tile with a
  radius of 12 and a three-pixel accent bar at its left edge
  (`build/dock/launcher.png`).

Besides those two, Packages (`packages.lua` 278-282, "what to look at")
and Mail (its folders) draw a navigation sidebar of their own rather than
calling `ui.sidebar`. Automations' left column is a list of *things*, where
the accent is right by the same rule. **Fix**: the four navigation
sidebars moved to the kit's, which is what 16.21 already wrote down.

### 3.4 Controls drawn by hand beside the kit's

- **The notification centre's Do Not Disturb switch** is painted with two
  `fill_round`s (`notifications.lua` 530-531), with a literal grey
  `0xff4a5468` as its fallback track and a fixed white knob, instead of
  `ui.switch` - which Preferences' own Do Not Disturb uses. The two
  switches for the same setting look different.
- **Two copies of the header and the controls**: `ui.lua` (`ui.header`,
  `ui.button`, `ui.iconbutton`) and `pixelkit.lua` (`pk.header`,
  `pk.button`, `pk.iconbutton`), "the same header and the same controls, at
  the same numbers". Since D2 every window draws itself, so the reason for
  two (a direct window could not send commands) has gone; `pixelkit.lua`'s
  own comment says "none of them draws with it today", which stopped being
  true when Write took `pk.header` (`review.md` section 7). A second copy
  is a defect by the premise (`CLAUDE.md`, *Kits supply*).
- **A menu's lit row is a square accent bar** edge to edge
  (`window:paint_menu`, `ui.lua` about 9497, `g:fill(2, y, m.w - 4, ...)`),
  inside a menu the compositor rounds at 10. Every other lit thing - the
  sidebar's row, a launcher tile, a list's selection - is an inset rounded
  shape.

### 3.5 Colours outside the look

Literal `0xff......` colours per application, top of the list: Chooser 21,
Paint 19, Maps 15, Cafesa3D 15, Sandboxes 12, LLM Monitor 12, Scheduler 11,
Linux 11, Camera 11, Disk Monitor 9 (`grep -cE '0x[fF]{2}[0-9a-fA-F]{6}'`).
Many are right - a map's water, a graph's series, Groove's instrument look
(`review.md` calls that a stated exception). The ones that are not:

- the Deskbar's tip, `TIP_BACK = 0xf0202227` (`deskbar.lua` 2008), and the
  switcher's panel, `PANEL = 0xf0262a31` (`switcher.lua` 41): both
  deliberately dark in every look, which is right for an overlay, but each
  its own number - they want one `overlay` token, which glass would then
  become;
- `theme.bad or 0xffe04848` (`deskbar.lua` 1365, 1488) and the launcher
  pad's mixes towards `0xffd6e4ff` (`launchpad.lua` 347): fallbacks for
  tokens every look has.

### 3.6 Shadows off, so dark windows merge

`settings.lua` 263-268: *Drop shadows*, "Costly on a slow machine",
**`default = false`**. In Night (`build/x86_64/dock-shortcuts.png`) the
Shortcuts window sits on Calculator on the Monitor with nothing between
them but a hairline. The cost that made it off was fixed on 24 September
(`shadow.c`: only the band, table falloff, `gfx_darken_span` in NEON and
SSE2), and the thin single shadow Diego found worse than the drawings'
(`roadmap.md` 6zk, two layers: 40 px at 28%, 6 px at 20%) is still to do.

### 3.7 What was drawn with glass was built without it

`docs/dock.html` 188 (`backdrop-filter: blur(20px)` on the dock), 204 (the
launcher), 219 (quick settings), `switcher.html` 81 (`blur(18px)
saturate(1.3)`), `notifications.html` 168: all agreed as drawn. The built
dock is `theme.window` at Appearance's transparency (`deskbar.lua`
1740-1752), with nothing behind it blurred: at 30-50% it reads as smoke
over whatever is under it, and at 100% it is gone. The strip in dock mode is
fully transparent and its words carry a one-pixel shadow (`deskbar.lua`
2118-2121) - legible on the M700's dark-orange wallpaper
(`build/m700-verge.png`), not on a white one (`white-turbine.jpg` ships).

### 3.8 The bar with thirty applications

In the bar at the top (the gallery's), every running window is an entry
of equal width, and with thirty the name is cut to "…" in every one
(`docs/screenshots/2026-11-17-2019-5b2545b3.png`; `deskbar.lua` 1475-1494,
`ui.fitted`). The dock groups by application (`dock.lua` 16-20); the bar
does not, and has no overflow. Grouping by application, as the dock does,
with a count, is the fix.

### 3.9 Section headings, two styles

Uppercase small captions - Maps' "PINNED", "SAVED", Sandboxes'
"SANDBOXES", Shortcuts' "KOSMOS", "MADE HERE" - beside sentence-case
headings in the same weight and place - Files' "Network", Disk Monitor's
"Drives", Preferences' "Look", Servers' "Servers" (gallery shots). One
kit call, `ui.section` or a `heading` role in the type scale, decides it.

### 3.10 Icons: one picture for six applications, and colour where lines are

- Six demos wear the same cube (`build/dock/launcher.png`: 3D Cube, GL
  Bounce, GL Cube, GL Gears, GL Mech, GL Morph 3D, GL Spin, GL Textures).
  `tools/appicons.py`, being written now, is the way out.
- Sandboxes' and Shortcuts' sidebars put a colour application picture on
  every row where every other sidebar has a line icon (gallery;
  `dock-shortcuts.png`).

### 3.11 Words that say two things

- **"Astra"** is the whole desktop (`CLAUDE.md`, 7 October) and also one
  desktop file, `user/desktops/Astra.desktop`, the default beside
  `Pewter.desktop` (`desktops.lua`: `M.DEFAULT = "Astra"`). So "Pewter is a
  desktop of Astra's" and "Astra or Pewter at boot" are both said.
- **The look's principle**: `ui.md` 16.8b and `theme.lua`'s opening say
  "Kosmos is dimensional, and it means it", while the default look,
  Endeavour, is `flat = yes`, as are Plex and Night. The principle is
  written and no longer true, which `CLAUDE.md` says must not happen.
- `theme.lua`'s fixed-layout comment says "a list or menu row is 24
  pixels, a button 28, a field 26"; the table under it says 32, 31, 31.

### 3.12 Smaller

- **Window buttons**: macOS's colours in Windows's order - green maximise,
  yellow minimise, red close, at the right (`wm.lua` 1806-1829). Diego chose
  it on 23 September; noted only because a person who knows either system
  will reach for the wrong one once.
- **Empty states**: "Nothing is kept yet.", "No sandboxes yet - ...",
  "No automations yet. + New makes one.", "Nothing has been said"
  (`notifications.lua` 545) - each placed and worded its own way. A kit
  `ui.empty{ picture, words, action }` would make them one.

---

## 4. Making it nicer

Every number here is from the code or `testing.md` unless marked
**estimate**. The machine numbers are the M700 (i7-6700); the Pi 5 is
taken as two to three times slower per pixel (**estimate**, no Pi
measurement exists). A frame at 60 Hz is 16.7 ms; 1920x1080 is 2.07 Mpx.

### 4.1 Tokens first: one sheet

Before any effect, the look gains what it lacks: `radius` (control, card,
window), a type scale (3.2), `overlay` (the dark of a tip and the
switcher), `glass` (a tint and its strength, per look), `shadow` as two
layers (6zk), and motion durations. **Cost**: none per frame; it is
`theme.lua` and the `.theme` files. **Recommended**: yes, first - every
other step reads these.

### 4.2 Shadows, on and layered

- **What**: 6zk's two layers - a wide soft one and a tight one - from the
  table `shadow.c` already keeps; and **on by default**.
- **Where**: `shadow.c`, `scene_core.c` `cast_shadow`; sizes in
  `theme.metrics`.
- **Cost**: today's band for a 1000x700 window with a 14 px spread is
  about 48 kpx; at 40 px about 150 kpx. Darkening is one multiply a pixel
  in four lanes; at about 1 ns a pixel (**estimate**, the same loop shape as
  the composing that measures 3.4) that is 0.05 ms and 0.15 ms per window
  when all of it is redrawn - and a drag redraws only the damage. Measured
  before it lands with `frames` dragging four windows, as 6zk asks.
- **Off switch**: stays, in Appearance.
- **Recommended**: yes, on by default once `frames` says a drag stays under
  8 ms a pass on the M700.

### 4.3 Glass, in three steps

A glass surface is one that shows what is behind it **blurred**, tinted
with the look's colour, with a hairline at its edge. Without the blur,
see-through is legibility lost for nothing (3.7).

**Which surfaces**: the strip, the dock, menus, notification banners and
the history, the switcher, the launcher pad, quick settings. **Never** an
ordinary window on the processor - a window is up to 2 Mpx, and its
content is what a person reads.

**Step G1 - wallpaper glass** (Windows 11's Mica). The window manager
already resamples the wallpaper once, in C, when it or its fit changes
(`roadmap.md`, *The wallpaper fills the screen*). It makes a second copy
then, blurred, and a glass surface shows the part of that copy under it,
tinted. Over a window it shows the wallpaper's blur, not the window's -
the one honest difference from real glass.
- **Cost**: a blur of 2 Mpx once per wallpaper change - about 8-10 ms on
  the M700 (**estimate**: three box passes at ~1.5 ns a pixel a pass); per
  frame, a tinted copy, which is what composing a blended strip already
  does. **Nothing added per frame.**
- **Memory**: one more screen-sized surface, 8.3 MB at 1920x1080, 19.8 MB
  at 3440x1440.

**Step G2 - backdrop glass**. The surface shows what is really under it,
blurred. In the scene: a window flagged `SC_GLASS`; when any damage touches
it or the blur's reach around it, the whole of it is redrawn; the layers
below are composed into the back buffer as now; that rectangle, plus the
blur's reach, is blurred **in place from the back buffer** into a small
scratch at a quarter of the size; then the surface is blended over it.
The back buffer already *is* the backdrop at that moment, so nothing is
composed twice.
- **Blur**: dual-Kawase or three box passes on the quarter-size copy -
  down twice, blur, up twice; about 2.5 full-size pixel touches per glass
  pixel, SIMD in `gfx` with a scalar reference held by a host test
  (`tools/test_blur.c`), as `test_pack.c` holds `pack.c`.
- **Cost** (**estimate**, about 3 ns a glass pixel on the M700): the strip
  61 kpx and the floating dock with its reach about 120 kpx - **0.5-0.6 ms**
  when either is redrawn; an open menu (100 kpx) 0.3 ms; the launcher pad
  (490 kpx) 1.5 ms while it is open. Only when something under them
  changes: a still desktop pays nothing; a video playing under the dock
  pays it every frame, bounded by the dock's size. Pi 5: 1-1.8 ms for strip
  and dock (**estimate**).
- **Budget held by a test**: `frames` on the M700 with a film under the
  dock, glass on and off, the difference under 1 ms a pass.

**Step G3 - glass on the GPU** (`m700-2d.md` 7d). On the render engine a
blur is a two-pass shader over a texture and costs nothing the processor
sees; there translucent *windows* - a terminal you can see through - become
affordable. Where there is no GPU (QEMU, the Pi until it has one), G1 or G2.

**Off switch**: Appearance's *Transparency* becomes four choices - Off
(opaque), Tinted (today's), Wallpaper glass (G1, default), Glass (G2) -
and the dock's own percentage folds into it.

**Recommended**: G1 now and as the default; G2 after, behind the choice,
the default where it measures under budget; G3 with 7d.

### 4.4 Motion

What is animated, and the budget each must hold. All on the window
manager's own clock, eased; **a key or a press never waits for one** - the
window is where it is going from the first frame, and only its picture
catches up.

| What | Duration | Per frame, M700 (**estimate**) |
|---|---|---|
| A window opens: from 96% and transparent to 100% and opaque | 150 ms, 9 frames | its rectangle stretched and blended: a 1000x700 window 0.7 Mpx at ~4 ns, 2.8 ms |
| A window closes: the reverse, from its last picture | 120 ms | the same |
| Minimise and restore: the outline from the frame to its dock cell (Diego, 3 October, under 0.5 s, `roadmap.md`) | 200 ms | four thin rectangles: nothing |
| A menu or popup: fades in | 80 ms | its own size, blended |
| The switcher, the launcher pad, quick settings: fade and rise 8 px | 120 ms | their own size |
| Magnet's preview: the target area fades in | 100 ms | an outline and a tint |
| A banner: slides in from the right | 160 ms | its own size |

- **Where**: the scene gains a per-window opacity and a scale
  (`scene_core.c` already stretches with alpha, `gfx.c` `stretch`); the
  timing and easing in the window manager (`user/lib/wm/`), which decides.
- **Pi 5**: the 2.8 ms is 6-8 ms (**estimate**), still inside a frame;
  a window larger than half the screen can skip its scale and only fade.
- **Off switch**: *Reduce motion* in Appearance, which makes every one of
  them a cut, and is also what accessibility asks for.
- **Recommended**: yes, with the minimise outline (already agreed) first.

### 4.5 Rounded corners, hover, press and focus

- Corners: from the token sheet (4.1); the menu's lit row inset and rounded
  (3.4). Cost: none measurable - `gfx_round_cover` is asked only in the
  corner squares.
- **Hover**: the kit has almost none - three mentions of `hover` in
  `ui.lua`'s 10,708 lines. A row, a tool button and a dock cell lit faintly
  under the pointer, from what the window already knows (the
  *instant feedback* rule: painted at the move, never waiting). Needs the
  pointer's position delivered to a window between presses, which the
  window manager has (`PT`, `wm.lua`), as a `hover` event at most once a
  frame.
- **Press**: a pressed control darker by one step, drawn at the press.
- **Focus ring**: exists (`theme.ring`, `ui.lua` 1840, 1987, 2117), drawn
  only when the keyboard moved the focus (`self.keyed`). Right; its
  thickness and offset become tokens so every control draws the same ring.

### 4.6 Accent colour

Today the accent is the look's. A choice of eight accents in Appearance,
over any look, writing `accent`, `ring` and the selection's tint - **not**
`tab`, which Diego made the Deskbar's and the title's one token on 22
September. Cost: none; the theme table is mutated in place
(`theme.lua` opening) and every window repaints. **Recommended**: yes.

### 4.7 Icons

Finish `tools/appicons.py`'s drawn icons for every application that wears
another's (the six demos first), and a rule in the kit: a sidebar row
carries a line icon, an application a Haiku-style one. **Recommended**: yes,
the main session's work already.

---

## 5. Modern features: what exists, what does not

Checked against the code. Ranked by value for Diego's daily use on the
M700.

| # | Feature | Today | Value |
|---|---|---|---|
| 1 | **Quick settings** - volume, brightness, network, servers, screenshot, Do Not Disturb, restart and shut down, one panel under the strip | **Agreed, not built** (`roadmap.md`, the dock's step 5; drawn in `dock.html` 219). The strip's indicators open whole applications - Mixer and Network (`deskbar.lua` 1524, 1528, 2216-2217) | High: touched many times a day |
| 2 | **Search for files** from the launcher (Spotlight) | **Wanted, 8 October** (`roadmap.md`, launcher step 4); the launcher finds applications only, Tracker's search filters one folder (`review.md` 3.9) | High |
| 3 | **Shadows and glass** | Shadows exist, off by default; glass none (3.6, 3.7) | High: the whole look |
| 4 | **Lock screen** | None (`roadmap.md`, *Login, lock screen and encryption at rest*) | High once it is the daily machine |
| 5 | **Clipboard history** | None; the clipboard is 1,900 bytes of text (`review.md` 3.1). After the clipboard is reshaped | Medium-high |
| 6 | **Overview** - every window side by side, scaled, chosen with the pointer | None. The switcher has a row of windows (`switcher.lua`); the window server already holds every window's picture, and `stretch` scales them | Medium-high |
| 7 | **Workspaces** | None in Astra; drawn for Corona (`docs/corona.html`, `roadmap.md` 803-817) | Medium |
| 8 | **Light and dark by the time of day** | None: a look is chosen by hand (`settings.lua` 153). Pairs exist: Plex and Plex Night, Endeavour and Night | Medium |
| 9 | **Night light** | None. On the M700 the display engine's gamma table makes it free; elsewhere a multiply on the final copy of the damage, about 1 ns a pixel (**estimate**) | Medium |
| 10 | **Screen recording** | None. The Record Kit encodes H.264 into MP4 already (`user/kits/record/`), and the scene keeps a copy for the VNC watcher - a kit reused, not written | Medium |
| 11 | **Accessibility**: high-contrast look, reduce motion, zoom, larger text | Scale 100-200% exists (`settings.lua` 241); larger text alone is refused by the fixed layout (`roadmap.md` 5x) and the scale is its answer; no contrast look, no zoom, no screen reader (`roadmap.md`, *Typing every language*) | Medium |
| 12 | **Wallpaper slideshow** | None; twelve wallpapers ship (`assets/wallpapers/`) | Low |
| 13 | **Notification actions** (Reply, Open) and Do Not Disturb by hours | History and DND exist; hours "later" (`roadmap.md`, Notifications) | Low-medium |
| 14 | **Dock: names on hover, thumbnails of a running application's windows** | Name on hover "left for later" (`roadmap.md`, dock step 3) | Low-medium |
| - | Snapping | **Exists**: Magnet, with keys and a Preferences page | - |
| - | App switcher | **Exists**: Super+Tab, Alt+Tab | - |
| - | Notification centre, history, DND | **Exists** | - |
| - | Scaling | **Exists**: 100, 125, 150, 200 | - |
| - | Screenshots | **Exists**: Super+Shift+3, Print, into Captures (`user/lib/wm/keys.lua` 382, 491) | - |
| - | Focus follows the pointer | **Exists** | - |

---

## 6. The order of work

Small steps, each a few days, each with its test and the desktop working
after it. **The whole gate for each**: every step touches the kit, the
scene or the window manager, which every application stands on.

1. **A1 - the token sheet.** `radius`, the type scale, `overlay`, `glass`,
   two-layer `shadow`, motion durations, in `theme.lua` and every `.theme`.
   The stale comments in `theme.lua` and `dock.lua` corrected. *Test*:
   `tools/test_theme.lua` holds every look to every token, and a host check
   that fails on a literal radius or `ui.sized` size in `user/bin/apps`
   outside a short list of stated exceptions (Groove, Cafesa3D's viewport,
   the monitors' graphs).
2. **A2 - the inconsistencies against it.** Sidebars to `ui.sidebar`
   (Sandboxes, the launcher pad, Packages; Mail's if it fits), the notification centre's switch to `ui.switch`, the menu's
   lit row inset, one section heading, the dock's radius, an `ui.empty`.
   *Test*: each application's suite, and the dated gallery shot compared
   before and after.
3. **A3 - shadows on, in two layers** (6zk). *Test*: `tools/test_shadow.c`
   for the falloff, `frames` dragging four windows on the M700 under 8 ms a
   pass, the gallery shot.
4. **A4 - wallpaper glass (G1)** for the strip, dock, menus, banners, the
   switcher and the launcher pad; Appearance's *Transparency* as four
   choices. *Test*: `tools/test_blur.c` (the SIMD blur against its scalar
   reference, natively and through Rosetta), `x86-dock` reading a pixel of
   the dock as the blurred wallpaper's, tinted.
5. **A5 - the minimise outline and window open and close** (4.4), with
   *Reduce motion*. *Test*: a suite that opens and minimises with motion
   on and checks the first frame already takes keys; `frames` during an
   open, under one frame's time.
6. **A6 - quick settings** (agreed), on glass. Drawn already in
   `dock.html`; a feature list, architecture and diagram before code, as
   every new window gets. *Test*: `x86-quick`, every tile turned and read
   back.
7. **A7 - backdrop glass (G2).** *Test*: `x86-scene` composing with and
   without `SC_GLASS` over 1500 passes, the glass region compared against
   a reference blur of the same back buffer; `frames` with a film under the
   dock on the M700, under 1 ms a pass more than without.
8. **A8 - accent choice, light and dark by the time, night light.**
9. **A9 - the overview, then workspaces** - each drawn first.
10. **A10 - glass and motion on the GPU** with `m700-2d.md` 7d.

Search for files (Spotlight), the lock screen and clipboard history are
larger than Astra's look and belong to their own designs (`roadmap.md`);
they are ranked in section 5 so they are not forgotten here.

---

## Decisions that are Diego's

**DECIDED on 17 November 2026: every recommendation, as written.** Diego: "Yes to all recommendations" - the review's ten, beside Starlight's and Liquid Glass's.

Each with a recommendation first. The identity's own decisions, I1-I8,
are at the end of *Identity* above and come before these; where one of
them changes an answer here, it says so.

1. **Glass: how much, and by default?** *Recommended: wallpaper glass (G1)
   as the default on every machine, real backdrop glass (G2) as a choice
   that becomes the default where `frames` says it fits, and see-through
   windows only on the GPU (G3).* Off and Tinted stay.
2. **Shadows on by default, in two layers.** *Recommended: yes*, after the
   drag is measured on the M700.
3. **Motion: yes, short, with Reduce motion.** *Recommended: yes* -
   open 150 ms, close 120, minimise outline 200, menus 80 - and the
   minimise outline first, since it is already agreed. With Starlight
   (I6), opening and closing are the unfurl and the roll.
4. **The look's principle.** `ui.md` 16.8b says dimensional; the default
   look is flat. *Recommended: rewrite 16.8b to say a look chooses flat or
   dimensional, and depth in the default comes from shadows and glass
   rather than bevels* - Classic and Studio keep their bevels.
5. **One sheet of tokens, enforced by a check.** *Recommended: yes*: three
   radii, five type sizes, and a host check that fails on new literals.
6. **An accent colour of the person's own**, over any look, never touching
   `tab`. *Recommended: yes.*
7. **The bar at the top groups by application**, as the dock does.
   *Recommended: yes*, with a count beside a picture with more than one
   window.
8. **The name of the default desktop file.** "Astra" is the whole desktop
   and also `Astra.desktop`. *Recommended: keep Astra for the whole, and
   give the default desktop file a name of its own* - Diego's to choose.
9. **Where this goes in the queue.** *Recommended: A1-A3 inside the
   daily-driver season `review.md` proposes* (cheap, and they are what the
   eye meets every minute); A4-A7 after it; A8-A10 later.
10. **The window buttons' order.** *Recommended: keep* Diego's 23 September
    choice of slots; their shape is I3.
