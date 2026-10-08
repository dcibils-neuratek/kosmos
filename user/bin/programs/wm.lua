-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: server
-- kosmos: needs processes screen network audio camera midi profile keyring tiles
--
-- `network` is here so the desktop can *pass it on*. The kernel refuses a
-- spawn that hands over authority the parent does not hold, so without this
-- an application declaring `needs network` fails to start with "no process"
-- - which says nothing about the card. init grants it to the shell for the
-- same reason and the shell to this.
--
-- `audio` is here for the same reason: it is the audio band (`kernel/sched.h`,
-- `roadmap.md` 4i), which an application that makes sound - Groove - puts
-- its sound thread in, and which this must hold to pass on. It used to be
-- the sound device itself, held so the Deskbar could draw the volume; the
-- Deskbar reads that from `/Devices/audio` as any client does, and the
-- device is the audio server's alone.
--
-- And `profile` for the same reason: `profile`, run from a Terminal this
-- starts, sees where every processor is (`kernel/profile.c`).
--
-- **None of them widens what an application can reach.** `init.lua` grants the
-- network only when the machine has a card, and a child gets a grant only
-- when its own header declares it needs one. So this is a conduit rather
-- than a store, which is the same arrangement `network` has had since the
-- browser.
-- The window manager: windows, decoration, stacking, focus, and the
-- compositor underneath them.
--
--   wm                    a desktop: the notifications, the backdrop and
--                         the Deskbar
--   wm hello-win          start /Kosmos/Apps/hello-win.lua in a window
--   wm hello-win,stuck    two applications, one of which hangs
--   wm gallery,setprop:/Running/gallery/title=hello
--                         and one that changes the other's title
--
-- Control-W then Q gives the screen back to the shell.
--
--------------------------------------------------------------------------
-- Why this is a separate process from the applications it draws.
--
-- The definition of done for this milestone is BeOS's test: drag a window
-- with a hung application inside it and have the window keep moving. That
-- is not a performance question, it is a question about who owns the
-- pixels. Here the application owns none of them. It sends a list of
-- drawing commands - `ui.md` 16.6, commands and not a shared buffer - and
-- this process renders them into a surface it owns and keeps.
--
-- So an application that stops answering changes nothing. Its window still
-- has its last contents, because those contents were never in its address
-- space, and this process never waits for it: every request is taken with a
-- non-blocking receive, and one that is not there is simply not there.
--
-- The BeOS app_server did the opposite - it handed the application a
-- pointer into its own buffer with a lock around it - and that is exactly
-- the arrangement that makes a hung application freeze a desktop.
--------------------------------------------------------------------------

local KEY_TAB    = 9
local KEY_ESC    = 27
local KEY_CTRL_C = 3
local KEY_PREFIX = 23      -- Control-W

--------------------------------------------------------------------------
-- **The four everybody's fingers already know.**
--
-- These were behind the prefix - `Control-W c` to copy - and the argument
-- for that was written down and was good at the time: *there are no
-- modifiers to escape into, so Control-C is a key an application can see,
-- and in this system it is already the key that stops one.*
--
-- Both halves of that have since stopped being true. There **is** a
-- modifier now: the Super key is read by the board and carried up as an
-- escape sequence, so the window manager has somewhere of its own to put a
-- command. And Control-C ending the desktop was never a decision so much as
-- the oldest line in this file - it sat at the top of `key`, before
-- anything else could look at the byte, so pressing copy in any window
-- closed the whole desktop.
--
-- So the ordinary keys do the ordinary thing, and what a person has to
-- learn is the *system* commands rather than the editing ones.
--
-- A table rather than a chain of comparisons because it is a lookup. There
-- is no upper case here and there does not need to be: Control plus a
-- letter is one control character whichever way the shift key is held.
--------------------------------------------------------------------------
local EDIT_KEYS = {
  [1]  = "selectall",     -- Control-A
  [3]  = "copy",          -- Control-C
  [24] = "cut",           -- Control-X
  [22] = "paste",         -- Control-V
}

local theme = use("/Kosmos/Libraries/theme.lua")


--
-- **A window's outside, in one table.**
--
-- `corner` is how far its corners are rounded and `shadow` how far its soft
-- edge reaches; both are points and both are rescaled with everything else.
--
--
-- **The shadow starts off and the corner starts on.**
--
-- A shadow is a band around every window, redrawn whenever anything under
-- it changes; a corner is 256 pixels once per window per damaged rectangle.
-- One of those is worth having without being asked for and the other is
-- not, which is Diego's reading after watching it under TCG - and both are
-- settings now (`handlers.theme`), so this is only where they start.
--
--
-- **`want_corner` and `want_shadow` are the settings; `corner` and `shadow`
-- are this scale's pixels.** Two fields rather than one, and the version
-- with one was wrong in a way worth recording: the shadow was turned off by
-- setting `shadow = 0` here, and the code that rescales everything then set
-- it straight back from `theme.metrics.shadow`, because that is what it does
-- to every other metric. Diego ran 0.10.141 and saw the shadow he had just
-- been told was off.
--
-- A number that is both "how big" and "whether at all" cannot survive being
-- recomputed. So the answer is kept as a flag and the pixels derived from
-- it, which is the same shape `scale` gives every other metric.
--
local OUT = { want_corner = true,
              want_shadow = false,
              corner  = theme.metrics.corner or 0,
              shadow  = 0,
              -- How far each new window steps from the last, when the
              -- quarters are gone. A title bar and a little, so the one
              -- underneath is still grabbable.
              cascade = theme.metrics.tab + 8 }

--
-- **Twenty-six, and the reasoning that gave twenty was measuring the wrong
-- thing.**
--
-- It said: the controls are fourteen and the glyphs sixteen, so this is the
-- smallest a tab can be and still hold both with a pixel either side. That
-- is an argument about what *fits*, and a title bar is not a container - it
-- is a handle. What decides its height is how hard it is to put a pointer on
-- it and keep it there while dragging.
--
-- On a 14-inch panel at 1920x1080 twenty pixels is about 2.4 mm of physical
-- target, and it reads as a hairline you have to aim at. The number that
-- matters is millimetres on the glass rather than pixels in the buffer, and
-- nothing in this file had ever asked that question - it was a desktop
-- measured in QEMU windows, where the panel is whatever the Mac's display
-- makes of it.
--
-- Six more, because that is what the machine says is comfortable. The
-- controls stay fourteen: a bigger handle, not bigger buttons.
--
-- **Read from the fixed layout** (`theme.metrics.tab`), which said 20 for
-- as long as this said 26 - the one number, kept in one place.
--
OUT.TAB_H =  theme.metrics.tab
--
-- **The frame down the sides and along the bottom**, in the title bar's
-- colour: 4, where it was 2. Diego, 24 September, looking at a Log View
-- whose console ran to the window's edge: "We need to add some extra
-- chrome to the other borders of the apps as now it looks weird and make
-- better rounded borders". Two pixels was a line rather than chrome, so a
-- window whose page runs edge to edge - a terminal, a log, a photograph -
-- had a title bar on top and nothing holding the other three sides; and at
-- the bottom the corner's arc cut straight through the page while the line
-- ran square past it. A frame carries the bar's colour round the window,
-- and the page is rounded inside it (`OUT.round_inside`), so the frame
-- follows the curve.
--
-- **Four, not the six it was for an afternoon**: "the chrome arround the
-- window is too thick, we should take a couple of pixels out". Six read as
-- a picture frame; four holds the page without being looked at.
--
OUT.BORDER     = 4
--
-- The three controls on a tab, and the room they take.
--
-- `BOX` is the control and `BOX_W` is the *slot*, so there is a gap between
-- two adjacent controls without either of them knowing about the other.
--
-- There was a `CLOSE_W` beside it, a wider slot for a close box that sat
-- alone at the left. All three are at the right now and share one slot
-- (`boxes_x`), so it went rather than staying as a number nothing reads.
--
-- **Eighteen, and it was fourteen until the tab grew.**
--
-- Fourteen was chosen against a twenty-pixel tab, where it filled most of
-- the height and looked deliberate. Twenty-six left the same small square
-- floating in a tall bar - and a control that got *harder* to hit as its
-- handle got easier is the wrong trade in both directions at once.
--
-- Two pixels on every side. The slots and the glyphs are derived from this
-- and follow, and so does the hit test, which is the whole reason it is one
-- number: a target that is drawn larger than it can be clicked is worse
-- than a small one, because it is a small one that lies.
--
-- `GRIP` below says the same thing about the resize corner - "a target that
-- thin is a target you miss" - and this is that sentence applied to the end
-- of the bar it shares.
--
--
-- **20 since 8 October** (Diego: "the close, maximise, minimize window
-- buttons are too small. can you make them 15% larger in diameter?"): the
-- disc is the box less 4, so 14 across became 16, and the run of three 62
-- wide became 68.
--
OUT.BOX        = 20

-- How far the controls sit from the end of the tab, and the title from its
-- start.
--
-- **Two numbers now, and both the drawings'** (`roadmap.md` 5zp). It was one,
-- 4, for both ends, which was fine while a window was square. Diego, 24
-- September, with a photograph of a tab: "The window title needs more left
-- margin as it's too close to the edge and looks bad" - four pixels from a
-- corner rounded at twelve put the first letter inside the curve.
--
-- The title is 18 in, which is where the drawings' header puts its own
-- title (`ui.layout.head_in`) - so a window's name on its tab and the
-- subject in the header under it start at the same x, and read as one
-- column rather than two near-misses. The controls are 10 in, the header's
-- `head_edge`, for the same reason at the other end.
--
OUT.MARGIN     = 10
OUT.TITLE_IN   = 18

-- How far each window steps down and across from the one already in its
-- corner. A tab's height, so the one underneath always has a strip of its
-- own title bar showing - which is the whole point: a window you can see a
-- piece of is a window you can pick up.



--
-- The sizing grip, bottom right, and how far into the window it reaches.
--
-- The border is four points, which is a fine thing to look at and an
-- impossible thing to hit: a target that thin is a target you miss. So the
-- grip claims a square of the window's own bottom-right corner, which is
-- what every desktop that has ever had one does, and for the same reason.
--
-- The cost is real and worth saying out loud: a click in that corner is the
-- window manager's and never reaches the application. Sixteen pixels square
-- in the one corner least likely to hold anything you meant to press.
--
OUT.GRIP       = 16
OUT.BOX_W      = OUT.BOX + 4        -- minimise and maximise, at the right
OUT.RUN        = OUT.BOX_W * 2 + OUT.BOX    -- the three, end to end

-- Nothing may be resized smaller than `scale.MIN_W` by `scale.MIN_H`, 120 by
-- 60 at 100 per cent. Below it a window is all decoration and no window.
-- They live in `scale`, below, since it rewrites them.

--
-- **The scale** (`ui.md` 16.18, `roadmap.md` 5z): how many of the
-- screen's pixels a point is, as a percentage - 100, or one of the steps
-- above it that Appearance offers. Diego, 22 September: "a factor
-- multiplier of all the things in the UI".
--
-- Applications give every size in points and do not change. This process
-- works in the screen's pixels, as it always has, and converts at its edge
-- with each window - a window opening, its drawing commands, its events -
-- by that window's own factor, `win.pct`: the scale for every window but a
-- full-screen one, which asked for the screen in the screen's own pixels.
-- The chrome - the title bar, its boxes, the border and the grip - is this
-- process's own, so its sizes are rewritten here, in pixels at the scale.
--
local scale = { pct = 100, STEPS = { 100, 110, 120, 135, 150, 175, 200 },
                MIN_W = 120, MIN_H = 60 }

-- Points to pixels, to the nearest.
function scale.px(v, pct)
  return math.floor(v * (pct or scale.pct) / 100 + 0.5)
end

-- Pixels to points, to the nearest - so a size taken to pixels and back is
-- the size it was, at every step, and a window changed from one scale to
-- another and back does not come home a point smaller.
function scale.pt(v, pct)
  return math.floor(v * 100 / (pct or scale.pct) + 0.5)
end

function scale.valid(pct)
  for _, step in ipairs(scale.STEPS) do
    if step == pct then return true end
  end

  return false
end

function scale.chrome()
  OUT.TAB_H   = scale.px(theme.metrics.tab)
  OUT.corner = OUT.want_corner and scale.px(theme.metrics.corner or 0) or 0
  OUT.shadow = OUT.want_shadow and scale.px(theme.metrics.shadow or 0) or 0
  OUT.BORDER  = scale.px(4)
  OUT.BOX     = scale.px(20)
  OUT.MARGIN  = scale.px(10)
  OUT.TITLE_IN = scale.px(18)
  OUT.cascade = OUT.TAB_H + scale.px(8)
  OUT.GRIP    = scale.px(16)
  OUT.BOX_W   = OUT.BOX + scale.px(4)
  OUT.RUN     = OUT.BOX_W * 2 + OUT.BOX
  scale.MIN_W = scale.px(120)
  scale.MIN_H = scale.px(60)
end

--
-- An event on its way to a window, in that window's points: a pointer's
-- place, a window's new place or size. Every event goes through `post`,
-- so this is the one door. Fields that are not positions - a key, a
-- button, a title - pass untouched.
--
function scale.event(event, pct)
  for _, k in ipairs({ "x", "y", "w", "h" }) do
    if math.type(event[k]) == "integer" then
      event[k] = scale.pt(event[k], pct)
    end
  end
end

-- The decoration reads the palette at the moment it draws rather than
-- copying it into constants here, which is what lets the theme change
-- without restarting anything. See `theme.lua`: the palette table is
-- mutated in place and never replaced, so these functions see the change.
local function desktop_colour()  return theme.desktop end
local function focused_colour()  return theme.tab end
local function idle_colour()     return theme.tab_idle end
local function title_colour()    return theme.tab_text end
local function stamp_colour()    return theme.stamp end

--------------------------------------------------------------------------
-- What was chosen last time.
--
-- Read once at startup from the disk, if there is one. A machine with no
-- filesystem gets the default palette and says nothing about it - the
-- appearance of the desktop is not a reason to fail to start one.
--------------------------------------------------------------------------
local prefs = use("/Kosmos/Libraries/prefs.lua")

--
-- **The title's shape: a bar across the whole window, and not a setting.**
--
-- It was a BeOS tab, as wide as what is on it, from 18 September - Diego:
-- "i love the tabs in the windows like BEOS instead of the full windoe tab
-- like we have today" - with the bar across as a choice in Appearance. The
-- choice left with the looks (`roadmap.md` 5y) and the tab stayed, until
-- Diego, 22 September: "i want to switch back the tabs from be os style to
-- full width". So every window's title bar is as wide as the window, and a
-- `tabs` an older `/Home/Preferences/appearance` saved is read by nothing: a machine
-- whose file still says "beos" would have kept the tab under a new default
-- alone. The tab's width was worked out from the close box, the title and
-- the two boxes; 0.10.113 has it.
--
-- `tabs.shape` is still the window as a title bar on a body, for the three
-- things that need its shape - the compositor cutting it out of what is
-- behind, the pointer finding what is under it, and the paint staying
-- inside it - and the two now make the frame's rectangle.
--
local tabs = {}

function tabs.width(win)
  return win.w + OUT.BORDER * 2
end

-- The tab and the body, as rectangles, for a decorated window.
-- A tabbed window's shape, its tab and its body, as eight numbers into
-- `out` - the compositor's own array, refilled every pass rather than two
-- tables made (`compose.lua`) - and how many parts that is.
function tabs.shape(win, out)
  local fx, fy = win.x - OUT.BORDER, win.y - OUT.TAB_H
  local fw = win.w + OUT.BORDER * 2

  out[1], out[2], out[3], out[4] = fx, fy, tabs.width(win), OUT.TAB_H
  out[5], out[6], out[7], out[8] = fx, win.y, fw, win.h + OUT.BORDER
  return 2
end

-- What `load_appearance` found, for the startup below to apply.
local saved_wallpaper = nil
local saved_fit = nil

-- Three faces, not one: a titlebar, a paragraph and a terminal want
-- different things, and the terminal's has to be fixed-width whatever the
-- other two are. `theme.fonts` is what was asked for, which is not always
-- what is loaded - a face that will not parse leaves the previous one in
-- place, and saying so is the caller's job.
--
-- **A role's font at a size an application asked for.**
--
-- Seven slots beyond the five roles, and `gfx.face` says no rather than
-- evicting one - so a window that asks for more sizes than the machine will
-- hold draws those at the role's own size, said once, instead of silently
-- losing its headings to the bitmap font.
--
local sized_faces, sized_full = {}, false

local function sized(role, px, variant)
  role = role or "ui"

  local want = theme.fonts[role]

  if not want then return role end

  local asked = px or want.px

  -- `px` is in points, like the role's; both are drawn at the scale.
  px = scale.px(asked)

  if not variant and px == scale.px(want.px) then return role end

  -- A weight or a slant of the role's font (`ui.sized`): its own file, or
  -- the role's size when the look's font has none.
  local key = role .. (variant and (":" .. variant) or "") .. "@" .. px
  local got = sized_faces[key]

  if got == nil then
    local face, why = gfx.face(want.font .. (variant and ("-" .. variant) or ""), px)

    if not face and variant then
      sized_faces[key] = sized(role, asked)
      return sized_faces[key]
    end

    got = face or false
    sized_faces[key] = got

    -- The reason `gfx` gave, which is not always room: a bitmap face has
    -- no other sizes to make.
    if not got and not sized_full then
      sized_full = true
      print("wm: " .. tostring(why) .. ", so " .. key
            .. " draws at the role's size")
    end
  end

  return got or role
end

--
-- What each role actually holds. Nothing means the bitmap, which is what a
-- role that has never been loaded draws in.
--
local in_force = {}

local function apply_fonts(fonts)
  if type(fonts) ~= "table" then return end

  local why

  for _, role in ipairs(theme.roles) do
    local want = fonts[role]

    if type(want) == "table" and want.font then
      local px = tonumber(want.px) or 16
      -- Loaded at the scale; what is recorded and sent is the size in
      -- points, which is what every application lays itself out in.
      local ok, err = gfx.use_font(want.font, scale.px(px), role)

      if ok then
        in_force[role] = { font = want.font, px = px }
      else
        why = tostring(err)
      end

      --
      -- **What is advertised is what is loaded**, and it was what was asked
      -- for. Applications are sent this table and lay themselves out against
      -- it, while the text of an ordinary window is drawn here - so a role
      -- this process failed to load and still named is a desktop measuring
      -- in one face and drawing in another.
      --
      -- A failure leaves the previous face in force rather than none, which
      -- is why this says what is there instead of assuming the bitmap.
      --
      theme.fonts[role] = in_force[role] or { font = "spleen", px = px }
    end
  end

  -- Sized faces were cut from the fonts that have just been replaced.
  -- And the slots behind them: the cache is the only holder of a sized
  -- face, so clearing it without giving them back would leave them taken.
  sized_faces, sized_full = {}, false
  gfx.release_faces()

  return why
end

--
-- **What a machine nobody has told looks like, and it is Plex.**
--
-- The palette used to be whatever the kit was compiled with, which is a
-- default by omission rather than by choice; then it was BeOS, the bet this
-- system brings forward. Since 22 September it is **Plex**, the first of the
-- four looks (`roadmap.md` 5y, `docs/looks.html`), whose faces are the
-- kit's own defaults - so a first boot is a finished look, not a palette
-- over faces chosen elsewhere. BeOS is still one look away, as Classic.
--
-- Only when nothing was saved: a person who has chosen keeps their choice,
-- and on a machine with no disk `/Home` does not survive a power cut, so
-- this is what they see again next time.
--
--
-- What a machine that has chosen nothing wears: the first look `themes.lua`
-- offers. It named Plex here, which made the default two facts in two files
-- - this, and the order the looks are offered in - and the day the default
-- became Endeavour (`roadmap.md` 5zq) one of them would have stayed behind.
--
local function default_appearance()
  local ok, shipped = pcall(use, "/Kosmos/Libraries/themes.lua")

  if not ok or type(shipped) ~= "table" or type(shipped.order) ~= "table" then
    return
  end

  local first = shipped[shipped.order[1] or ""]

  if not first then return end

  local palette = theme.read(first, "dark")

  if palette then theme.apply(palette) end
end

local startup_font_why

local function load_appearance()
  local saved = prefs.read("appearance")

  --
  -- **Whether a window is rounded and whether it casts a shadow, before
  -- the scale** - because `scale.chrome` below derives the pixels from
  -- these flags, and reading them after would give the first frame the
  -- defaults and every frame after them the setting.
  --
  -- Absent means the default, which is rounded and no shadow. A file
  -- written before these existed therefore reads exactly as a fresh
  -- machine does, which is the behaviour every other key here has.
  --
  if saved.corner ~= nil then OUT.want_corner = saved.corner and true end
  if saved.shadow ~= nil then OUT.want_shadow = saved.shadow and true end

  -- Focus following the pointer (`wm/pointer.lua`), and its delay.
  OUT.focus_follows = saved.focus_follows == true

  if math.type(saved.focus_delay_ms) == "integer" then
    OUT.focus_delay_ms = math.max(250, math.min(3000, saved.focus_delay_ms))
  end

  -- The scale next: the chrome's sizes and every face below follow it.
  if math.type(saved.scale) == "integer" and scale.valid(saved.scale) then
    scale.pct = saved.scale
  end

  scale.chrome()

  if scale.pct ~= 100 then print("wm: scale " .. scale.pct) end

  if not saved.palette then default_appearance() end

  --
  -- **A saved theme, found where Appearance finds themes.**
  --
  -- This was `theme.apply(saved.palette)`, and the name reached a table
  -- holding only the two palettes compiled into `theme.lua`. So Photon,
  -- BeOS, Platinum, IRIX and Plex - everything `themes.lua` ships and
  -- every `.theme` file on the disk - were written to `/Home/Preferences/appearance`
  -- faithfully, and at the next start came back as `dark`, with the
  -- faces restored beside them because those are saved spelled out.
  -- Nothing said so, because the answer `theme.apply` gives was thrown
  -- away. Diego, 21 September: the Appearance panel "does not rememver the
  -- wallpapers and other things upon restarting"; the wallpaper was 18.133,
  -- and this was the other thing (`testing.md` 18.135).
  --
  local function find_theme(name)
    if type(name) ~= "string" then return nil, "no name" end
    if theme.palettes[name] then return theme.palettes[name] end

    local ok, shipped = pcall(use, "/Kosmos/Libraries/themes.lua")

    if ok and type(shipped) == "table" and type(shipped[name]) == "string" then
      return theme.install(name, (theme.read(shipped[name], "dark")))
    end

    -- A look somebody added is theirs, so it is in `/Home/Themes`; `/system`
    -- held them until it went (`roadmap.md` 6s c3).
    for _, file in ipairs(fs.list("/Home/Themes") or {}) do
      if file:match("%.theme$") then
        local p = theme.load("/Home/Themes/" .. file, "dark")

        if p and (p.name or file:gsub("%.theme$", "")) == name then
          return theme.install(name, p)
        end
      end
    end

    return nil, "no theme called " .. name .. " in /Kosmos/Themes or "
                .. "/Home/Themes"
  end

  if saved.palette then
    local palette, why = find_theme(saved.palette)

    if palette then
      theme.apply(palette)

      -- Its corner and shadow, which `scale.chrome` above read before the
      -- look was known (Night's are rounder).
      OUT.corner = OUT.want_corner and scale.px(theme.metrics.corner or 0) or 0
      OUT.shadow = OUT.want_shadow and scale.px(theme.metrics.shadow or 0) or 0
      print("wm: theme " .. tostring(saved.palette))
    else
      print("wm: theme " .. tostring(saved.palette) .. " would not load: "
            .. tostring(why))
    end
  end
  if saved.desktop then theme.override { desktop = saved.desktop } end

  --
  -- **`or theme.fonts`, and without it the desktop was two fonts at once.**
  --
  -- This applied `saved.fonts` alone, and returned above without applying
  -- anything at all when there was no settings file - so the window manager
  -- loaded a face only if somebody had been to the Appearance panel. Nothing
  -- showed it for as long as the default was the bitmap, because a face that
  -- is not loaded *is* the bitmap.
  --
  -- The day the default became IBM Plex it showed everywhere at once. The
  -- defaults still reached applications, because what is sent below is
  -- `theme.fonts` rather than what was loaded here, so every application
  -- loaded Plex and laid itself out in it - while this process, which draws
  -- the text of every ordinary window, still had the 8x16 bitmap. Menu
  -- titles were spaced for 18 pixels of "File" and drawn 32 wide, so "Go"
  -- started inside it; buttons sized in Plex held bitmap text and clipped it.
  --
  startup_font_why = apply_fonts(saved.fonts or theme.fonts)

  -- Kept, not applied: this runs before the framebuffer is taken, and a
  -- picture cannot be centred until something knows how big the screen is.
  saved_wallpaper = saved.wallpaper
  saved_fit = saved.wallpaper_fit
end

local screen = gfx.screen()
--
-- The wallpaper, if there is one.
--
-- A surface and where its top-left corner sits, worked out once when it is
-- loaded rather than per rectangle: the desktop is composed in pieces and
-- the arithmetic would otherwise be redone for every one of them.
--
-- **Filling the screen, or centred** - Appearance's *Wallpaper size*
-- (Diego, 3 October, at the M700's 1720 by 1440: "the wallpaper needs to be
-- either stretched or expanded to fill the screen", "either center or
-- fill"). This said *centred, never stretched*: scaling needed a resampler
-- and would blur a picture already the screen's size. Both halves still
-- hold, and neither stops a fill: the gfx kit's `stretch` resamples, in C
-- and smoothed, **once**, when the picture or the choice changes - into a
-- surface the screen's size that a pass blits as it blitted the picture -
-- and a picture already the screen's size is never resampled at all.
--
-- **Fill keeps the picture's shape**: as large as covers the screen, its
-- middle kept and the rest cut away, rather than stretched out of
-- proportion. Centred, a smaller picture sits in the middle on the desktop
-- colour and a larger one is cropped to its middle, since `blit` clips.
--
local wallpaper, wall_x, wall_y, wall_w, wall_h = nil, 0, 0, 0, 0
local wall_fit = "fill"           -- or "centre"
local wall_filled = nil           -- the picture made the screen's size

--
-- Read a picture and make it the desktop.
--
-- PNG or JPEG, chosen by the name on the end of the file: `DECODERS` below
-- says which function decodes which. Anything else is refused with a
-- sentence, which is better than a file that silently does nothing when you
-- pick it.
--
--
-- A picture off the filesystem, decoded here.
--
-- Into a region, not a Lua string. A screen-sized photograph is a megabyte
-- or three of PNG and this heap is two, by design. `fs.read` would build
-- that as a string out of chunks that are themselves on the heap, and the
-- answer was "cannot read" on a file that was plainly there. `fs.read_into`
-- puts it in pages instead, and the decoder is given where it landed.
--
-- Written for the wallpaper and now shared with `ui.image`, which is what
-- lets Photo open a file rather than only the pictures compiled into this
-- image. Same reason it lives here rather than in the application: this
-- process owns every pixel on the screen, and an application that decoded
-- its own would be holding them.
--
--
-- Which decoder, from the name on the end of the file.
--
-- The extension and not the bytes, deliberately. Both formats have a
-- signature and sniffing it would work, but a picture that is called `.png`
-- and is not one is a mistake worth reporting rather than quietly coping
-- with - and every picture this system opens is one somebody put there on
-- purpose.
--
-- JPEG is here for wallpapers, which is the one thing on this machine that
-- is a photograph. A 1920x1080 photograph is about three megabytes as a PNG
-- and three hundred kilobytes as a JPEG, and the disk is 32 MB: the format
-- is the difference between nine wallpapers and ninety.
--
local DECODERS = {
  png  = function(...) return gfx.png(...) end,
  jpg  = function(...) return gfx.jpeg(...) end,
  jpeg = function(...) return gfx.jpeg(...) end,
}

--
-- One chooser, used by both things that open a picture: a file on the disk
-- and a file compiled into the image. They had drifted apart already - the
-- asset path said `gfx.png` outright - and that was harmless only for as
-- long as every asset was a PNG.
--
local function decoder_for(name)
  local suffix = tostring(name):lower():match("%.([%a]+)$")

  return suffix and DECODERS[suffix], suffix
end

--
-- **A picture by name, from wherever the name says**: a path beginning `/`
-- is a file, read into pages as above; any other name is carried in the
-- image and handed over by `sys.asset` - the icons, the test pictures, and
-- in a `FULL=1` image the desktop's own wallpapers, `wallpaper/<file>`. One
-- function for both, because the two had drifted once already.
--
-- The pages are `regions.lua`'s, and so is the read: one of the whole file,
-- through a string where the filesystem hands over no pages. It was a loop
-- asking again for what was left, which `fs.read_into` would have written
-- over the start of the region rather than after what came first.
--
local regions = use("/Kosmos/Libraries/regions.lua")

local function picture_from(path)
  local decode, suffix = decoder_for(path)

  if not decode then
    return nil, "this opens PNG and JPEG, and that is neither"
  end

  if path:sub(1, 1) ~= "/" then
    local bytes, why = sys.asset(path)

    if not bytes then return nil, why end

    local ok, made = pcall(decode, bytes)

    if not ok or not made then
      return nil, tostring(made or ("that " .. suffix .. " would not decode"))
    end

    return made
  end

  local size = (fs.getattr(path) or {}).size

  if not size or size == 0 then
    return nil, "cannot read " .. tostring(path)
  end

  local region = regions.make(size)

  if not region then
    return nil, "no memory for a picture that size"
  end

  local done = regions.read_file(path, region, size) or 0

  if done ~= size then
    regions.free(region)
    return nil, "could only read " .. done .. " of " .. size .. " bytes"
  end

  local ok, made = pcall(decode, region.at, size)

  -- The compressed copy is scratch: the surface holds the pixels now.
  regions.free(region)

  if not ok or not made then
    -- `made` is the decoder's own sentence when the call raised, which is
    -- the one that says *what* was wrong with the file.
    return nil, tostring(made or ("that " .. suffix .. " would not decode"))
  end

  return made
end

local function wallpaper_load(path)
  if path == nil or path == "" then
    if wallpaper then wallpaper:free() end
    wallpaper = nil
    return true
  end

  local made, why = picture_from(path)

  if not made then return nil, why end

  -- The old one goes back only once the new one exists: a decode that fails
  -- should leave the desktop it had rather than clearing it.
  if wallpaper then wallpaper:free() end

  wallpaper = made
  return true
end

local function wallpaper_place()
  if wall_filled then
    wall_filled:free()
    wall_filled = nil
  end

  if not wallpaper then return end

  local sw, sh = screen:size()
  local pw, ph = wallpaper:size()

  if wall_fit == "fill" and (pw ~= sw or ph ~= sh) then
    -- The part of the picture with the screen's shape, from its middle.
    local cw, ch = pw, ph

    if pw * sh > ph * sw then
      cw = ph * sw // sh
    else
      ch = pw * sh // sw
    end

    -- On the desktop colour, for a picture with transparent parts.
    wall_filled = gfx.surface{ w = sw, h = sh }
    wall_filled:fill(0, 0, sw, sh, desktop_colour())
    wall_filled:stretch(wallpaper, (pw - cw) // 2, (ph - ch) // 2, cw, ch,
                0, 0, sw, sh, nil, true)
    wall_x, wall_y, wall_w, wall_h = 0, 0, sw, sh
    print(("wm: the wallpaper, %dx%d, fills the screen from %dx%d of it")
          :format(pw, ph, cw, ch))
    return
  end

  wall_w, wall_h = pw, ph
  wall_x = (sw - wall_w) // 2
  wall_y = (sh - wall_h) // 2
end

if not screen then
  print("wm: this process was not given the screen")
  return
end

local W, H = screen:size()

-- The backbuffer. Everything is composed here and only rectangles that
-- changed are copied out - `ui.md` 16.7. Under QEMU the framebuffer is
-- ordinary cached RAM and a full copy is not expensive, so this buys
-- nothing in speed here and is not pretending to: what it buys is that a
-- frame is never seen half-composed, and it is the shape the hardware
-- needs. On a real board the scanout buffer is often uncached, where
-- drawing into it directly is the mistake that costs 10-50x.
local back = gfx.surface{ w = W, h = H }

--
-- The screen is this process's now.
--
-- Until this call the kernel console is also drawing here, and the two
-- cannot share: printing one line scrolls the whole display, which drags
-- every window up sixteen pixels and leaves a copy of its tab behind. It
-- reads as a compositor bug and is not - the console's scroll moves every
-- pixel, because as far as it knows every pixel is text.
--
-- Nothing stops being reported. The console keeps writing to the serial
-- line, which is where a system with a window manager on it is debugged
-- from, and a panic takes the screen back regardless.
--
sys.screen_take(true)

--
-- **What the desktop is doing, in the kernel's log, whether or not the
-- desktop can draw.**
--
-- The first machine this ran on had no serial port, and every instrument
-- built for it needed a window - which meant asking the window manager to
-- report on the window manager, and getting silence exactly when there was
-- something to say. `print` here does not need a window: it reaches the
-- console server and so the kernel's ring, which `log` prints at the prompt
-- after Control-C has ended the desktop. The one channel that kept working
-- on that laptop was the shell, so this is the channel the narration uses.
--
-- **Bounded rather than switched on, because a flag has to be remembered by
-- somebody already having a bad evening.** `args` is the list of programs
-- to start, so `wm trace` would try to run `/Kosmos/Programs/trace.lua`; and a machine
-- that hangs on the third pass is one nobody gets to type a flag into
-- twice. Forty passes is about a third of a second of a healthy desktop and
-- some eight kilobytes of the sixty-four in the ring - it costs a blink at
-- startup and nothing at all afterwards, and if the loop stops early the
-- last line printed is the stage it stopped in.
--
--
-- **Off unless asked for, and that is not caution - it is a measurement.**
--
-- With this on unconditionally the display harness failed at its last
-- phase: the compositor was six passes into its life several seconds after
-- starting, and the trace said why. `hello-win` polls with `wait_ticks = 0`,
-- so it is answered immediately and asks again immediately, and every one
-- of those turns into two lines here - two calls to the console server,
-- which is two round trips the compositor spends not compositing. The
-- window being dragged did not move because the manager had not looked at
-- the pointer yet.
--
-- Which is the whole argument for making it opt-in rather than cheap: an
-- instrument that changes the thing it measures is worse than none, and a
-- desktop being investigated for slowness is exactly where that matters.
--
--     wm trace
--
-- `trace` is a reserved word in the program list rather than a separate
-- argument, because `args` here *is* the list and there is no room for a
-- flag beside it. It is taken out of the list before anything is started,
-- so `wm trace` opens the same desktop `wm` does.
--
local TRACE = false

-- Forward-declared: `step` below needs it and the definition is further
-- down the file, next to the rest of the clock arithmetic.
local counter_per_tick

local passes = 0

--
-- **How long each stage took, in microseconds.**
--
-- The line stamps in the kernel's log are scheduler ticks - four
-- milliseconds here - which is coarse enough that a whole pass fits inside
-- one and every stage of it reads the same number. That was enough to prove
-- the compositor was not spinning and useless for the question underneath:
-- *which stage is the time in*.
--
-- `sys.ticks()` is the counter, which on this machine runs at 2.6 GHz. The
-- divisor is worked out on first use rather than at load, because
-- `counter_per_tick` is defined further down the file.
--
local per_us

local last_step = 0

local function step(what)
  --
  -- **No pass bound, and removing it is the point.**
  --
  -- It was forty passes, from when this printed unconditionally and the
  -- display harness had to be protected from it. `trace` is opt-in now, so
  -- the harness never sets it and the bound protects nobody - while cutting
  -- the instrument off exactly where it starts being useful. On the first
  -- machine the four login windows opened at passes 39 and 40, so the whole
  -- record ended on the last line before anything interesting happened.
  --
  -- Unbounded is *correct* here because the kernel's ring is bounded: it
  -- holds 64 KB and evicts the oldest, so what survives is the last few
  -- hundred passes before you pressed Control-C. That is the state worth
  -- having, and a prefix of the boot is the state that is not.
  --
  if TRACE then
    local now = sys.ticks()

    if not per_us then
      per_us = math.max(1, counter_per_tick()
                           * ((sys.info() or {}).tick_hz or 250) // 1000000)
    end

    print(("wm: %d %s %dus"):format(passes, what,
          last_step > 0 and (now - last_step) // per_us or 0))

    last_step = now
  end
end

--
-- **The counter, in microseconds, for a trace line another will be
-- subtracted from.**
--
-- A note is printed at the start of the *next* pass, so the moment it is
-- printed is up to a pass late and says nothing exact. The moment it is
-- written is exact, and stamping it then is what lets two notes be compared:
-- `focus` and the `draw` that follows it are how long the Deskbar took to
-- show a change, measured on the one clock both happened on.
--
local function trace_us()
  if not per_us then
    per_us = math.max(1, counter_per_tick()
                         * ((sys.info() or {}).tick_hz or 250) // 1000000)
  end

  return sys.ticks() // per_us
end

--
-- **A poll and its answer, on the record for as long as `trace` is on.**
--
-- An application asks to be woken in a quarter of a second to a second, so
-- the two halves of a conversation are many passes apart. When this was
-- bounded at forty passes it caught five polls and one answer, which said
-- nothing at all about whether the answers were arriving. So it has no
-- bound, for the reason `step` has none: the kernel's ring is.
--
--
-- **Recorded here, printed at the top of the next pass, and the difference
-- is not tidiness.**
--
-- `print` is a call to the console server: the compositor blocks until
-- another process answers. `step` does that from the loop body and is
-- harmless - measured, the display harness passes with it. Doing the same
-- thing from *inside* `answer_waiting` and the message drain is not: with
-- these lines printed where they are generated, dragging a hung
-- application's window stopped working entirely - the window did not move
-- at all - and putting them back in a queue fixed it with nothing else
-- changed. Twice, deterministically, at the same coordinates.
--
-- Which is a small demonstration of the rule this system already holds
-- about servers on a deadline: the cost of a round trip is not the
-- microseconds it takes, it is where in the loop you spend them.
--
local queued = {}

--
-- **A reply that failed to go, said out loud.**
--
-- Every `sys.reply` in this file is wrapped in `pcall`, which is right -
-- an application that died between asking and being answered must not take
-- the compositor with it - and the result was thrown away, which is not.
--
-- On the first real machine every application blocked in its first poll for
-- as long as the desktop was up and the log said nothing at all, because
-- the one thing that could have gone wrong was the one thing nobody was
-- allowed to hear about. A dropped reply is a hung application by
-- construction: the caller is in `sys.call` and there is nothing else that
-- will ever wake it.
--
-- Bounded, because if it happens once it will happen every pass and the
-- ring would hold nothing else.
--
local complaints = 0

local function replied(ok, err, what)
  if not ok and complaints < 20 then
    complaints = complaints + 1
    print(("wm: reply for %s failed: %s"):format(tostring(what),
          tostring(err)))
  end

  return ok
end

--
-- **Only while `trace` is on, and the reason is a measurement.**
--
-- These lines once printed on every desktop, sixty of them, and that kept
-- them going for several seconds - long enough to still be going during a
-- drag, and a compositor that does a round trip to another process while a
-- window is being dragged loses the drag. Measured: the display harness
-- failed at the same coordinates twice with these lines on and passed with
-- them off, and queueing them changed nothing, which is what says the cost
-- is *when* they happen rather than where.
--
-- So a note is opt-in, as `step` is, and a caller formats its line under
-- `TRACE` too: a desktop with the trace off builds no string for it.
--
local function note(what)
  if TRACE then
    queued[#queued + 1] = what
  end
end

local function flush_notes()
  for i = 1, #queued do
    print("wm: " .. queued[i])
  end

  if #queued > 0 then
    queued = {}
  end
end



-- Whatever appearance was chosen last time, before the first pixel is
-- drawn. After `screen_take` because a failure to read it must not stop the
-- desktop starting, and before compositing because otherwise the first
-- frame is the default palette and the second is the chosen one.
load_appearance()

--
-- **Said out loud, because this is the face every window's text is drawn
-- in.** An application is sent these names and lays itself out against
-- them; if this process holds something else, the desktop measures in one
-- font and draws in another, which is what happened the day the default
-- stopped being the bitmap. A line in the log is what lets a test - and a
-- person looking at a boot - see which faces are really in force.
--
do
  local said = {}

  for _, role in ipairs(theme.roles) do
    local f = theme.fonts[role]

    said[#said + 1] = role .. "=" .. f.font .. "/" .. f.px
  end

  print("wm: faces " .. table.concat(said, " "))
end

-- And the picture, now that there is a screen to centre it on. A wallpaper
-- that has gone missing is not an error worth stopping for: the desktop
-- colour is underneath it and always was.
--
-- **And it says which, or why not.**
--
-- This threw the reason away: `wallpaper_load` answers `nil, why` and the
-- `if` took only the truth of it. A wallpaper that had been chosen,
-- accepted by this process and written to `/Home/Preferences/appearance` then failed
-- to come back with the machine saying *nothing at all* - which is what
-- Diego met on the ThinkPad on 21 September: "the appearance app does not
-- rememver the wallpapers and other things upon restarting".
--
-- The fonts in that same file were being restored perfectly, which the
-- boot log proves - `wm: faces ui=ibmplexsans/14 ...` is his choice and
-- not the default - so the file, the disk and the load were all working
-- and only this one line was quiet about its failure.
--
-- A setting that does not apply and does not say so is worse than one
-- that refuses loudly, because there is nothing to search for. It is the
-- same lesson as the region error three files away, which had three
-- causes and named none of them.
--
wall_fit = saved_fit == "centre" and "centre" or "fill"

if saved_wallpaper then
  local ok, why = wallpaper_load(saved_wallpaper)

  if ok then
    wallpaper_place()
    print("wm: wallpaper " .. tostring(saved_wallpaper))
  else
    print("wm: wallpaper " .. tostring(saved_wallpaper)
          .. " would not load: " .. tostring(why))
  end
end

local ep = sys.endpoint()

if not ep then
  print("wm: no endpoint")
  return
end

--
-- **Asked for once: a request ends this process's sleep.**
--
-- Each pass sleeps in `wait_input` - which is the console server's sleep -
-- and only then collects what applications have asked. A request arriving
-- during the sleep waited for it to run out: 11.5 ms a round trip on an idle
-- desktop, and a game making two a frame could not pass 43 frames a second
-- on the ThinkPad, however little the manager had to do. The console now
-- watches this endpoint and answers early when somebody calls. If it cannot,
-- nothing breaks; requests are simply answered a sleep late, as they were.
--
do
  local watched, why = fs.watch_input("/Devices/console", ep)

  if not watched then
    print("wm: requests will wait for the next pass: " .. tostring(why))
  end
end

--
-- Publish it, so a process that was not started by this one can find it.
--
-- The window manager used to be reachable only as `/Devices/wm`, and only by
-- children it launched itself - the endpoint was handed over at spawn and
-- there was no other way to get it. That made two things wrong at once.
--
-- It was in `/Devices`, which is devices: `cpu`, `memory`, `screen`, `keyboard`,
-- read a path and get a table of facts. A window manager is not a fact and
-- is not hardware. It is a *server* - the thing every graphical application
-- talks to - and the screen is the device it draws on, reached by syscall
-- and not by path at all. Plan 9, which this system takes namespaces from,
-- names the interface and never the program: it has `/Devices/draw` and
-- `/Devices/cons`, and no `/Devices/rio`.
--
-- And nothing could discover a running desktop. `frames` and `procs` both
-- ask the window manager questions, both are run from a shell, and neither
-- was ever handed `/Devices/wm` - so both failed, reporting a protocol error
-- from the *devices* server, because `/Devices/wm` fell back to the `/Devices`
-- mount by prefix. An error that names the wrong server is worse than one
-- that says nothing.
--
-- `/Running` is exactly the registry for this: which running application
-- answers to which name. It hands over the endpoint and steps out of the
-- way rather than forwarding, so a hung desktop blocks whoever chose to
-- talk to it and nobody else.
--
-- **Children are still handed it directly**, mounted under the same name.
-- That is not redundancy: a game started from the Deskbar gets a window
-- manager and nothing else, and must not need `/Running` to draw - `/Running` would
-- let it enumerate and script every other running application. One name,
-- two ways to get it: given to you, or looked up if you are allowed to look.
--
if not fs.send("/Running", { type = "register", name = "wm" }, ep) then
  -- Not fatal. A desktop that cannot publish itself still works for
  -- everything it starts, which is how it worked before this existed.
  print("wm: could not register in /Running; only my own children can find me")
end

-- Which build this is, in the corner. `sys.build()` is compiled in by the
-- Makefile from the commit, so it identifies the source that produced the
-- image rather than the moment it was linked.
local b = sys.build()
-- ASCII only. The font is the 8x16 one from `assets/`, which has glyphs for
-- 0x20 to 0x7e and a box for everything else - so a middot separator came
-- out as a row of boxes, which is the font saying exactly what it should.
local stamp = ("%s %s  |  %s  |  %s  |  %s")
              :format(b.name, b.version, b.build, b.date, b.platform)

--------------------------------------------------------------------------
-- Windows, back to front. The last one is on top and has the focus.
--------------------------------------------------------------------------

local windows = {}
local by_handle = {}
local pending_pid = nil

-- And which program that launch was, for the same window. See where
-- both are collected, a few hundred lines down in `handlers.open`.
local pending_program = nil

--
-- **What is starting, until its first window opens** (Diego, 29 September:
-- "we do need some indicator of the app loading", and a second click that
-- starts it twice; `docs/launching.html`). A program is in here from the
-- request until a window arrives that names it, its launch fails, its
-- process ends, or `STARTING_FOR` passes - a program that opens no window
-- at all. The Deskbar is told when this changes, as it is for windows, and
-- draws a button for each; and a second request for a program in here
-- starts nothing.
--
local starting = {}
local STARTING_FOR = 20          -- seconds, for a program that never opens one
local FAILED_FOR = 3             -- seconds a failure stays on its button

-- The same, in the counter's units: `since` is `sys.ticks()`.
local COUNTER_HZ = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local STARTING_COUNTS = STARTING_FOR * COUNTER_HZ
local FAILED_COUNTS = FAILED_FOR * COUNTER_HZ

local function stop_starting(match)
  for i, s in ipairs(starting) do
    if match(s) then
      table.remove(starting, i)
      return true
    end
  end

  return false
end

-- Decoded pictures, by name. `false` means it was tried and would not
-- decode, which is remembered so a broken image is not re-decoded every
-- frame for the life of the desktop.
local image_cache = {}

--
-- The ones that came off the filesystem, in the order they were decoded.
--
-- An asset is one of a handful compiled into the image, shared by every
-- window, and keeping all of them for ever costs a few kilobytes. A *file*
-- is whatever somebody opened, a decoded photograph is four megabytes, and
-- there is no limit on how many somebody opens - so remembering them the
-- same way is how the one process the whole desktop depends on runs out of
-- memory because a person looked through a folder of holiday pictures.
--
-- So files get a queue with a lid on it, and the oldest is freed when a new
-- one arrives. Four is enough for what actually happens, which is a window
-- redrawing the picture it is showing. Insertion order rather than use
-- order, because the case where the difference matters - five picture
-- windows open at once, each evicting another's - costs a re-decode and
-- nothing else.
--
local from_file = {}
local FILE_PICTURES = 4

--
-- A decoded picture into the cache, under that queue.
--
-- Factored out of `picture_named` when pictures started arriving from
-- applications as well: two queues would be two answers to "how many decoded
-- photographs may the one process the desktop depends on hold at once".
--
local function remember_picture(name, picture)
  image_cache[name] = picture
  from_file[#from_file + 1] = name

  while #from_file > FILE_PICTURES do
    local old = table.remove(from_file, 1)
    local gone = image_cache[old]

    image_cache[old] = nil

    if gone and gone ~= picture then gone:free() end
  end
end

--
-- A picture by name, decoded once.
--
-- **A leading slash means a file; anything else is an asset.** That one
-- distinction is the whole of it, and it is what lets Photo open something
-- off the disk with no change to `ui.image` and no new message: the widget
-- has always named a picture and let this process find it, and where this
-- process looks was never the application's business.
--
-- `nil` for a picture that is not there or will not decode, and the
-- negative answer is cached too - `false` in the table - so a name that is
-- wrong is not re-read from the disk on every repaint.
--
-- `local ok, decoded = bytes and pcall(...)` is what the asset branch was,
-- and it is wrong in a way Lua does not warn about: `and` truncates a
-- multi-value expression to one value, so `ok` got pcall's first return and
-- `decoded` got nothing. The picture decoded perfectly and was thrown away,
-- and the window said there was no such picture.
--
local function picture_named(name)
  local picture = image_cache[name]

  if picture ~= nil then return picture or nil end

  picture = false

  local began = sys.ticks()
  local made = picture_from(name)

  if made then
    picture = made

    --
    -- **A large one is said, with what it cost** - a wallpaper, a
    -- photograph - and an icon is not, or a boot would print forty. Once a
    -- picture, since it is cached: what a decode takes on the machine it
    -- ran on, in the log, and the line the display harness waits for rather
    -- than a sleep sized for the slowest decode anybody has seen.
    --
    local w_, h_ = made:size()

    if w_ * h_ >= 512 * 512 then
      print(("wm: decoded %s, %dx%d, in %d ms"):format(
        name, w_, h_, (sys.ticks() - began) * 1000 // COUNTER_HZ))
    end

    -- Only a file goes on `remember_picture`'s list, which keeps a bounded
    -- number of them and frees the oldest; the image's own pictures are
    -- kept in the cache as they always were.
    if name:sub(1, 1) == "/" then remember_picture(name, made) end
  end

  image_cache[name] = picture

  return picture or nil
end

--------------------------------------------------------------------------
-- Applications that are waiting for something to happen.
--
-- `poll` used to answer immediately, always, with an empty list when there
-- was nothing - so every application span: ask, get nothing, yield, ask
-- again, for ever. A desktop with four windows open had five threads that
-- were permanently runnable and a processor meter that read ninety-six per
-- cent with nothing happening. The meter was right.
--
-- Now the reply is parked. An application asks and is not answered until
-- there is an event for it or its own deadline arrives, so it is blocked
-- rather than running - not scheduled, not costing anything. It is the same
-- mechanism the filesystem uses for a live query, and for the same reason:
-- waiting is not the same as asking repeatedly.
--
-- This process blocks too: each pass sleeps in `fs.wait_input` until a key
-- or the pointer arrives or the soonest deadline is due, so a desktop with
-- nothing happening has nothing runnable at all.
--------------------------------------------------------------------------

local DEFER = { "deferred" }
local waiting = {}

--
-- **A `wait` on the wire is in scheduler ticks; a deadline here is not.**
--
-- `sys.ticks()` is the counter - a timestamp, tens of megahertz - and
-- every *timeout* in this system is in scheduler ticks: `sys.sleep`,
-- `sys.receive`, `fs.wait_input`. `poll`'s `wait` is a timeout, so it is
-- scheduler ticks too, and every caller in the tree writes it that way:
-- `cube3d` and `plasma` send 1 and call it a tick, `music` sets
-- `poll_wait = 1` and comments it "4 ms, one scheduler tick".
--
-- It was added to `sys.ticks()` raw. At 62.5 MHz against 250 Hz that is a
-- factor of a quarter of a million, so `wait = 1` asked for sixteen
-- nanoseconds: every animating window's deadline was already past, and the
-- window manager answered them all on whatever pass came next. **The pass
-- rate was the frame rate.**
--
-- Which is why the cube ran faster while the mouse was moving. A pointer
-- event cuts `wait_input` short, so passes came more often, so windows were
-- answered sooner. Input was acting as a clock for everything that
-- animates - and for `music`, which hands over a period when its poll
-- returns and calls a feed later than 23 ms a hole in the sound.
--
-- Memoised because neither rate changes and this is on the poll path.
--
local per_tick

function counter_per_tick()
  if not per_tick then
    local rate = (sys.info() or {}).tick_hz or 100

    per_tick = math.max(1, COUNTER_HZ // rate)
  end

  return per_tick
end

local function in_counter(ticks)
  return ticks * counter_per_tick()
end

-- How long a poll waits when the caller does not say: not at all. It is
-- answered on the next pass with whatever there is, which is what
-- `wmproto.poll` sends when it is given no wait - a window that wants to
-- block says for how long.
local POLL_DEFAULT = 0
local next_handle = 1
local damage = {}
OUT.running = true

local function add_damage(x, y, w, h)
  if w <= 0 or h <= 0 then return end
  damage[#damage + 1] = { x = x, y = y, w = w, h = h }
end

-- Everything a window occupies on screen, decoration included. Used for
-- damage, so it has to be the outside of the outermost thing drawn.
--
-- Menus, above everything, in a list of their own.
--
-- `roadmap.md` M13 item 2 and the design that won: **a menu is a window.**
-- Same record, same `by_handle`, same `draw`, same `close`, same
-- `collect_closing` - so a menu inherits every death path the desktop
-- already survives rather than inventing one. That is the whole argument
-- for it: the alternative designs each grew a modal input grab held by the
-- window manager on behalf of an application, which is the one new way to
-- wedge a desktop whose entire premise is that it cannot be wedged.
--
-- What is *not* shared is the stacking. `windows` is walked in a dozen
-- places, two of which read `i == #windows` as "focused", and threading a
-- kind flag through all of them to keep menus out of the focus order would
-- be twelve chances to get it wrong. A second list, composited after the
-- first and hit-tested before it, leaves every one of those walks exactly
-- as correct as it was.
--
local menus = {}

--
-- A menu has no decoration, so its frame is its rectangle. Everything that
-- damages, hit-tests or composites goes through here, which is why this is
-- the only place that has to know.
--
local function frame_of(win)
  -- A menu and the backdrop are both undecorated, at opposite ends of the
  -- stack: one floats over everything, the other is what everything sits
  -- on. Neither has a tab, so for both the frame is the rectangle.
  --
  -- **And a window whose header is its title bar** (`win.headed`,
  -- `roadmap.md` 6zj): no tab and no border, so its frame is its page -
  -- rounded, and with its shadow, which the others here do not have.
  if win.kind == "menu" or win.backdrop or win.strip or win.fullscreen
     or win.headed or win.popup or win.tip then
    return win.x, win.y, win.w, win.h
  end

  return win.x - OUT.BORDER,
         win.y - OUT.TAB_H,
         win.w + OUT.BORDER * 2,
         win.h + OUT.TAB_H + OUT.BORDER
end

--
-- **The frame plus its shadow**, which is what has to be repainted when a
-- window moves or goes away - and is *not* what a click hits.
--
-- `frame_of` stays the window: hit-testing, stacking and the outline a drag
-- draws all mean the thing somebody can grab, and a shadow is not that. A
-- shadow that took clicks would be a window with an invisible border a
-- fortnight wide, which is the failure every compositor with soft edges has
-- had at least once.
--
-- Damage is the other question - what is on the screen because of this
-- window - and the answer there does include it. Separating the two is the
-- whole of what shadows cost in this design, because `frame_of`'s comment
-- was already true: it is the only place that has to know.
--
--
-- **Rounding a window, by keeping its corners and putting them back.**
--
-- Everything a window puts on the screen goes through half a dozen calls -
-- a gradient for the tab, fills for the border and the controls, a blit or
-- a stretch for the content - and rounding each of them would be six places
-- to keep in step and six chances to miss one. A window that was rounded in
-- five of them has a square notch, which is worse than a square window.
--
-- So the corners are *saved* from the backbuffer before the window is
-- painted and copied back afterwards over the pixels outside the arc. The
-- backbuffer at that moment holds everything behind this window, because
-- the desktop composes back to front - so what goes back is exactly what
-- should show through, whatever it is, and no drawing call has to know any
-- of this is happening.
--
-- Four squares of `corner` a side, clipped to the damaged rectangle: 256
-- pixels at the default radius, saved and restored once per window per
-- rectangle. Against a window of half a million it does not appear in a
-- profile.
--
OUT.keep_surface = gfx.surface{ w = 64, h = 64 }

--
-- The four corner squares of a frame, clipped to `r`, as
-- `{ sx, sy, w, h, kx, ky }` - where in the backbuffer, and where in the
-- scratch it goes. Empty when the rectangle touches no corner, which is the
-- common case for a window being dragged across the middle of the screen.
--
--
-- **One list, refilled at each call** (`compose.lua`, 18.439): its caller
-- keeps the corners, draws, and puts them back with no other call between,
-- so the list and its four entries are this function's, made once.
--
local corner_list, corner_entries = {}, { {}, {}, {}, {} }
local corner_at = { {}, {}, {}, {} }

function OUT.corners(fx, fy, fw, fh, r)
  local c = OUT.corner
  local out = corner_list

  for i = #out, 1, -1 do out[i] = nil end

  if c <= 0 or c > 32 then return out end

  local a = corner_at

  a[1][1], a[1][2], a[1][3], a[1][4] = fx, fy, 0, 0
  a[2][1], a[2][2], a[2][3], a[2][4] = fx + fw - c, fy, c, 0
  a[3][1], a[3][2], a[3][3], a[3][4] = fx, fy + fh - c, 0, c
  a[4][1], a[4][2], a[4][3], a[4][4] = fx + fw - c, fy + fh - c, c, c

  for k = 1, 4 do
    local at = a[k]
    local x0 = math.max(at[1], r.x)
    local y0 = math.max(at[2], r.y)
    local x1 = math.min(at[1] + c, r.x + r.w)
    local y1 = math.min(at[3] and at[2] + c or 0, r.y + r.h)

    if x1 > x0 and y1 > y0 then
      local e = corner_entries[#out + 1]

      e[1], e[2], e[3], e[4] = x0, y0, x1 - x0, y1 - y0
      e[5], e[6] = at[3] + (x0 - at[1]), at[4] + (y0 - at[2])
      out[#out + 1] = e
    end
  end

  return out
end

function OUT.keep(list)
  for _, c in ipairs(list) do
    OUT.keep_surface:blit(back, c[1], c[2], c[3], c[4], c[5], c[6])
  end
end

function OUT.put_back(list, fx, fy, fw, fh)
  for _, c in ipairs(list) do
    back:blit_round(OUT.keep_surface, c[5], c[6], c[3], c[4], c[1], c[2],
                    fx, fy, fw, fh, OUT.corner, true)
  end
end

--
-- **The page rounded inside the frame**, at its two bottom corners: the
-- frame's colour painted over the page's pixels outside an arc of the
-- corner less the border, anti-aliased by the same coverage that rounds the
-- frame itself. So the frame is a band of one width all the way round the
-- curve, where the page used to show through the frame's arc as a dark
-- sliver with a square line beside it. The top corners of the page meet
-- the title bar and are square, as a page under a bar is.
--
-- Through `blit_round` from a square of the frame's colour, filled again
-- only when the colour changes - focused and unfocused are the two.
--
OUT.frame_fill = gfx.surface{ w = 64, h = 64 }

function OUT.round_inside(win, r, colour)
  local c = OUT.corner - OUT.BORDER

  if c <= 0 or c > 64 or win.kind == "menu" then return end

  if OUT.frame_fill_colour ~= colour then
    OUT.frame_fill:fill(0, 0, 64, 64, colour)
    OUT.frame_fill_colour = colour
  end

  local y = win.y + win.h - c

  for _, x in ipairs({ win.x, win.x + win.w - c }) do
    local x0, y0 = math.max(x, r.x), math.max(y, r.y)
    local x1 = math.min(x + c, r.x + r.w)
    local y1 = math.min(y + c, r.y + r.h)

    if x1 > x0 and y1 > y0 then
      back:blit_round(OUT.frame_fill, 0, 0, x1 - x0, y1 - y0, x0, y0,
                      win.x, win.y, win.w, win.h, c, true)
    end
  end
end

--
-- **A rounded window does not cover its corners**, so they are not cut out
-- of what is painted behind it. The pass below cut the frame's whole
-- rectangle away, which was exact while windows were square; with a corner
-- the desktop - or the window behind - was then never painted under the
-- arc, and `put_back` restored whatever the backbuffer last held there.
-- Diego photographed it on 24 September: Photo's own dark page outside its
-- bottom-right curve, left from where the window was before `tile` moved
-- it. Only a window that moved onto its own old pixels showed it, which is
-- why Tracker beside it looked right.
--
-- The four squares of `corner` a side, when this window is rounded -
-- the same condition `OUT.corners` rounds it under.
--
function OUT.corner_squares(win)
  local c = OUT.corner

  if c <= 0 or c > 32 then return nil end

  local fx, fy, fw, fh = frame_of(win)

  -- Kept on the window and refilled, rather than five tables a pass.
  local s = win.corner_squares or { 0, {}, {}, {}, {} }

  win.corner_squares = s
  s[1] = c
  s[2][1], s[2][2] = fx, fy
  s[3][1], s[3][2] = fx + fw - c, fy
  s[4][1], s[4][2] = fx, fy + fh - c
  s[5][1], s[5][2] = fx + fw - c, fy + fh - c

  return s
end

-- What of those squares lies inside `x0, y0 - x1, y1`, the part of a piece
-- the window was cut from, handed back to be painted behind it.
function OUT.uncover(keep, round, x0, y0, x1, y1, rect)
  local c = round[1]

  for k = 2, 5 do
    local sx, sy = round[k][1], round[k][2]
    local ix0, iy0 = math.max(sx, x0), math.max(sy, y0)
    local ix1, iy1 = math.min(sx + c, x1), math.min(sy + c, y1)

    if ix1 > ix0 and iy1 > iy0 then
      keep[#keep + 1] = rect(ix0, iy0, ix1 - ix0, iy1 - iy0)
    end
  end
end

--
-- **What the power button and the Super key do**, as Preferences' Power
-- and Keyboard pages set them (`/Home/Preferences/power`, `/Home/Preferences/keyboard`). Read
-- once at the start and told again by `handlers.keys` when a row changes,
-- because both are acted on in the key path, where a read from the disk is
-- a call that may not be made. Until 24 September both rows wrote files
-- nothing read: the button always shut down and the key always opened the
-- menu.
--
OUT.keys = { power = "off", super = "menu" }

-- Focus following the pointer: off until Preferences turns it on, a second
-- by default (`load_appearance`, `handlers.keys`, `wm/pointer.lua`).
OUT.focus_follows, OUT.focus_delay_ms = false, 1000

function OUT.load_keys()
  local power = prefs.read("power")
  local keyboard = prefs.read("keyboard")

  if type(power) == "table" and power.button then
    OUT.keys.power = power.button
  end

  if type(keyboard) == "table" and keyboard.super then
    OUT.keys.super = keyboard.super
  end
end

OUT.load_keys()

function OUT.shadowed(win)
  local fx, fy, fw, fh = frame_of(win)

  if win.kind == "menu" or win.backdrop or win.strip or win.fullscreen or win.tip then
    return fx, fy, fw, fh
  end

  local s = OUT.shadow

  if s <= 0 then return fx, fy, fw, fh end

  -- Down by a third, as `shadow` in the gfx kit draws it.
  return fx - s, fy - s + s // 3, fw + s * 2, fh + s * 2
end

local function damage_window(win)
  add_damage(OUT.shadowed(win))
end

--
-- Only the four edges, not the rectangle they enclose.
--
-- A resize drag damages the outline twice per step - where it was and where
-- it is - and the whole point is that this costs nothing. Damaging the area
-- inside would recomposite every window under it on every pointer movement,
-- which is the expense the outline exists to avoid.
--
local OUTLINE = 2

local function damage_outline(o)
  if not o then return end

  add_damage(o.x, o.y, o.w, OUTLINE)
  add_damage(o.x, o.y + o.h - OUTLINE, o.w, OUTLINE)
  add_damage(o.x, o.y, OUTLINE, o.h)
  add_damage(o.x + o.w - OUTLINE, o.y, OUTLINE, o.h)
end

--
-- How far down the screen an ordinary window may start.
--
-- `TAB_H` on its own, until something claims a strip across the top. A
-- window's own title bar is drawn *above* `win.y`, so the smallest `win.y`
-- that leaves the tab on screen is `TAB_H` - and with a bar up there it is
-- that plus the bar.
--
-- A number the strip sets rather than a constant, because the strip is an
-- application and its height is its own business: it chooses a font, and a
-- compositor that had the height compiled into it would put every window
-- under a bar of the wrong size the first time somebody chose a bigger one.
--
local reserved_top = 0

--
-- **And the dock's, at the bottom** (`roadmap.md`, a dock at the bottom;
-- Diego, 3 October: "place the taskbar on the bottom center"): a strip
-- across the foot of the screen, or a dock floating above it, which takes
-- its height - and when it floats, the gap under it and one above - away
-- from what a maximised window may cover. Nought with no dock.
--
local reserved_bottom = 0
-- A floating dock's distance from the edge, in points: a tenth of its
-- height, as macOS's dock has it (Diego, 3 October, with a picture of his:
-- "the whitespace from the bottom of the screen to the dock should be half
-- of whats now", "mac os dock margin to the bottom of the screen is what it
-- should be"). It was 12, the drawing's.
local DOCK_GAP = 6

-- The room a bottom strip takes from the screen: its height, and the gap
-- above and below it when it floats.
local function bottom_room(win)
  return win.h + (win.floating and scale.px(DOCK_GAP) * 2 or 0)
end

-- The stamp in the corner sits above the dock (`draw_desktop`), so where it
-- was and where it is are both drawn again when the dock's room changes.
local function bottom_changed(was)
  if was == reserved_bottom then return end

  local band = math.max(was, reserved_bottom) + gfx.font.h + 16

  add_damage(0, H - band, W, band)
end

-- Where a bottom strip goes: centred and `DOCK_GAP` above the edge when it
-- floats, across the foot of the screen when it does not.
local function place_bottom(win)
  local gap = win.floating and scale.px(DOCK_GAP) or 0

  win.x = win.floating and (W - win.w) // 2 or 0
  win.y = H - win.h - gap
end

--
-- **The order windows are drawn and pressed in**: the stack, and the dock
-- in front of all of it. It is not moved to the top of the stack, because
-- the top of the stack is the focus (`focused_window`) and a dock that took
-- the keys would take them from every window. Nil when there is no dock,
-- which is every look but one, so that case costs nothing; the table is
-- reused, so the other costs no garbage.
--
local drawing_order = {}

function OUT.order()
  local dock, tips = false, false

  for _, w in ipairs(windows) do
    if w.strip == "bottom" then dock = true end
    if w.tip then tips = true end
  end

  if not dock and not tips then return nil end

  local n = 0

  for i, w in ipairs(windows) do
    if w.strip ~= "bottom" and not w.tip then n = n + 1; drawing_order[n] = i end
  end

  for i, w in ipairs(windows) do
    if w.strip == "bottom" then n = n + 1; drawing_order[n] = i end
  end

  -- And the tips and banners in front of even the dock: a name over one
  -- of its icons, and a notification, which is there to be seen.
  for i, w in ipairs(windows) do
    if w.tip then n = n + 1; drawing_order[n] = i end
  end

  for k = #drawing_order, n + 1, -1 do drawing_order[k] = nil end

  return drawing_order
end

local function top_limit()
  return OUT.TAB_H + reserved_top
end

--
-- **How high this window may go**: its tab's height below the strip, or
-- right up to the strip when it has no tab (`win.headed`) - its header is
-- then the handle, and it is inside the window.
--
function OUT.top_of(win)
  return win.headed and reserved_top or top_limit()
end

--
-- **The largest page this window may have**: the screen less the strip,
-- for a window with no tab and no border; the screen less those for one
-- with them.
--
function OUT.room(win)
  if win.headed then return W, H - reserved_top - reserved_bottom end

  return W - OUT.BORDER * 2, H - OUT.TAB_H - OUT.BORDER
end

--
-- **Whether this window's header is its title bar**: it has one the kit
-- can put the three in (`win.can_head`). True or nil. It used to be the
-- look's to say as well (`title_bars = no`), and four looks kept the old
-- tab above every window; there is one chrome now (`roadmap.md`, agreed 7
-- October), and the kit gives every ordinary window a header.
--
function OUT.headed(win)
  return win.can_head and true or nil
end

--
-- **Whether a window asking to open has a header that can be the title
-- bar**: it says so, and it is an ordinary window - not a menu, the
-- backdrop, a strip or full screen, none of which has a bar to lose.
--
-- A window that draws its own pixels may say so as well, since Groove
-- (`roadmap.md` 6zh): PulseMusic's top bar is its title bar, it leaves the
-- three their room at its right end and says where (`handlers.lights`), and
-- it hands a press on the bar's empty band back as a drag
-- (`handlers.move_begin`) - which is everything a kit's header does. This
-- used to leave such a window out, on the grounds that the kit had no
-- header in it to put the three in; the window has one of its own.
--
function OUT.wants_head(req)
  return req.header == true and not req.kind and not req.backdrop
         and not req.strip and not req.fullscreen
end

-- The room the three take, in a window's points: what its header leaves
-- free for this process to draw them in (`handlers.lights`).
function OUT.lights_size(pct)
  return { w = scale.pt(OUT.RUN, pct), h = scale.pt(OUT.BOX, pct) }
end

--
-- **A new look, and a window gains its tab or loses it.** Its page stays
-- where it is; one that gains a tab is moved down if the tab would be
-- above the room, since a tab off the top of the screen is a window that
-- cannot be taken hold of.
--
function OUT.rehead(win)
  local now = OUT.headed(win)

  if now == win.headed then return end

  damage_window(win)
  win.headed = now

  if win.y < OUT.top_of(win) then win.y = OUT.top_of(win) end

  damage_window(win)
  print(("wm: %s %s"):format(tostring(win.title),
        now and "has its header for a title bar" or "wears a title bar"))
end

--
-- Where the first of the three boxes starts; `OUT.SLOT` says which is where.
--
-- One function rather than the same arithmetic in the compositor and in the
-- pointer, because those two agreeing by coincidence is how a control ends
-- up drawn in one place and clickable in another.
--
--
-- **Where a headed window's three are**: where its header said (`handlers.
-- lights`), or until it has, where every header puts them - 12 in from the
-- right and centred in the 46 of the band. Nowhere at all was the answer
-- before, and a window that stopped before it first drew had no close box:
-- with the old chrome gone (one window chrome, 7 October) the window
-- manager's own bar no longer stood in for it.
--
function OUT.lights_at(win)
  if win.lights_at then return win.lights_at end

  local pct = win.pct or 100

  return { x = win.w - scale.px(12, pct) - OUT.RUN,
           y = (scale.px(46, pct) - 1 - OUT.BOX) // 2 }
end

local function boxes_x(win)
  local fx = frame_of(win)

  --
  -- **Three boxes now, all at the right** - maximise, minimise, close
  -- since 24 September (`OUT.SLOT`). Diego, 23 September 2026: "i would
  -- like the close, minimize and maximize buttons to be placed in the
  -- right side of the bar like windows does", with a screenshot of one
  -- beside it.
  --
  -- This was BeOS's split - close at the left, the other two at the right -
  -- and the argument for it was real and is worth keeping written down:
  -- close is the irreversible one, and a window's width between it and the
  -- two harmless ones means a slip hides a window instead of ending it.
  --
  -- What outweighs it is that every machine anybody has used for thirty
  -- years puts them together at the right, and a hand that has been aiming
  -- there since Windows 95 does not care which arrangement is safer in
  -- principle. `ui.md` 16.8b is the rule this follows rather than breaks:
  -- copy a decision about *behaviour*, decide a decision about *shape*
  -- fresh - and where the controls sit is shape.
  --
  -- The run spans from here to `2 * BOX_W + BOX`: each box starts a slot
  -- and the last is `BOX` wide, with the far edge `MARGIN` from the frame.
  --
  -- **On a window with no tab, where its header left room for them**
  -- (`handlers.lights`), and where every header leaves it until it has said
  -- (`OUT.lights_at`).
  --
  if win.headed then
    return win.x + OUT.lights_at(win).x
  end

  return fx + tabs.width(win) - OUT.MARGIN - OUT.RUN
end

--
-- **The three as coloured circles** (`roadmap.md` 5zq). Diego, 24
-- September, with a picture of the three traffic lights: "Can we change our
-- window title bar buttons to this style of the photo? Rounded and
-- colored". Each is a disc with a ring a shade darker, and its glyph is
-- drawn only while the pointer is over the three - which is what makes a
-- row of bright dots read as controls rather than as decoration.
--
-- **The colour follows the job, not the picture's order.** The picture
-- puts close first; this bar keeps close outermost at the right, where
-- Diego asked for it on 23 September ("like windows does"). So minimise is
-- amber, maximise green and close red, wherever each sits.
--
-- Fixed colours rather than a look's tokens, because they are a sign and
-- not a surface: red means this ends the window in every look, the same
-- way a stop sign is not themed.
--
OUT.LIGHTS = {
  close    = { fill = 0xffff5f57, ring = 0xffe0443e, mark = 0xff8c1a10 },
  minimise = { fill = 0xfffebc2e, ring = 0xffdea123, mark = 0xff985b00 },
  maximise = { fill = 0xff28c840, ring = 0xff1aab29, mark = 0xff0b6a1d },
}

--
-- **Which slot each is in, from the left: green, amber, red.** Diego, 24
-- September: "the windows bar close, maximize and minimize buttons are
-- incorect oder", "from left to right: green - maximixze, yellow -
-- minimize, red - close". They were minimise, maximise, close - Windows's
-- order in macOS's colours. Close stays outermost.
--
-- One table that the drawing and the press both read, so a box cannot be
-- drawn in one slot and pressed in another.
--
OUT.SLOT = { maximise = 0, minimise = 1, close = 2 }
OUT.IN_SLOT = { [0] = "maximise", [1] = "minimise", [2] = "close" }

--
-- The window whose three the pointer is over, or nil - and the rectangle
-- they take, so a change of which is repainted without the whole tab.
--
function OUT.boxes_rect(win)
  local bx = boxes_x(win)

  if not bx then return nil end

  if win.headed then
    return bx, win.y + OUT.lights_at(win).y, OUT.RUN, OUT.BOX
  end

  local _, fy = frame_of(win)

  return bx, fy + (OUT.TAB_H - OUT.BOX) // 2, OUT.RUN, OUT.BOX
end

function OUT.damage_boxes(win)
  if not win then return end

  -- Not `win and OUT.boxes_rect(win)`: `and` keeps one value of the four.
  local x, y, w, h = OUT.boxes_rect(win)

  if x then add_damage(x, y, w, h) end
end

--
-- One of them: a disc, its ring, and - while `lit` - its glyph, all centred
-- in the square its slot gives it. The disc is four pixels smaller than the
-- slot, so the three sit apart the way the picture spaces them, and the
-- square around each is still what a press lands in: a target drawn
-- smaller than it can be clicked is fine, and the reverse is the lie `BOX`
-- exists to prevent.
--
function OUT.light(x, y, kind, lit, off)
  local c = OUT.LIGHTS[kind]
  local d = OUT.BOX - scale.px(4)
  local dx, dy = x + (OUT.BOX - d) // 2, y + (OUT.BOX - d) // 2

  if off then
    back:fill_round(dx, dy, d, d, theme.track, d // 2)
    back:frame_round(dx, dy, d, d, theme.line_soft, d // 2)
    return
  end

  back:fill_round(dx, dy, d, d, c.fill, d // 2)
  back:frame_round(dx, dy, d, d, c.ring, d // 2)

  if not lit then return end

  local cx, cy = dx + d // 2, dy + d // 2
  local k = d // 4

  if kind == "minimise" then
    back:fill(cx - k - 1, cy - 1, 2 * k + 2, 2, c.mark)
  elseif kind == "close" then
    -- A cross as two diagonals of two-pixel steps: there is no line
    -- primitive, and at this size two pixels wide is what the picture's
    -- stroke comes to.
    for i = -k, k do
      back:fill(cx + i - 1, cy + i - 1, 2, 2, c.mark)
      back:fill(cx - i - 1, cy + i - 1, 2, 2, c.mark)
    end
  else
    -- Two corners pulling apart, as the picture draws maximise.
    back:triangle(cx - k - 1, cy - k, cx + k, cy - k, cx + k, cy + k - 1,
                  c.mark)
    back:triangle(cx - k, cy - k + 1, cx - k, cy + k, cx + k - 1, cy + k,
                  c.mark)
  end
end

--------------------------------------------------------------------------
-- The drawing commands an application may send.
--
-- A small set on purpose. Each one is a primitive that already exists in
-- C, so this table is a name-to-primitive map and not an interpreter -
-- there is nothing here that loops over pixels in Lua, which is the rule
-- `gfx.md` 19.2 exists to keep.
--
-- Anything unrecognised is skipped rather than refused. An application
-- built against a later version of this list should lose a rectangle, not
-- its window.
--------------------------------------------------------------------------

--
-- **A drawing command, in the screen's pixels at a scale** (`ui.md` 16.18).
-- A rectangle by its two edges, so neighbours still meet at a step that is
-- not whole; text at its place, since its face is already loaded at the
-- scale (`apply_fonts`, `sized`); a triangle's corners, which `gfx` takes
-- as doubles; and a picture drawn at its scaled size, averaged - an icon
-- from its 64-pixel export when there is one, which is every icon Haiku's
-- set gives, so it is shrunk rather than blown up.
--
function scale.op(o, pct)
  local kind = o.op

  if kind == "fill" or kind == "fill_round" or kind == "frame_round" then
    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0
    local w, h = tonumber(o.w) or 0, tonumber(o.h) or 0
    local x0, y0 = scale.px(x, pct), scale.px(y, pct)

    o.x, o.y = x0, y0
    o.w, o.h = scale.px(x + w, pct) - x0, scale.px(y + h, pct) - y0

    -- The radius is a length like any other, so a control at 150 per cent
    -- is rounded a half more rather than keeping the pixels of a smaller
    -- screen.
    if o.r then o.r = scale.px(o.r, pct) end
  elseif kind == "text" then
    o.x = scale.px(tonumber(o.x) or 0, pct)
    o.y = scale.px(tonumber(o.y) or 0, pct)
  elseif kind == "triangle" then
    for _, k in ipairs({ "x1", "y1", "x2", "y2", "x3", "y3" }) do
      o[k] = (tonumber(o[k]) or 0) * pct / 100
    end
  elseif kind == "tint" then
    --
    -- **A line icon at another size is another picture**, not this one
    -- resampled: `tools/lineicons.py` renders each from its vectors at 15,
    -- 19, 23 and 30, which is what 15 points comes to at 100, 125, 150 and
    -- 200 per cent. A one-pixel line averaged into a larger box is a grey
    -- smear; drawn again at the size, it is a line. A size with no picture
    -- keeps the 15 at the scaled place, which is small rather than wrong.
    --
    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0
    local want = scale.px(tonumber(o.w) or 0, pct)
    local base = tostring(o.asset or ""):match("^(.-)%-%d+%.png$")

    o.x, o.y = scale.px(x, pct), scale.px(y, pct)

    if base and picture_named(("%s-%d.png"):format(base, want)) then
      o.asset = ("%s-%d.png"):format(base, want)
      o.w, o.h = want, want
    end
  elseif kind == "image" then
    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0
    local dw, dh = tonumber(o.dw) or 0, tonumber(o.dh) or 0

    -- No size given is a crop drawn pixel for pixel: its size is the crop's.
    if dw <= 0 or dh <= 0 then
      dw, dh = tonumber(o.w) or 0, tonumber(o.h) or 0
    end

    local x0, y0 = scale.px(x, pct), scale.px(y, pct)

    o.x, o.y = x0, y0
    o.dw, o.dh = scale.px(x + dw, pct) - x0, scale.px(y + dh, pct) - y0
    o.smooth = true

    -- An icon's 64: `16x16/<name>` or a bare 32 name, when the 64 exists.
    local asset = tostring(o.asset or "")
    local name, from = asset:match("^16x16/(.+)$"), 16

    if not name and not asset:find("/", 1, true) then
      name, from = asset, 32
    end

    if name and picture_named("64x64/" .. name) then
      local k = 64 // from

      o.asset = "64x64/" .. name
      o.sx, o.sy = (tonumber(o.sx) or 0) * k, (tonumber(o.sy) or 0) * k
      o.w, o.h = (tonumber(o.w) or 0) * k, (tonumber(o.h) or 0) * k
    end
  end
end

-- The commands a window sends, drawn: `/Kosmos/Libraries/paint.lua`, shared with
-- `ui.paint_view` so a widget looks the same in a window that owns its
-- pixels as in one that sends drawing.
local ops = use("/Kosmos/Libraries/paint.lua").new(picture_named, sized)

--------------------------------------------------------------------------
-- The pointer.
--
-- The tablet reports absolute position in a range of its own - 0 to 32767
-- on this machine, whatever the display is - so the scaling happens here,
-- where the size of the screen is known. The kernel passes the range out
-- undecoded for exactly this reason.
--
-- Absolute rather than relative is the right kind of device for a virtual
-- machine: there is no acceleration curve to agree on with the host, so the
-- guest cursor cannot drift away from the real one.
--
-- The cursor is composited like everything else: drawn into the backbuffer
-- after the windows, so the blit that reaches the screen already has it.
--
-- It was drawn straight onto the screen after the composite, which is
-- cheaper and wrong. Every repaint blits the finished region over the
-- cursor and then puts it back on the next line - and QEMU scans out on its
-- own schedule, so it can sample between those two. What that looks like is
-- the cursor flickering on every click, once on the press and once on the
-- release, because each of those repaints the window it is sitting on.
--
-- Moving it costs two rectangles of damage instead of one: where it was, so
-- it is erased, and where it is, so it is drawn. That is the price of the
-- frame being whole.
--------------------------------------------------------------------------

local CURSOR_W, CURSOR_H = 10, 16

-- An arrow. Two colours so it stays visible on a light window and on the
-- dark desktop: white body, dark outline.
local CURSOR = {
  "X.........",
  "XX........",
  "XoX.......",
  "XooX......",
  "XoooX.....",
  "XooooX....",
  "XoooooX...",
  "XooooooX..",
  "XoooooooX.",
  "XooooooooX",
  "XoooooXXXX",
  "XooXooX...",
  "XoX.XooX..",
  "XX..XooX..",
  "X....XooX.",
  "......XX..",
}

-- **The pointer's state, in one table** (`roadmap.md` 6zn): where it is,
-- its buttons, and what a press has started - a drag, a resize, a grab, an
-- outline, a drop. The pointer's pass sets these and the compositor and the
-- rest read them, so they are one table's fields rather than locals a part
-- in a file of its own could only have a copy of.
local PT = { x = 0, y = 0 }
PT.buttons = 0
PT.dragging = nil          -- { win, dx, dy } while a title bar is held

--
-- **Which keys are held**, from the raw events every key sends (the same
-- numbers on both boards, `hal/keys.h`), for a press that asks with them:
-- Super and Control held and a window pressed anywhere moves it
-- (`roadmap.md` 6zj - Diego: "a key combination and that activates full
-- window drag? Like super+ctrl+click and drag"). The window manager sees
-- the keys and the press before any application, so it keeps the press.
--
-- **And the Super it was held with does not open the menu.** Both keyboard
-- drivers send Super's tap - `ESC [ 1 ; 9 ~`, the Kosmos menu - on its
-- release when no letter came between, and a click is not a letter; so a
-- move marks the Super held for it, and that Super's tap is let go by.
--
OUT.chord = { held = {}, super_moved = false }

function OUT.chord.move_held()
  local held = OUT.chord.held

  return (held[29] or held[97])                    -- Control, left or right
         and (held[125] or held[126])              -- Super, left or right
         and true or false
end
PT.resizing = nil          -- { win, ox, oy, ow, oh } while a grip is held

-- The right button's own grab, because the two buttons are two
-- conversations: one can be held while the other is pressed and released,
-- and sharing `grabbed` would have the second end the first.
PT.right_grabbed = nil

--
-- The rubber band: where the frame *would* be, while the grip is held.
--
-- Resizing used to reallocate the window's surface on every step of the
-- drag. At 1920x1080 that is an eight megabyte allocation, a fill, a whole
-- frame of damage and a resize event to the application - per pointer
-- movement. What that looks like is the window going black and stuttering
-- while you drag it, because the surface is new and empty and the
-- application has not had a chance to draw into it yet.
--
-- So the drag moves an outline and nothing else, and the window is resized
-- once, on release. Four thin rectangles instead of eight megabytes. This is
-- what X11 and every desktop of that era did, and the reason was the same
-- one.
--
PT.outline = nil           -- { x, y, w, h }, a frame rectangle

-- Declared here and defined below, because the compositor and the pointer
-- both ask about resizing and both run above the code that answers. Locals
-- rather than globals: `luaglobals` is what caught this, which is what it
-- is for - twice now, and both times within a minute of writing the bug.
-- `move_window` too, for the drag and the keyboard (`handlers.move`), and
-- `luaglobals` caught it a third time.
local resizable, resize_window, move_window

-- The same, for the queue every handler puts events on. `handlers.move`
-- reports where a window ended up, and it is written above the function
-- that does the reporting.
local post

-- The Deskbar's Kosmos menu, opened as if its button were pressed: the
-- Super key's, and the power button's when Preferences asks for the menu.
-- Posted, never sent - it runs on the key path.
function OUT.open_kosmos_menu()
  local to = nil

  -- The dock's Kosmos button when there is one, the bar's otherwise.
  for _, win in ipairs(windows) do
    if win.strip == "bottom" then to = win break end
    if win.strip and not to then to = win end
  end

  if to then post(to, { type = "menu" }) end
end
PT.grabbed = nil           -- the window a press landed in, until release

--------------------------------------------------------------------------
-- Dragging something from one window into another.
--
-- **This process carries a payload it never looks at.** A drag is started by
-- the window the press landed in, which says what is being dragged as a
-- `kind` and an opaque string; on release this finds the window under the
-- pointer and posts the string to it. Tracker puts paths in there. Nothing
-- here knows that, and the whole reason the desktop can have a file manager
-- at all is that it does not - `handlers.open`'s backdrop comment is the
-- same argument, one layer along.
--
-- A press already grabs, so every move and the release go to the *source*
-- window whatever the pointer is over. That is right for the source, which
-- is drawing its own rubber band, and is exactly why the destination cannot
-- learn about the drop by itself: it never sees the pointer. Only this
-- process does.
--
-- `answering` is a one-shot right to reply, and it is a capability in the
-- small. Without it a `dropped` message would be a way for any window to
-- post an event to any other window by guessing a handle, which is a hole
-- straight through everything else this system does with names. With it,
-- the only window that can answer is the one this process just handed a
-- drop to, once.
--------------------------------------------------------------------------
PT.drag = nil              -- { from, kind, payload, label } while held
PT.answering = nil         -- { from, to } between a drop and its answer

-- Where the badge sits next to the arrow, and how big the damage has to be
-- while one is up. Both rectangles are the cursor's, widened.
local BADGE_DX, BADGE_DY = 12, 10
local BADGE_PAD = 4

--------------------------------------------------------------------------
-- The frame profile - `/Kosmos/Libraries/wm/profile.lua`, where it says
-- what it measures and why (`roadmap.md` 6zn). Its state is `P`'s: the
-- loop and `frames` set and read `P.profiling`, `P.measuring` and the rest.
--------------------------------------------------------------------------

local P = use("/Kosmos/Libraries/wm/profile.lua")

--
-- A raised control, drawn straight onto the backbuffer.
--
-- The kit has `gc:raised` and this cannot use it: that builds a *command*
-- for an application's window, and the decoration is drawn here, in screen
-- coordinates, by the process that owns the screen. Same two edges and the
-- same colour tokens, so the two agree about what raised looks like even
-- though neither can call the other.
--
--
-- A control on the tab, raised - or in a flat look, outlined.
--
-- The same rule the kit follows (`ui.lua`, `gc:raised`): a look that says
-- `flat` has nothing to say about which way a surface faces, so what
-- separates a control from the bar it sits on is the line alone.
--
local function raised_box(x, y, w, h, face)
  back:fill(x, y, w, h, face)

  if theme.flat then
    back:fill(x, y, w, 1, theme.line_soft)
    back:fill(x, y + h - 1, w, 1, theme.line_soft)
    back:fill(x, y, 1, h, theme.line_soft)
    back:fill(x + w - 1, y, 1, h, theme.line_soft)
    return
  end

  back:fill(x, y, w - 1, 1, theme.edge_light)
  back:fill(x, y, 1, h - 1, theme.edge_light)
  back:fill(x, y + h - 1, w, 1, theme.edge_dark)
  back:fill(x + w - 1, y, 1, h, theme.edge_dark)
end

--
-- How much of the screen the pointer occupies, which is not always the
-- arrow. While something is being dragged the arrow carries a label saying
-- what - "3 items" - and the damage rectangle has to include it.
--
-- One function, because the compositor and the pointer both need the answer
-- and a badge drawn outside the rectangle that was damaged for it is a
-- smear that stays on the screen until something else repaints over it.
--
local function badge_size()
  if not (PT.drag and PT.drag.label) then return 0, 0 end

  return gfx.measure(PT.drag.label) + BADGE_PAD * 2, gfx.font.h + BADGE_PAD
end

local function cursor_size()
  local bw, bh = badge_size()

  -- The display draws the arrow (`OUT.hw_cursor`, below): nothing of it is
  -- in a frame, so a move damages nothing but a drag's badge.
  if bw == 0 then
    if OUT.hw_cursor then return 0, 0 end

    return CURSOR_W, CURSOR_H
  end

  return math.max(CURSOR_W, BADGE_DX + bw),
         math.max(CURSOR_H, BADGE_DY + bh)
end

--
-- Into the backbuffer, clipped by the surface primitives like anything
-- else. A run of identical pixels at a time rather than one fill per pixel:
-- the arrow is ten columns wide and mostly runs.
--
local function draw_cursor()
  for row = 0, OUT.hw_cursor and -1 or CURSOR_H - 1 do
    local line = CURSOR[row + 1]
    local col = 0

    while col < CURSOR_W do
      local ch = line:sub(col + 1, col + 1)

      if ch == "." then
        col = col + 1
      else
        local run = 1

        while line:sub(col + run + 1, col + run + 1) == ch do
          run = run + 1
        end

        back:fill(PT.x + col, PT.y + row, run, 1,
                  (ch == "X") and 0xff000000 or 0xffffffff)
        col = col + run
      end
    end
  end

  --
  -- What is being carried, in a word, beside the arrow.
  --
  -- BeOS drew the icons themselves, half transparent, and that is nicer and
  -- is also a picture this process would have to be given and hold. A label
  -- says the one thing the person actually needs while the pointer is
  -- moving - that a drag is happening at all, and how much of one - and it
  -- costs a fill and a string.
  --
  local bw, bh = badge_size()

  if bw > 0 then
    raised_box(PT.x + BADGE_DX, PT.y + BADGE_DY, bw, bh,
               theme.raised)
    back:text(PT.x + BADGE_DX + BADGE_PAD,
              PT.y + BADGE_DY + BADGE_PAD // 2,
              PT.drag.label, theme.text, theme.raised)
  end
end

--
-- **The pointer drawn by the display, when it can** (`roadmap.md` 4h b): the
-- arrow handed over once, a 64 by 64 picture with its alpha, and from then on
-- only its place - so a move composes no frame and sends no pixels, and the
-- arrow is in no frame at all. A drag's badge stays here, drawn beside it. A
-- display with no pointer of its own - ramfb, a firmware screen - says no,
-- and the arrow is composited as it always was. Said, for whoever reads the
-- log (`tools/run_screenshot.py`'s cursor phase).
--
do
  local picture = gfx.surface{ w = 64, h = 64 }       -- zeroed: transparent

  for row = 0, CURSOR_H - 1 do
    local line = CURSOR[row + 1]

    for col = 0, CURSOR_W - 1 do
      local ch = line:sub(col + 1, col + 1)

      if ch ~= "." then
        picture:fill(col, row, 1, 1, (ch == "X") and 0xff000000 or 0xffffffff)
      end
    end
  end

  OUT.hw_cursor = gfx.cursor and gfx.cursor(picture, 0, 0, PT.x, PT.y) == true
  picture:free()
  print(OUT.hw_cursor and "wm: the pointer drawn by the display"
        or "wm: the pointer composited")
end

--------------------------------------------------------------------------
-- Compositing.
--
-- One rectangle at a time: what each window shows of it, worked out front
-- to back; the desktop where no window reaches; the windows back to front;
-- then the copy out. A window cuts away what is behind it except where it
-- is blended - the backdrop, a strip that asked to be, a rounded corner and
-- a shadow. The passes are `/Kosmos/Libraries/wm/compose.lua`, below; what
-- is here are the pieces it draws with.
--------------------------------------------------------------------------

--
-- The desktop under everything: the wallpaper, or the flat colour, and the
-- stamp in the corner.
--
-- Split out of `compose_rect` when occlusion culling arrived, because it
-- stopped being something that happens once per damage rectangle and became
-- something that happens once per *piece of a rectangle that no window
-- covers*. On a busy screen that is often nothing at all.
--
local function draw_desktop(r)
  --
  -- The desktop: a picture if there is one, the flat colour if not.
  --
  -- **The fill is skipped when the picture covers this rectangle**, which
  -- is every rectangle when the wallpaper is the size of the screen - the
  -- case worth having, and the one the Appearance app tells you to aim for.
  -- Filling first and blitting over it wrote every damaged pixel twice, and
  -- composing is already eighty-odd per cent of a pass.
  --
  -- Only the part this rectangle covers, in any case: the compositor draws
  -- in damaged pieces, and blitting the whole picture for a ten-pixel change
  -- would cost the whole screen every time the clock ticked.
  --
  -- **No message anywhere near this.** The wallpaper is asked for once, when
  -- somebody picks one, and lives here as a surface from then on. What
  -- happens per frame is one `blit`, which is C.
  --
  local covered = wallpaper
                  and r.x >= wall_x and r.y >= wall_y
                  and (r.x + r.w) <= wall_x + wall_w
                  and (r.y + r.h) <= wall_y + wall_h

  if not covered then
    back:fill(r.x, r.y, r.w, r.h, desktop_colour())
  end

  if wallpaper then
    -- `blit` clips against both surfaces, so a rectangle that misses the
    -- picture copies nothing and one that runs off its edge stops there.
    back:blit(wall_filled or wallpaper, r.x - wall_x, r.y - wall_y, r.w, r.h, r.x, r.y)
  end

  -- What is running, bottom right, on the desktop and under everything
  -- else. Drawn as part of the composite rather than once at startup, so a
  -- window dragged over it and away again leaves it intact.
  --
  -- And only when this rectangle actually reaches that corner. The
  -- primitives clip to the backbuffer, so a rectangle in the top left was
  -- still paying for every glyph of it.
  -- Measured rather than counted: the interface font need not be monospaced,
  -- and a stamp positioned by character count would drift off the corner the
  -- moment it is not.
  -- Above the dock when there is one, which would otherwise sit on it.
  local sx, sy = W - gfx.measure(stamp) - 10, H - reserved_bottom - gfx.font.h - 8

  if r.x < W and r.x + r.w > sx and r.y < H and r.y + r.h > sy then
    back:text(sx, sy, stamp, stamp_colour(), desktop_colour())
  end

end

--------------------------------------------------------------------------
-- A menu bar above a window that draws its own pixels -
-- `/Kosmos/Libraries/wm/strips.lua` (`roadmap.md` 6zn).
--
-- `post` is declared above and given its body far below, so it is handed
-- as a function that asks for it when called: handed as it is here, it
-- would be nil.
local strips = use("/Kosmos/Libraries/wm/strips.lua"){
  back = back, theme = theme, add_damage = add_damage,
  post = function(win, event) return post(win, event) end,
}

-- One window, clipped - `/Kosmos/Libraries/wm/drawwindow.lua`
-- (`roadmap.md` 6zn). `resizable` is declared above and given its body
-- below, so it is handed as a function that asks for it when called.
--
local draw_window = use("/Kosmos/Libraries/wm/drawwindow.lua"){
  OUT = OUT, back = back, theme = theme, tabs = tabs, windows = windows,
  strips = strips, frame_of = frame_of, boxes_x = boxes_x,
  resizable = function(win) return resizable(win) end,
  focused_colour = focused_colour, idle_colour = idle_colour,
  title_colour = title_colour,
}

--------------------------------------------------------------------------
-- The level bar - `/Kosmos/Libraries/wm/osd.lua`, the volume and the
-- brightness shown over everything (`roadmap.md` 6zn).
--
local osd = use("/Kosmos/Libraries/wm/osd.lua"){
  width = W,
  add_damage = add_damage,
  reserved_top = function() return reserved_top end,
}

--------------------------------------------------------------------------
-- The screen, for somebody watching it (`vncd`; `roadmap.md` remote 7a).
--
-- **Asked of this process, which owns every pixel, and never read from the
-- framebuffer.** A watcher hands over a region the screen's size (`watch`);
-- after each rectangle is composed it is copied there too, and the
-- rectangle kept, and `watched` hands the list back and starts a new one.
-- Control by message, data by shared memory: the pixels never travel, and
-- what a message carries is where they changed.
--
-- One watcher, and nothing copied while there is none: a region not asked
-- about for five seconds is let go, which is also what happens when the
-- watcher dies. `vncd` watches again when a viewer comes.
--------------------------------------------------------------------------

local watcher = nil         -- { cap, surface, rects, asked }
local WATCH_RECTS = 32
local WATCH_LAPSE = 5 * COUNTER_HZ

local function watch_add(x, y, w, h)
  local list = watcher.rects

  -- A list without end is a reply that does not fit a message; past its
  -- length, one rectangle round everything.
  if #list >= WATCH_RECTS then
    local x0, y0, x1, y1 = x, y, x + w, y + h

    for _, r in ipairs(list) do
      x0, y0 = math.min(x0, r[1]), math.min(y0, r[2])
      x1, y1 = math.max(x1, r[1] + r[3]), math.max(y1, r[2] + r[4])
    end

    watcher.rects = { { x0, y0, x1 - x0, y1 - y0 } }
  else
    list[#list + 1] = { x, y, w, h }
  end
end

local function unwatch(why)
  if not watcher then return end

  -- The surface is a view of the region, not its owner; the release is
  -- what unmaps it, and nothing may draw into it after.
  watcher.surface = nil
  sys.release(watcher.cap)
  watcher = nil
  print("wm: the screen is no longer watched (" .. why .. ")")
end

-- After a rectangle is composed: into the watcher's copy as well.
local function mirror(r)
  if not watcher then return end

  watcher.surface:blit(back, r.x, r.y, r.w, r.h, r.x, r.y)
  watch_add(r.x, r.y, r.w, r.h)
end

-- Compositing - `/Kosmos/Libraries/wm/compose.lua` (`roadmap.md` 6zn).
--
local compose = use("/Kosmos/Libraries/wm/compose.lua"){
  OUT = OUT,
  OUTLINE = OUTLINE,
  P = P,
  PT = PT,
  back = back,
  damage = damage,
  mirror = mirror,
  draw_cursor = draw_cursor,
  draw_desktop = draw_desktop,
  draw_window = draw_window,
  focused_colour = focused_colour,
  frame_of = frame_of,
  menus = menus,
  osd = osd,
  screen = screen,
  tabs = tabs,
  windows = windows,
}

--------------------------------------------------------------------------
-- The protocol.
--------------------------------------------------------------------------

local function focused_window()
  -- Never a tip (`req.tip`), which is shown and nothing else: the window
  -- under it in the stack keeps the keys.
  for i = #windows, 1, -1 do
    if not windows[i].tip then return windows[i] end
  end

  return nil
end

--
-- Full screen, and back again.
--
-- `win.restore` holds where it was and is the flag as well as the record:
-- a window with one is maximised, and restoring clears it. Keeping the two
-- separate would let them disagree, which is how a maximised window ends up
-- restoring to its own maximised size and can never be got back.
--
--
-- The size a maximised window's contents have: the screen less the strip,
-- the title bar and the frame - and never more than `open` lets a window
-- be, whose room leaves eight pixels, where this leaves one border below.
-- The two disagreed by four rows, and a window that asked for this size
-- was quietly given less (26 September, Cafesa3D opening maximised).
--
--
-- **A window with no tab has the whole room below the strip** (`win.headed`):
-- no bar above it and no frame round it, so nothing to leave space for.
--
function OUT.maximised(win)
  if win and win.headed then return OUT.room(win) end

  return math.min(W - OUT.BORDER * 2, W - 8),
         math.min(H - top_limit() - OUT.BORDER, H - OUT.TAB_H - 8)
         - reserved_bottom
end

local function maximise(win)
  if not resizable(win) then return false end

  if win.restore then
    local r = win.restore

    win.restore = nil
    damage_window(win)
    win.x, win.y = r.x, r.y
    resize_window(win, r.w, r.h)
    post(win, { type = "moved", x = win.x, y = win.y })

    return true
  end

  --
  -- Moved first and *put back* if the resize will not happen.
  --
  -- The order used to be: record where it was, move it to the corner, then
  -- resize. When the resize failed - which it did, on the screen this was
  -- built for - the window was left in the corner at its old size with
  -- `win.restore` set, so it was small, in the wrong place, and convinced
  -- it was maximised. Nothing here may be half-applied.
  --
  local was = { x = win.x, y = win.y, w = win.w, h = win.h }

  damage_window(win)
  win.x, win.y = win.headed and 0 or OUT.BORDER, OUT.top_of(win)

  if not resize_window(win, OUT.maximised(win)) then
    win.x, win.y = was.x, was.y
    damage_window(win)

    return false
  end

  win.restore = was

  --
  -- **And told where it went**, as a drag tells it: a menu is a window on
  -- the screen, and one opened after a maximise opened where the window
  -- had been, since nothing said it had moved.
  --
  post(win, { type = "moved", x = win.x, y = win.y })

  return true
end

--
-- Out of sight, and the Deskbar is how it comes back.
--
-- Nothing else is needed: the Deskbar already lists every window and
-- already raises one by handle, so `raise` clearing this is the whole of
-- restore. A minimised window keeps its surface and its contents and is
-- simply not composed - an application that carries on drawing into it is
-- not interrupted, which is the same arrangement as a window buried under
-- another one.
--
local function minimise(win)
  if win.hidden then return false end

  win.hidden = true
  damage_window(win)

  -- Said, since nothing else on the screen but its absence says it.
  print(("wm: minimised %s"):format(tostring(win.title)))

  return true
end

--
-- Whatever a strip is currently claiming, recomputed rather than
-- decremented. A count that goes up on open and down on close is a count
-- that drifts the first time something closes twice or dies without saying
-- so - and an application dying without saying so is the ordinary case.
--
local fit_backdrop            -- below: the backdrop follows this number

local function recount_strips()
  local was_bottom = reserved_bottom

  reserved_top, reserved_bottom = 0, 0

  for _, w in ipairs(windows) do
    if w.strip == "top" and not w.hidden then
      reserved_top = w.h
    elseif w.strip == "bottom" and not w.hidden then
      reserved_bottom = bottom_room(w)
    end
  end

  bottom_changed(was_bottom)
  fit_backdrop()
end

--
-- A window given new pixels at another size, and told.
--
-- One function for a resize and for the backdrop following the strip,
-- which are the same operation asked for by different people.
--
local function swap_surface(win, w, h)
  -- Where it was, so the part it no longer covers is repainted.
  damage_window(win)

  --
  -- **One that draws its own pixels** (`roadmap.md` 6zz e): the frame takes
  -- its new size now, and the picture it has is shown stretched into it
  -- (`drawwindow`, which stretches whenever `src_w` is not the width) until
  -- the application hands over a region the new size and draws in it. It
  -- is told the size of its own pixels, in its points - the window less a
  -- menu bar - which is what it makes the region for.
  --
  if win.shared then
    win.w, win.h = w, h

    if win.menubar then
      win.menubar.surface:free()
      win.menubar.surface = gfx.surface{ w = w, h = win.menubar.h }
      strips.paint(win)
    end

    damage_window(win)
    post(win, { type = "resize", w = w, h = h - strips.below(win) })
    return true
  end

  --
  -- `pcall`, because `gfx.surface` *raises* when the kernel refuses the
  -- pages rather than returning nil - and this used to check for nil, which
  -- is a check that could never fire.
  --
  -- What that cost: maximising a window on a 1920x1080 screen asked for
  -- eight megabytes, the kernel said no, and the error went up through the
  -- compositor's main loop and killed the desktop. Every window on the
  -- screen went with it. A window that will not resize is a window that did
  -- not resize; it is not a reason to end the session.
  --
  local ok, fresh = pcall(gfx.surface, { w = w, h = h })

  if not ok or not fresh then
    print("wm: " .. tostring(fresh))
    return false
  end

  fresh:fill(0, 0, w, h, 0xff202020)

  win.surface:free()
  win.surface = fresh
  win.w, win.h = w, h

  damage_window(win)

  -- And the application, so it can lay out again. Queued like every other
  -- event: this process does not call applications.
  post(win, { type = "resize", w = w, h = h })

  return true
end

--
-- The backdrop is the screen less the strip, and follows the strip.
--
-- Sized when it opens from whatever the strip had claimed by then - but the
-- strip is an application like any other, and may start after the desktop
-- or stop. Either way the backdrop is moved and resized here and told, and
-- a desktop that draws its icons from its own top-left corner puts them
-- below the bar without having to know there is one.
--
-- **Told that it moved as well as that it resized**, which it was not until
-- 22 September. `swap_surface` posts a `resize` and nothing else, so a
-- desktop that started before the Deskbar kept `origin_y = 0` for ever -
-- and an origin is how a window works out where on the *screen* to put
-- something that is not inside it. Nothing had ever needed it, because the
-- only thing that does is a menu, and the desktop had no menu until the
-- icon sizes (`roadmap.md` 5za): the first one opened 32 pixels above the
-- pointer, over the Deskbar.
--
function fit_backdrop()
  for _, w in ipairs(windows) do
    if w.backdrop and (w.y ~= reserved_top or w.h ~= H - reserved_top) then
      local was = w.y

      damage_window(w)
      w.y = reserved_top

      if swap_surface(w, W, H - reserved_top) then
        print(("wm: the desktop is below the strip, at 0,%d %dx%d")
              :format(w.y, w.w, w.h))

        if w.y ~= was then post(w, { type = "moved", x = w.x, y = w.y }) end
      else
        w.y = was
      end
    end
  end
end

local function raise(win)
  --
  -- The backdrop is never raised. It is the thing windows are in front of,
  -- and a desktop that came to the front when you clicked it would hide
  -- everything you were working on - which is what clicking the desktop is
  -- for the opposite of.
  --
  if win.backdrop then return end

  -- Raising a minimised window is how it comes back, and the Deskbar is
  -- what does the raising. See `minimise`.
  if win.hidden then
    win.hidden = false
    damage_window(win)
  end

  if windows[#windows] == win then return end

  -- The window *losing* the focus is damaged as well as the one gaining it.
  --
  -- Its decoration is about to be drawn in a different colour, and this
  -- compositor only repaints what is damaged - so without this the old
  -- window keeps the focused colour until something else happens to cover
  -- it. Two windows both looking focused, which is the one thing the colour
  -- is there to tell you.
  --
  -- It was always wrong and was easy to miss while the tab was as narrow as
  -- its title. Painting the whole border made it obvious.
  local losing = windows[#windows]

  for i, w_ in ipairs(windows) do
    if w_ == win then
      table.remove(windows, i)
      break
    end
  end

  windows[#windows + 1] = win

  --
  -- **After the list is whole again, and that is the whole fix.**
  --
  -- This ran between the `remove` and the append, so it counted the strips
  -- in a list the window had just been taken out of. Raising the strip
  -- itself - which a click on the Deskbar does, because `pointer_pass`
  -- raises whatever is under the pointer before it asks what kind of window
  -- it is - therefore found *no* strip, set `reserved_top` to zero, and
  -- resized the backdrop to the whole screen. Putting it back resized it
  -- again.
  --
  -- So every click on the bar reallocated and repainted an 8 MB desktop
  -- surface twice. On a ThinkPad that is a visible flicker across the whole
  -- screen; under QEMU it hides in the noise, which is why the gate is
  -- green and always was.
  --
  -- Raising cannot change *which* strips exist, so this is only here to
  -- recompute an ordering-dependent answer over a complete list.
  --
  recount_strips()
  damage_window(win)

  if losing then
    damage_window(losing)
  end
end

local handlers = {}

--
-- **What this process lends: the desktop, to one who works it from
-- elsewhere** (`vncd`; `roadmap.md` remote 7b - Diego, 30 September: "yes
-- agreee"). Reading every window's pixels and typing into every window are
-- authorities, and `/Running/wm` is a name every process can reach, where
-- nothing a request carries is checked. So they are not requests there:
-- they are an endpoint of their own, mounted as `/Running/wm/remote` in a
-- program this process launches whose header says `kosmos: needs
-- desktop`, and in nothing else. What you were not handed, you cannot
-- reach. Whether a viewer may do more than look is the Servers window's
-- setting, which `vncd` keeps; holding this is what makes it possible at
-- all.
--
local remote = { ep = sys.endpoint(), handlers = {}, moved = false,
                 physical = { x = -1, y = -1, buttons = -1 } }

--
-- `watch`: without a region, the size one must be; with one, the screen
-- copied into it from now on - the whole of it at once, so the watcher
-- starts from a complete picture. **The region is held to its size** before
-- anything is wrapped over it: a region smaller than it says would have this
-- process write past its mapping, and a fault here is every window's.
--
remote.handlers.watch = function(req, who, cap)
  local bytes = gfx.bytes(W, H)

  if not cap or cap < 0 then
    return { ok = true, w = W, h = H, bytes = bytes }
  end

  local function refuse(text)
    sys.release(cap)
    return { ok = false, error = text, w = W, h = H, bytes = bytes }
  end

  local pages = sys.memory_size(cap)

  if not pages or pages * 4096 < bytes then
    return refuse(("the screen is %dx%d, %d bytes, and that region holds %d")
                  :format(W, H, bytes, (pages or 0) * 4096))
  end

  local at, why = sys.memory_map(cap)

  if not at then return refuse("could not map it: " .. tostring(why)) end

  unwatch("another asked")
  watcher = { cap = cap, surface = gfx.wrap{ at = at, w = W, h = H },
              rects = {}, asked = sys.ticks() }
  watcher.surface:blit(back, 0, 0, W, H, 0, 0)
  watch_add(0, 0, W, H)
  print(("wm: the screen is watched, %dx%d"):format(W, H))

  return { ok = true, w = W, h = H }
end

--
-- `watched`: the rectangles since last asked, eight bytes each - x, y, w, h
-- as big-endian sixteen-bit numbers - and a new list begun.
--
remote.handlers.watched = function()
  if not watcher then return { ok = false, error = "nothing is watched" } end

  local parts = {}

  for i, r in ipairs(watcher.rects) do
    parts[i] = string.pack(">I2I2I2I2", r[1], r[2], r[3], r[4])
  end

  watcher.rects = {}
  watcher.asked = sys.ticks()

  return { ok = true, rects = table.concat(parts) }
end

--
-- **Recently used** (`docs/launcher.html`, agreed 8 October): an
-- application whose window opens here goes to the top of the settings kit's `recent`,
-- which the launcher's Recently used row shows. Here because this is the
-- one place every window passes, whoever started it, and `recent.lua` is
-- the one door to the list. Two small requests to `/Home` on a process's
-- first window, the same disk its program was just read from; a `/Home`
-- that refuses (none, or read only) costs the list and nothing else.
--
local recent = use("/Kosmos/Libraries/recent.lua")

--
-- **When it opens a window a person looks at**, not when it is started:
-- the desktop is Tracker started with `desktop`, and counting starts put a
-- second Tracker in the list on every boot (Diego, 8 October: "tracker
-- appears twice in the recently used apps"). So a process's first window
-- that is not a backdrop, a strip, a popup, a menu or a tip records its
-- program - whoever started it, the launcher or a Terminal - and an
-- application is kept once, by its program, without the words it was
-- started with.
--
local function remember(win)
  if not win.program or win.kind == "menu" or win.backdrop or win.strip
     or win.popup or win.tip or win.banner then
    return
  end

  -- Its process's first: a second window of an open application is not
  -- another start. Asked before this one joins `windows`.
  for _, other in ipairs(windows) do
    if other.pid == win.pid and other.program == win.program
       and not (other.backdrop or other.strip or other.popup or other.tip) then
      return
    end
  end

  if not recent.counts(fs.getattr(win.program)) then return end

  local list = recent.add(recent.parse(prefs.read(recent.NAME)), win.program)
  local ok, why = prefs.write(recent.NAME, recent.format(list))

  if not ok then
    print(("wm: recently used not kept: %s"):format(tostring(why)))
  end
end

--------------------------------------------------------------------------
-- A window whose pixels the application draws itself.
--
-- `gfx.md` 19.4. Everything else here sends drawing *commands* and this
-- process owns every pixel, which is what lets a hung application keep a
-- window. That is right for a window of widgets and wrong for a video
-- frame or a rendered scene, where the pixels change wholesale thirty
-- times a second and describing them costs more than copying them.
--
-- So an application may hand over a region of memory instead. It draws into
-- that region with the same C primitives everything else uses, and says
-- when it is finished.
--
-- **Two buffers and an index, because there are no locks.** If the
-- application wrote the buffer while this process read it there would be
-- tearing and nothing to prevent it - `design.md` 6 has no locks and is not
-- getting any. So the region holds two, the application draws into the one
-- that is not being shown, and `commit` swaps which is which. Neither side
-- ever touches the buffer the other is using.
--
-- **Damage is part of the commit, not a separate call.** Without it this
-- process would have to blit the whole surface every frame, which is the
-- cost the whole arrangement exists to avoid. Making it a field of the
-- message rather than another message makes it hard to forget.
--
-- **An application that never commits is composed from its last frame.**
-- The hung-window property is unchanged: nothing here waits.
--------------------------------------------------------------------------

handlers.open = function(req, who, cap)
  --
  -- Room for a window, which is the screen less its own decoration.
  --
  -- A strip is not a window and gets neither reduction: it is exactly as
  -- wide as the screen and sits at the very top. The eight pixels this
  -- takes off an ordinary window are the border either side, and a bar
  -- four pixels short at each end has a dark stripe down its right-hand
  -- edge where the desktop shows past it - which is what happened, and
  -- read as a border because a border is what it looked like.
  --
  --
  -- **Points in, the screen's pixels from here** (`ui.md` 16.18). Every
  -- size and place asked for is multiplied by the window's factor before
  -- anything else looks at it, and the reply divides back. A full-screen
  -- window's factor is one: it asked for the screen in its own pixels.
  --
  local pct = req.fullscreen and 100 or scale.pct
  local asked_w, asked_h = tonumber(req.w), tonumber(req.h)

  if pct ~= 100 then
    local function px(v)
      v = tonumber(v)
      return v and scale.px(v, pct) or nil
    end

    req.w, req.h, req.x, req.y = px(req.w), px(req.h), px(req.x), px(req.y)
  end

  -- What was given, in points: exactly what was asked for when it fitted,
  -- rather than a rounding of it back.
  local function given(v, asked)
    return (asked and v == scale.px(asked, pct)) and asked or scale.pt(v, pct)
  end

  local strip = req.strip == "top" or req.strip == "bottom"
  local room_w = strip and W or (W - 8)
  local room_h = strip and H or (H - OUT.TAB_H - 8 - reserved_bottom)

  --
  -- The floor of 32 is so a window cannot be smaller than its own
  -- decoration. A strip has none, so it does not apply: it was rounding a
  -- 26-pixel bar up to 32, the application went on drawing 26 rows, and the
  -- six rows nobody painted showed as a dark line under the bar. Which read
  -- as a border, and was chased three times as one.
  --
  local floor = strip and 1 or 32

  --
  -- **A maximised window whose header will be its title bar** has no tab
  -- and no border to leave room for (`OUT.room`): the screen's width, and
  -- down from the strip - which is what `workarea` told it when it asked
  -- with its header.
  --
  if req.maximised and OUT.wants_head(req) and theme.title_bars == false then
    room_w, room_h = W, H - reserved_top - reserved_bottom
  end

  local w_ = math.min(math.max(tonumber(req.w) or 320, floor), room_w)
  local h_ = math.min(math.max(tonumber(req.h) or 200, floor), room_h)

  --
  -- The backdrop is whatever the strip leaves, whatever it asked for.
  --
  -- It asks for the screen because the screen is all it knows, and a
  -- desktop that believed it was the whole screen drew its first row of
  -- icons under the bar. So it is sized here from the one number that says
  -- how much the strip took, and `fit_backdrop` keeps it so when the strip
  -- starts after it or goes away.
  --
  if req.backdrop then
    w_, h_ = W, H - reserved_top
  end

  --
  -- **And a full-screen window is the screen**, which `room_w` and `room_h`
  -- are not: those leave space for a border and a tab that this window does
  -- not have, so the clamp above handed back 1912 by 1072 for a screen of
  -- 1920 by 1080. Eight pixels of nothing at two edges is exactly the sort
  -- of thing that reads as a border and gets chased as one - the strip's
  -- own comment, six lines up, is about the same mistake.
  --
  -- It matters more here than it would for an ordinary window: this one
  -- draws its own pixels, so it has *already* made buffers the size of the
  -- screen, and a window manager that quietly gave it a smaller rectangle
  -- would be showing a part of each frame.
  --
  --
  -- **And it is refused if the application has not done its half.**
  --
  -- The window becomes the screen, and a window that draws its own pixels
  -- composites straight out of the region it allocated - so a program that
  -- asks for full screen having made a smaller region has just told this
  -- process to read past the end of it. On 21 September that is exactly
  -- what happened: `solar --full` asked for 960x540, got a 1024x768
  -- window, and the *desktop* died with a translation fault reading
  -- 0x1803f5000. The application's mistake killed the window manager,
  -- which is the one outcome a server may never allow.
  --
  -- So the mismatch is caught here, where both numbers are known, and the
  -- open is refused with a sentence saying which two disagree. The comment
  -- above already stated the contract - "it has *already* made buffers the
  -- size of the screen" - and an unchecked contract is a wish.
  --
  if req.fullscreen then
    local rw, rh = tonumber(req.w) or 0, tonumber(req.h) or 0

    if rw < W or rh < H then
      return { ok = false, error = ("full screen needs a %dx%d window; "
                                    .. "this one asked for %dx%d")
                                   :format(W, H, rw, rh) }
    end

    w_, h_ = W, H
  end

  local win = {
    handle  = next_handle,
    title   = tostring(req.title or "window"),
    --
    -- Where it opens. `centre` wins over any x and y the application also
    -- sent, because the two cannot both be honoured and only one of them
    -- was a decision - `x` and `y` in an application's source are a guess
    -- made without knowing the screen size.
    --
    -- The vertical middle is measured from `top_limit()` rather than from
    -- zero, so a centred window sits in the middle of the room that is
    -- actually available rather than half a strip too high.
    --
    x       = req.centre
              and math.max(OUT.BORDER, (W - w_) // 2)
              or math.min(math.max(tonumber(req.x) or 40, OUT.BORDER),
                          W - w_ - OUT.BORDER),
    y       = req.centre
              and math.max(top_limit(),
                           top_limit() + (H - top_limit() - h_) // 2)
              or math.min(math.max(tonumber(req.y) or 40, top_limit()),
                          H - h_ - OUT.BORDER),
    w       = w_,
    h       = h_,
    pct     = pct,
    surface = gfx.surface{ w = w_, h = h_ },
    events  = {},

    --
    -- **The silence clock starts when the window opens, not at its first
    -- poll**, and that distinction is a bug this cost.
    --
    -- `collect_dead` reaps a window whose application has gone quiet *and*
    -- whose process is gone. It guards on `w.last_poll` being set, and
    -- `last_poll` was only ever assigned inside the poll handler - so an
    -- application that died before it ever polled had `last_poll` nil, the
    -- guard short-circuited, and its window was exempt from collection for
    -- ever. Which is precisely the case worth catching: dying during the
    -- first paint is the most likely moment for an application to die.
    --
    -- On the first real machine that produced four windows drawn once and
    -- then abandoned, no polls at all, and a compositor cycling happily
    -- underneath - a desktop that looked wedged while nothing was wrong
    -- with it, because the corpses were unreapable.
    --
    last_poll = sys.ticks(),
  }

  win.surface:fill(0, 0, w_, h_, 0xff202020)

  --
  -- A shared region came with the request, so this window's contents are
  -- the application's own memory rather than a surface this process draws
  -- into. Two buffers in one region, and `live` says which one is being
  -- shown - `gfx.md` 19.4.
  --
  --
  -- **The surface is the size the application made it**, in its points:
  -- what it asked for, and not the window's size after the clamp above. It
  -- is composed stretched to its place (`src_w`, `src_h`). This was the
  -- clamped size, and the application had made its buffers at the asked
  -- one and drew into them at it: every row read short by the difference,
  -- and Groove, asking 1720 in a look with no borders and given 1712, came
  -- out in diagonal bands on the M700 (`testing.md` 18.355). The kit makes
  -- its buffers again at the granted size once it is told it, and this side
  -- reads them at that size from the first frame drawn in them.
  --
  local src_w = math.tointeger(asked_w) or given(w_, asked_w)
  local src_h = math.tointeger(asked_h) or given(h_, asked_h)

  --
  -- **And a region too small for that is refused, not read past.** A
  -- window asking for less than the floor of 32 was given 32, and its
  -- buffers read at 32 - beyond the end of a region made for less; the
  -- full-screen case above was caught on 21 September for exactly that,
  -- after it killed the desktop, and this one never was.
  --
  if cap and cap >= 0
     and (sys.memory_size(cap) or 0) * 4096 < gfx.bytes(src_w, src_h) * 2 then
    print(("wm: %s's region does not hold two %dx%d pictures, so it is not "
           .. "shown"):format(tostring(req.title), src_w, src_h))
    sys.release(cap)
    cap = nil
  end

  if cap and cap >= 0 then
    local at, why = sys.memory_map(cap)

    if at then
      local bytes = gfx.bytes(src_w, src_h)

      win.src_w, win.src_h = src_w, src_h
      win.shared = {
        cap = cap,
        [1] = gfx.wrap{ at = at, w = src_w, h = src_h },
        [2] = gfx.wrap{ at = at + bytes, w = src_w, h = src_h },
        live = 1,
      }

      -- And whether it can make a region at another size when told one
      -- (`handlers.surface`, `roadmap.md` 6zz e): it asked, so it gets a
      -- grip.
      win.resizes_itself = req.resizable == true
    else
      -- Said rather than silently ignored. A window that quietly refuses a
      -- shared surface is a window that opens, stays blank, and gives the
      -- application no idea why - which is how this arrived the first time.
      print("wm: could not map a shared surface: " .. tostring(why))
    end
  end

  -- And a menu bar above it, when one was asked for (`strips`).
  strips.accept(win, req.menubar)

  --
  -- **A header that can be the title bar** (`roadmap.md` 6zj): the window
  -- says it has one, and the look decides whether it is - Plex's are,
  -- Classic keeps its tab (`OUT.headed`). Which windows may say so is
  -- `OUT.wants_head`.
  --
  win.can_head = OUT.wants_head(req) or nil
  win.headed = OUT.headed(win)

  --
  -- Cascaded, if something is already there.
  --
  -- Applications carry a position in their source and several of them were
  -- written to the same corner, so opening four of them put four windows
  -- exactly on top of each other with only the last one reachable. Every
  -- desktop since the Macintosh has stepped each new window down and across
  -- instead, and the reason is not tidiness: a window nobody can see is a
  -- window nobody can move, because the one underneath cannot be clicked.
  --
  -- Only when the corner is actually taken, so an application that asks for
  -- somewhere free is put exactly where it asked. And it gives up after a
  -- few steps rather than walking off the screen - past that, landing on
  -- top of something is better than landing outside.
  --
  -- Scenery places itself: the backdrop and the strip are pinned to 0,0 a
  -- few lines below, so searching for somewhere free for them is work
  -- whose answer is thrown away.
  --
  -- **A maximised window is placed, not searched for**: at the corner
  -- maximise uses, over whatever is there, which is the point of it.
  --
  if req.maximised and not req.fullscreen and not req.backdrop and req.strip ~= "top" then
    win.x, win.y = win.headed and 0 or OUT.BORDER, OUT.top_of(win)
  end

  if req.kind ~= "menu" and not req.backdrop and req.strip ~= "top"
     and not req.centre and not req.maximised and not req.popup and not req.tip then
    --
    -- **Taken means hidden, not "in the same spot".**
    --
    -- This compared origins: two windows counted as colliding only if their
    -- top-left corners were within a title bar of each other. That is the
    -- right question for a cascade and the wrong one for a screen. Two
    -- 850-pixel windows whose origins are 110 apart pass it and cover each
    -- other by seven hundred pixels - which is what four applications
    -- opening at login actually looked like: a pile, every one of them
    -- technically in a different place and none of them readable.
    --
    -- So the test is how much of the *new* window would be buried. Over
    -- half, and it goes somewhere else. That is the thing a person means by
    -- "I cannot see it", and it is the same rule whichever window sizes an
    -- application happens to ask for.
    --
    -- **The backdrop and the menu strip are not in the way.** Tracker's
    -- desktop window is the whole screen and lives under everything by
    -- construction, so counting it would make every position on the machine
    -- "taken" and this whole search a no-op that quietly fell through to the
    -- cascade - which is exactly what it did the first time it ran. The
    -- same flags the window cycle uses (`wm/keys.lua`), for the same
    -- reason: these are scenery rather than windows you are being hidden
    -- behind.
    local function taken_at(x, y)
      for _, other in ipairs(windows) do
        if not (other.backdrop or other.strip or other.kind == "menu" or other.popup
                or other.tip) then
          local ox = math.min(x + win.w, other.x + other.w) - math.max(x, other.x)
          local oy = math.min(y + win.h, other.y + other.h) - math.max(y, other.y)

          --
          -- **Only the new window, and deliberately not the other one.**
          --
          -- Asking "would either be buried" reads better and is wrong: the
          -- Deskbar was then a 210x266 panel, so *any* large window covered
          -- more than half of it, and every window on the machine started
          -- jumping into a quarter to avoid a panel it was perfectly
          -- entitled to overlap. That moved windows the display harness
          -- clicks on, which is how it was caught.
          --
          -- The case symmetry was meant to fix - a large window landing
          -- exactly on a small one - is handled where it belongs, in the
          -- quarter search below: a quarter already holding a window is not
          -- offered to the next one.
          --
          -- A third rather than a half, and the difference is a real
          -- window: the log is 620x420 and Tracker is 780x520, so opening
          -- one over the other buries 48% of it - under a half by two
          -- points, and unreadable by any standard a person would use.
          -- A third is about where "I cannot see it" starts.
          if ox > 0 and oy > 0 and ox * oy * 3 > win.w * win.h then
            return true
          end
        end
      end

      return false
    end

    if taken_at(win.x, win.y) then
      --
      -- **The quarters first, and the cascade only when they are gone.**
      --
      -- A cascade steps by `OUT.cascade`, which is a title bar and a little -
      -- about thirty pixels. That is the right amount to prove two windows
      -- are not the same window, and it is nowhere near enough to *read*
      -- the one underneath: four windows opened at login came up in a stack
      -- with an inch of each showing, which on a 1920x1080 panel is a
      -- desktop mostly made of wallpaper with everything piled in one
      -- corner of it.
      --
      -- The login set is what made this matter. One window at a time,
      -- opened by hand, wants to land near the last one; four opened at
      -- once want to be *visible*, which means using the screen there
      -- already is. So the free quarters are tried first and the cascade is
      -- what happens when they are used up.
      --
      -- Bottom-left before top-right, which was on purpose while the
      -- Deskbar was a panel in the top right corner and that quarter the
      -- one most likely to be spoken for. It is a strip or a dock now, which
      -- no search here counts, and the order is what it was.
      --
      local top = top_limit()
      local midx = OUT.BORDER + (W - OUT.BORDER * 2) // 2
      local midy = top + (H - OUT.BORDER - top) // 2

      local placed = false

      --
      -- **In a slot, the question is whether anybody gets buried - either
      -- way round.**
      --
      -- The trigger above asks only about the new window, because a window
      -- is entitled to overlap a panel. Here the opposite case matters:
      -- Processes is 850x482 and Monitor is 380x112, so dropping Processes
      -- on Monitor's slot covers Monitor completely while leaving nine
      -- tenths of Processes showing - not "buried" by the trigger's rule,
      -- and plainly wrong on a screen.
      --
      -- This was a test of whether the *quarter* was occupied, which is
      -- simpler and threw away a quarter of the screen: the Deskbar was
      -- then a 210x266 panel in the top right, and counting it as the owner
      -- of that whole quarter sent every window that would have fitted
      -- beside it into the cascade instead.
      --
      local function slot_ok(x, y)
        for _, other in ipairs(windows) do
          if not (other.backdrop or other.strip or other.kind == "menu" or other.popup
                or other.tip) then
            local ox = math.min(x + win.w, other.x + other.w) - math.max(x, other.x)
            local oy = math.min(y + win.h, other.y + other.h) - math.max(y, other.y)

            if ox > 0 and oy > 0
               and (ox * oy * 3 > win.w * win.h
                    or ox * oy * 3 > other.w * other.h) then
              return false
            end
          end
        end

        return true
      end

      for _, slot in ipairs({ { OUT.BORDER, top }, { OUT.BORDER, midy },
                              { midx, midy }, { midx, top } }) do
        -- Pulled back to fit rather than skipped: a window taller than half
        -- the screen still belongs in the left half of it.
        local x = math.min(slot[1], W - OUT.BORDER - win.w)
        local y = math.min(slot[2], H - OUT.BORDER - win.h)

        if x >= OUT.BORDER and y >= top and slot_ok(x, y) then
          win.x, win.y = x, y
          placed = true
          break
        end
      end

      if not placed then
        for _ = 1, 8 do
          if not taken_at(win.x, win.y) then break end

          win.x = win.x + OUT.cascade
          win.y = win.y + OUT.cascade

          -- Back to the top left rather than off the bottom right.
          if win.x + win.w > W - OUT.BORDER or win.y + win.h > H - OUT.BORDER then
            win.x, win.y = OUT.BORDER + OUT.cascade, top + OUT.cascade
            break
          end
        end
      end
    end
  end

  --
  -- Whether anything may be dropped on it, which decides one thing only:
  -- whether this process outlines it while a drag is overhead.
  --
  -- The drop itself is posted regardless and a window that does not listen
  -- ignores it, exactly as it ignores any event it has no handler for. What
  -- the flag prevents is a *promise*: an outline around a window that is
  -- going to do nothing with what lands on it is the interface lying, and
  -- the person only finds out after letting go.
  --
  win.drops = req.drops and true or nil

  --
  -- Whether the *right* button reaches it at all.
  --
  -- Off unless asked for, and that is the whole design. Every other event
  -- this process posts is one an application may ignore safely; a right
  -- press is not, because an application that has never heard of one reads
  -- it as a left press and acts on it. Paint would draw with it and Quake
  -- would fire - programs that handle `mouse` themselves rather than
  -- through the kit, and neither wrong to. (Lite XL would have moved its
  -- cursor, when it was in the tree.)
  --
  -- The alternative was a guard in each of them, and CLAUDE.md has already
  -- paid for that lesson: a rule that requires you to recognise a third
  -- case will miss the fourth. The fourth program to read `mouse` directly
  -- would be written without the guard, and right-clicking it would fire a
  -- weapon.
  --
  -- So this is the capability argument one layer up, and the same sentence
  -- as `drops` above: what you were not handed, you do not have. A window
  -- that never asked cannot receive one, so it cannot misread one.
  --
  -- **What it asserts is "I understand the `button` field"**, rather than
  -- "I want a menu". That is the distinction that makes it free: `ui.lua`
  -- sets it on every window it opens, because the kit reads `button` and
  -- drops what no widget claimed, so no application using the kit has to
  -- know this exists. The three that read `mouse` themselves never set it,
  -- and are the three it protects.
  --
  win.context = req.context and true or nil

  --
  -- The backdrop: the window everything else sits on.
  --
  -- BeOS's desktop was a Tracker window - borderless, screen-sized, at the
  -- bottom of the stack - and that is why BeOS had no separate desktop
  -- program. It is the right shape here for a stronger reason: this process
  -- knows nothing about files, and drawing icons would mean learning what a
  -- directory is, what a file type is, and how to start a program. Tracker
  -- knows all three already.
  --
  -- So the compositor gains no knowledge, only a place to put a window.
  --
  -- **Pinned, as the strips below are**: part of the desktop rather than a
  -- thing running on it, so no close box, no minimise and no maximise -
  -- and never by asking, since an application's window is not scenery. The
  -- reason is not tidiness: the Deskbar is *how you get a window back*.
  -- Minimising it hid the one thing that restores hidden windows, and
  -- closing it took away the only way to start anything. BeOS's Deskbar
  -- had none of those controls either, for the same reason. A control
  -- that must never be pressed should not be drawn, which is the same
  -- argument as not drawing a grip on a window that cannot resize.
  --
  if req.backdrop then
    win.backdrop = true
    win.pinned = true
    win.x, win.y = 0, reserved_top
  end

  --
  -- A strip across the top: the menu bar's place on a Macintosh, and the
  -- opposite end of the same idea as the backdrop. Undecorated, screen-wide,
  -- pinned, and - unlike everything else - it takes room away from the rest
  -- of the screen rather than sitting over it.
  --
  -- That last part is what makes it worth a flag instead of a well-behaved
  -- application putting itself at the top. A window placed at y=0 is a
  -- window every other window opens underneath.
  --
  if req.strip == "top" then
    win.strip = "top"
    win.pinned = true
    win.x, win.y = 0, 0
    reserved_top = win.h
    fit_backdrop()
  end

  --
  -- **And one across the foot of the screen**: the dock (`roadmap.md`, a dock
  -- at the bottom), centred and floating above the edge unless it asks for
  -- the whole width. Pinned and undecorated as the top strip is, and in
  -- front of every window (`OUT.order`) rather than at the top of the
  -- stack, which is the focus.
  --
  if req.strip == "bottom" then
    win.strip = "bottom"
    win.pinned = true
    win.floating = req.floating == true or nil
    place_bottom(win)
    -- Set here, as the top strip's is: the window is not in `windows` yet,
    -- so counting them would not find it - which is what a maximised
    -- window opened after the dock was given, the whole height.
    local was = reserved_bottom

    reserved_bottom = bottom_room(win)
    bottom_changed(was)
    fit_backdrop()
  end

  --
  -- **A strip may be blended**, as the backdrop always is: what it has not
  -- drawn shows what is behind it. The dock's strip at the top is the time
  -- and the indicators over the wallpaper, and the dock a pill with the
  -- screen round it. Strips only, since blending costs more than a copy and
  -- an application window has no reason to.
  --
  if req.blend == true and win.strip then win.blend = true end

  --
  -- **Full screen: the window *is* the screen.**
  --
  -- Diego, 20 September: "the video player needs a switch to fullscreen
  -- mode, as most video players do". It is here rather than in the player
  -- because every application that draws its own pixels wants the same
  -- thing - Doom, Quake, the Super Nintendo, the browser - and because
  -- `maximise` cannot serve them: it resizes, and a window that draws its
  -- own pixels cannot be resized, its buffers being a region the
  -- application allocated for exactly these dimensions.
  --
  -- So an application asks for it when it opens the window, having made its
  -- buffers the size of the screen. No tab, no border, at the origin, and
  -- above everything including the strip - which is what "full screen"
  -- means and is three lines because `frame_of` and the compositor already
  -- know how to treat a window with no chrome.
  --
  if req.fullscreen then
    win.fullscreen = true
    win.x, win.y = 0, 0
  end

  --
  -- **A popup** (`ui.window{ popup = true }`): put where it asked, pulled
  -- back only onto the screen, as a menu is - the dock's launcher above the
  -- dock, quick settings under the strip. Chrome rather than something
  -- running, so the Deskbar lists none; closed by a press outside it
  -- (`OUT.popup`, `pointer.lua`).
  --
  if req.popup == true then
    win.popup = true
    win.x = math.min(math.max(tonumber(req.x) or 0, 0), W - w_)
    win.y = math.min(math.max(tonumber(req.y) or 0, 0), H - h_)
  end

  --
  -- **A tip** (`ui.window{ tip = true }`): a name over a dock's icon, shown
  -- and nothing else - never pressed, never focused, never closed by a press
  -- elsewhere, no frame and no shadow; blended, so what it does not draw is
  -- the screen (macOS's, which Diego showed: a dark pill and an arrow down
  -- to the icon). Put where it asked, onto the screen.
  --
  if req.tip == true then
    win.tip = true
    win.blend = true
    win.x = math.min(math.max(tonumber(req.x) or 0, 0), W - w_)
    win.y = math.min(math.max(tonumber(req.y) or 0, 0), H - h_)
  end

  --
  -- **A banner** (`ui.window{ banner = true }`, `roadmap.md`,
  -- *Notifications*): a tip that takes a press. Everything a tip is - never
  -- focused, so a notification arriving while somebody types takes nothing
  -- from them; no frame, no shadow, not listed, not minimised; blended, so
  -- it draws its own rounded cards and its own shadow - and pressed as a
  -- popup is, straight to the application. In front of every window and
  -- the dock (`OUT.order`), and found first by a press (`window_at`).
  --
  if req.banner == true then
    win.tip = true
    win.banner = true
    win.blend = true
    win.x = math.min(math.max(tonumber(req.x) or 0, 0), W - w_)
    win.y = math.min(math.max(tonumber(req.y) or 0, 0), H - h_)
  end

  --
  -- A menu rather than a window, and the difference is only in where it is
  -- kept: no decoration, above everything, placed exactly where it was
  -- asked for rather than clamped into the workspace, and it never takes
  -- the focus.
  --
  -- `owner` is the window it belongs to. Its events go to that window's
  -- queue, so an application polls one handle and gets everything - which
  -- is what keeps `ui.window`'s loop a loop rather than two.
  --
  if req.kind == "menu" then
    win.kind = "menu"
    win.owner = tonumber(req.owner)

    -- Placed where it was asked, only pulled back far enough to be on the
    -- screen. A menu under the pointer is the whole point of a menu.
    win.x = math.min(math.max(tonumber(req.x) or 0, 0), W - w_)
    win.y = math.min(math.max(tonumber(req.y) or 0, 0), H - h_)
  end

  -- Where it ended up, which is the other half of "started": a program that
  -- started and never opened a window is a different fault from one whose
  -- window landed under another, and the log has to tell them apart on a
  -- machine nobody can see.
  --
  -- After the backdrop and strip are pinned rather than before, because
  -- before is where the search left them and not where they end up - which
  -- made this line report a desktop at 30,48 that was about to be moved to
  -- the origin. A diagnostic that prints a number nothing else will ever
  -- use is worse than none.
  --
  -- And after a menu is put where it was asked, for the same reason. A menu
  -- is named as its window's, because it has no title of its own: the
  -- Deskbar's menu opening used to read `wm: window window at 812,74`, and
  -- on a machine where clicks were the thing in question that was the one
  -- line that could have said a click had worked.
  if win.kind == "menu" then
    local owner = by_handle[win.owner]

    print(("wm: menu of %s at %d,%d %dx%d"):format(
          tostring(owner and owner.title), win.x, win.y, win.w, win.h))
  elseif win.backdrop or win.strip or win.popup or win.tip then
    print(("wm: window %s at %d,%d %dx%d"):format(
          tostring(win.title), win.x, win.y, win.w, win.h))
  elseif win.headed then
    -- No tab to be wide: its header is the title bar, and the log says so
    -- where the others give the tab's width.
    print(("wm: window %s at %d,%d %dx%d, its header the title bar"):format(
          tostring(win.title), win.x, win.y, win.w, win.h))
  else
    -- And how wide its tab is, which the title's font decides (`tabs`): a
    -- harness pressing the minimise box at the tab's end is told where
    -- that is rather than working out a font's metrics.
    print(("wm: window %s at %d,%d %dx%d, a tab %d wide"):format(
          tostring(win.title), win.x, win.y, win.w, win.h, tabs.width(win)))
  end

  --
  -- **Whose window it is, as the kernel says** (`SYS_SENDER`, `roadmap.md`
  -- *Notifications*): the process this `open` came from. It was whoever
  -- was launched most recently - a guess, written down as one, that a
  -- window opened by anything else in between took for its own: a banner
  -- arriving while an application started would have been given that
  -- application's process, and closing one could end the other. The guess
  -- stays only for a kernel that cannot say.
  --
  local opener = sys.sender()

  win.pid = (opener and opener.id) or pending_pid

  if not opener or opener.id == pending_pid then pending_pid = nil end

  --
  -- And what was started to produce it. The Deskbar draws a button per
  -- window and wants the application's picture on it, and a *title* cannot
  -- give it one - a title is a sentence the application chose and changes
  -- whenever it likes. The program is a path, and a path has a
  -- `kosmos: icon` line at the end of it.
  --
  -- **The window's own word first** (`roadmap.md` 6r): the kit sends the
  -- file its process runs, which is right however the process was started
  -- - from a Terminal, by Tracker, by the IDE's Run, at startup - where
  -- the launch's guess was right only for what this process launched, so
  -- every other window had the generic picture. A plain path or nothing:
  -- all it can do if it lies is show another program's picture. The
  -- launch's guess stays for a window that says nothing - one opened
  -- without the kit - and nil is a window neither names: the generic one.
  local said = type(req.program) == "string" and #req.program < 128
               and req.program:match("^/[^%c]+$") and req.program

  win.program = said or pending_program
  pending_program = nil

  -- Its first window: no longer starting.
  if win.program then
    stop_starting(function(s) return s.path == win.program end)
  end

  remember(win)

  next_handle = next_handle + 1

  by_handle[win.handle] = win

  if win.kind == "menu" then
    --
    -- Into the menu list, and it takes no focus: the window that owns it
    -- keeps the focus while it is up, which is what makes a menu feel like
    -- part of the window it came from rather than a window of its own.
    --
    menus[#menus + 1] = win
    damage_window(win)
  elseif win.backdrop then
    -- At the bottom, and it does not take the focus: `focused_window` is
    -- `windows[#windows]`, and the backdrop is never last.
    table.insert(windows, 1, win)
    damage_window(win)
  else
    -- The window that had the focus loses it to this one, and has to be
    -- repainted to say so. Same reason as `raise`: only damage is redrawn.
    local losing = windows[#windows]

    windows[#windows + 1] = win
    damage_window(win)

    if losing then
      damage_window(losing)
    end
  end

  -- The appearance in force, in the reply that creates the window.
  --
  -- An application loads `theme.lua`, which defaults to dark, and had no
  -- way to learn that the desktop is currently light - so every new window
  -- opened dark and only became light when somebody changed the theme
  -- *again*. Telling it here rather than posting an event means it knows
  -- before its first paint, so there is no flash of the wrong colours.
  -- `x` and `y` go back as well as `w` and `h`. A menu asks to appear at a
  -- particular place on the screen and may have been pulled back to fit, and
  -- a caller that does not know where its menu ended up cannot hit-test it.
  --
  -- In the window's points, and the screen's size in them as well: at a
  -- scale the framebuffer's size is not the room an application has.
  --
  return { ok = true, window = win.handle,
           w = given(w_, asked_w), h = given(h_, asked_h),
           x = scale.pt(win.x, pct), y = scale.pt(win.y, pct),
           screen_w = scale.pt(W, pct), screen_h = scale.pt(H, pct),
           palette = theme.current(), desktop = theme.desktop,
           fonts = theme.fonts, headed = win.headed or false,
           lights = OUT.lights_size(pct) }
end

handlers.draw = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  if not req.more then P.answered(win) end

  for _, o in ipairs(req.ops or {}) do
    local fn = ops[o.op]
    if fn then
      if win.pct and win.pct ~= 100 then scale.op(o, win.pct) end

      fn(win.surface, o)
    end
  end

  --
  -- `more` means the application has not finished this frame.
  --
  -- A window's drawing does not fit in one message, so it arrives in
  -- several - and damaging after each one composited the window
  -- half-redrawn. What that looks like is a flicker on every click: the
  -- first message clears the background and the widgets arrive over the
  -- following two, and the screen is scanned out somewhere in the middle.
  --
  -- The surface is written either way. Only the *damage* waits, so what
  -- reaches the screen is one complete frame rather than three partial
  -- ones. That is what a backbuffer is for, applied one level up: an
  -- application composes off-screen and says when it is done.
  --
  if not req.more then
    --
    -- One line per *finished* frame, under `wm trace`. A window that redraws
    -- when nothing has happened is otherwise invisible: it costs too little
    -- to move a processor meter and it puts the same pixels back, so the
    -- screen cannot show it either. The Terminal repainted itself once a
    -- second for months because there was nowhere to see that it did.
    --
    --
    -- Behind `TRACE` here as well as inside `note`, because the string is
    -- built before `note` can decline it, and this is every finished frame
    -- of every window.
    --
    if TRACE then
      note(("draw %s at %dus"):format(tostring(win.title), trace_us()))
    end

    damage_window(win)
  end

  return { ok = true }
end

--
-- Start a program, in a window, from inside the desktop.
--
-- The window manager does this rather than the application asking for it,
-- and the reason is capabilities: launching means handing the new process
-- an endpoint to this one, and the only process that holds that endpoint is
-- this one. A launcher that could do it itself would have to be given the
-- desktop's own door, and then every application that could reach the
-- launcher could reach that too.
--
-- So the Deskbar asks. It can name a program and nothing else.
--
--
-- **Launches under way, stepped a window at a time** (Diego, 29 September:
-- running Doom "will get the desktop stuck for a second and then run"). An
-- installed application is read off the disk into an image before it can
-- start - eighteen megabytes for Doom and Quake - and this handler read all
-- of it before it answered, so for that second nothing was drawn and nobody
-- was answered: under QEMU 283 ms, against 60 for Calculator, and on the
-- M700 the image comes off a USB stick. So a launch is a coroutine that
-- `IMAGES.load` yields from after each window it copies (`run`'s `pace`),
-- stepped once a pass (`step_launches`, in the loop), and the answer to
-- whoever asked - the Deskbar, which shows an error - waits for the program
-- to have started, as it always did. A program in this image never yields,
-- and finishes in the first step, from here.
--
local launching = {}

local function finish_launch(l, ok, err, id)
  print(("wm: launched %s -> %s %s"):format(tostring(l.program),
        tostring(ok), tostring(ok and id or err)))

  local reply = { ok = true }

  if not ok then
    reply = { ok = false, error = tostring(err) }

    -- Said on its button for a moment, whoever asked: the Deskbar shows
    -- this for a program a desktop icon or Tracker started as well.
    for _, s in ipairs(starting) do
      if s.launch == l then s.failed, s.since = tostring(err), sys.ticks() end
    end
  else
    for _, s in ipairs(starting) do
      if s.launch == l then s.pid = id end
    end

    -- Remembered so that the *next* window to open can be tied to it, when
    -- the kernel cannot say whose an `open` is - which it now can
    -- (`SYS_SENDER`, in `handlers.open`); this is that answer's fallback.
    pending_pid = id
    pending_program = l.path
  end

  -- Nobody waiting: asked with `wait = false`, and answered at the start.
  if l.who then
    replied(pcall(sys.reply, l.who, reply), nil, "launch")
  end
end

local function step_launches()
  if #launching == 0 then return end

  local still = {}

  for _, l in ipairs(launching) do
    local resumed, ok, err, id = coroutine.resume(l.co)

    if not resumed then
      finish_launch(l, false, ok)             -- `ok` is the error, here
    elseif coroutine.status(l.co) == "dead" then
      finish_launch(l, ok, err, id)
    else
      still[#still + 1] = l
    end
  end

  launching = still
end

handlers.launch = function(req, who)
  local name = tostring(req.program or "")

  --
  -- A name is letters, digits, `-`, `_` and `.`; **a whole path is any
  -- file's**, spaces and all - a Lua application in `/Home/my programs` is
  -- opened from Tracker by its path, and this refused it (`testing.md`
  -- 18.414). Never a control character, in either.
  --
  if name == "" or name:find("%c")
     or (name:sub(1, 1) ~= "/" and name:match("[^%w%-_./]")) then
    return { ok = false, error = "not a program name: " .. name }
  end

  local path = fs.program(name)
  --
  -- When, and with what result. `launch` is how everything except the
  -- desktop and the Deskbar gets started, so without this the log has no
  -- moment to measure an application's startup *from* - which is exactly
  -- the number that mattered when one of them took two minutes to reach
  -- its first line.
  --
  -- Still starting from a click a moment ago: this one starts nothing, and
  -- the answer says so rather than failing (`docs/launching.html`).
  for _, s in ipairs(starting) do
    if s.path == path and not s.failed then
      print(("wm: %s is already starting"):format(tostring(req.program)))
      return { ok = true, starting = true }
    end
  end

  print(("wm: launching %s"):format(tostring(req.program)))

  --
  -- **`wait = false` is answered now**, and a failure is said in `starting`
  -- rather than in the reply. The Deskbar asks that way: a window that
  -- waited for the answer could not animate the button that says the
  -- program is starting, which is the whole of what it is for.
  --
  local l = { who = (req.wait ~= false) and who or nil,
              program = req.program, path = path, args = req.args }

  -- A failure still on its button is replaced by the new try.
  stop_starting(function(s) return s.path == path end)
  starting[#starting + 1] = { path = path, launch = l, since = sys.ticks() }

  -- The desktop lent, to a program that declares it needs it (above).
  local shares = { ["/Running/wm"] = ep }
  local attrs = remote.ep and fs.getattr(path)

  for _, word in ipairs(type(attrs) == "table" and attrs.needs or {}) do
    if word == "desktop" then
      shares["/Running/wm/remote"] = remote.ep
      print(("wm: lending the desktop to %s"):format(tostring(req.program)))
    end
  end

  l.co = coroutine.create(function()
    return run(path, req.args or "", true, shares, nil, coroutine.yield)
  end)

  launching[#launching + 1] = l
  step_launches()

  if not l.who then
    return { ok = true, starting = true }
  end

  return DEFER
end

--
-- Stop the machine, or start it again.
--
-- Here rather than in the Deskbar, and the reason is the same one `launch`
-- is here: this process holds `owns_procctl` - it declares
-- `kosmos: needs processes` - and the Deskbar does not. The authority to
-- end every process at once lives in one place, and what the menu sends is
-- a request rather than a right.
--
-- Every window is told first. An application that is listening gets the
-- chance to write something down; one that is not gets nothing, which is
-- the same bargain closing a window already makes. There is no waiting for
-- them: a machine you cannot turn off because something will not answer is
-- the failure mode this whole desktop is built to avoid.
--
handlers.power = function(req)
  local what = (req.action == "restart") and "restart" or "off"

  for _, win in ipairs(windows) do
    post(win, { type = "close" })
  end

  local _, why = sys.power(what)

  -- Only reached if it was refused; otherwise nothing runs after this.
  return { ok = false, error = tostring(why) }
end

--
-- Every window on the desktop, back to front.
--
-- The Deskbar asks this rather than asking /Running, and the difference matters:
-- /Running holds applications that registered, which means the ones that used
-- `ui.window`. A program that opens a window by talking to this process
-- directly - as the first two demonstrations here do, because they were
-- written before there was a kit - has a window on screen and no
-- registration anywhere.
--
-- What belongs in a list of what is running is what is on the screen, and
-- this process is the only one that knows that.
--
--
-- **A page at a time.** The list was one reply, and a reply is a message:
-- 2048 bytes, about sixteen windows' worth. On 28 September the dated
-- picture opened sixteen applications and the Deskbar, the seventeenth did
-- not fit, the reply was dropped - and `tile` and the Deskbar, each waiting
-- in `fs.send` for it, waited for ever. So `from` asks for the list from
-- that place, a page is `WINDOWS_PAGE` long, and `more` says where the next
-- one starts; `wmproto.windows` puts them together. Six, because a window's
-- title and its program's path are each allowed to be long.
--
local WINDOWS_PAGE = 6

handlers.windows = function(req)
  local out = {}
  local from = math.max(1, math.floor(tonumber(req.from) or 1))
  local last = math.min(#windows, from + WINDOWS_PAGE - 1)

  local front = focused_window()

  for i = from, last do
    local win = windows[i]

    out[#out + 1] = { handle = win.handle, title = win.title,
               -- The window the keys go to, which is not a tip on top of it.
               focused = (win == front) or nil,

               -- Who to ask about, and how it draws. `procs` shows this:
               -- a window with a shared region owns its own pixels and the
               -- compositor only blits them, which is a different bargain
               -- from sending drawing commands and is worth being able to
               -- see. `gfx.md` 19.4.
               pid = win.pid,
               direct = (win.shared ~= nil) or nil,

               -- Chrome rather than an application: the desktop underneath
               -- everything and the bar across the top. The Deskbar lists
               -- what is *running* and neither of these is something you
               -- started or can switch to, so it filters on this rather
               -- than on titles - which is what it did for its own window
               -- and does not scale to a second one.
               chrome = (win.backdrop or win.strip or win.popup or win.tip) and true or nil,

               -- Which *kind* of chrome, because "is there a desktop
               -- already" is a question with an answer only this process
               -- has. `desktop` asks it before starting a second one, and
               -- `chrome` cannot answer it: the strip is chrome too.
               backdrop = win.backdrop or nil,

               -- What was started to make it, as a path. The Deskbar puts a
               -- button on the bar per window and draws the application's
               -- own picture on it, which it finds by asking this path's
               -- attributes for the icon its header declares - a title could
               -- not say, because a title is whatever the application feels
               -- like calling itself today.
               program = win.program,

               -- And whether it is minimised, which the bar needs for two
               -- reasons: to draw the button differently, and to know that
               -- clicking it should bring the window back rather than put
               -- it away. Without this the bar would have to remember what
               -- it did last, and a second memory of one fact is a second
               -- thing to be wrong.
               hidden = win.hidden or nil,

               -- How large its content is, which `tile` needs to give a
               -- big window room rather than a corner of it.
               w = win.w, h = win.h }
  end

  -- `watch`: and post this window a `windows` event whenever this answer
  -- would be different. See `tell_watchers` directly below.
  local watcher = by_handle[tonumber(req.watch) or -1]

  if watcher then
    watcher.watching = true
  end

  -- And what is still starting, for the Deskbar's buttons.
  local names = {}

  for i, s in ipairs(starting) do
    names[i] = { program = s.path, failed = s.failed }
  end

  return { ok = true, windows = out, starting = names,
           more = (last < #windows) and (last + 1) or nil }
end

--
-- **Told when the list changes, rather than asking on a clock.**
--
-- The Deskbar learned where the focus was by asking `windows` on its tick,
-- which is once a second, so a click on a window or a Control-W Tab sat on
-- the bar unchanged until the tick came round. Measured under QEMU with the
-- two notes this writes under `trace`: from the focus moving to the bar's
-- next frame took 290 to 1029 ms, about 600 on average - which is Diego's
-- "like half a second" on the ThinkPad. The bar's *own* clicks were already
-- quick, because it paints what it asked for without waiting to be told.
--
-- So a window may ask to be told: `windows` with `watch` set to its own
-- handle, and from then on it is posted a `windows` event whenever the
-- answer would be different. **The event carries nothing.** A list of
-- titles does not fit in an event, and the reply is the one place the list
-- is written down - so the watcher asks again and draws what it is told,
-- and there is no second copy of the list to fall out of step.
--
-- **Posted, never sent**, for `nothing blocks in the key path`'s reason:
-- this runs inside the compositor's loop, and a synchronous call from here
-- to a process that is not answering stops the desktop.
--
-- **Compared once a pass, not announced by each thing that moves a
-- window.** The list changes in `raise`, `open`, `close`, `minimise`, the
-- reaper and whatever sets a title, and a rule that needs every one of them
-- to remember would miss the next one. This looks at the list itself. It
-- runs before `answer_waiting`, so a change made this pass is delivered this
-- pass, and it allocates nothing unless something changed - it is on the
-- frame path.
--
local told = { n = 0, handle = {}, title = {}, hidden = {}, starting = "" }
local told_focus = nil

local function already_told(win)
  for _, ev in ipairs(win.events) do
    if ev.type == "windows" then return true end
  end

  return false
end

local function tell_watchers()
  local n = #windows
  local changed = (n ~= told.n)

  for i = 1, n do
    local w = windows[i]
    local hidden = w.hidden or false

    if told.handle[i] ~= w.handle or told.title[i] ~= w.title
       or told.hidden[i] ~= hidden then
      told.handle[i], told.title[i], told.hidden[i] = w.handle, w.title, hidden
      changed = true
    end
  end

  for i = n + 1, told.n do
    told.handle[i], told.title[i], told.hidden[i] = nil, nil, nil
  end

  told.n = n

  -- What is starting, as one string to compare: a handful of paths at most.
  if #starting > 0 or told.starting ~= "" then
    local now = {}

    for i, s in ipairs(starting) do
      now[i] = s.path .. (s.failed and ("\t" .. s.failed) or "")
    end

    now = table.concat(now, "\n")

    if now ~= told.starting then
      told.starting = now
      changed = true
    end
  end

  --
  -- Under `trace`, when the focus moves, stamped with the counter. With the
  -- `draw` note that follows it, this is how long the Deskbar took to show
  -- where the focus went - which the display harness holds to a bound.
  --
  local top = windows[n]

  if top ~= told_focus then
    told_focus = top

    if TRACE then
      note(("focus %s at %dus"):format(tostring(top and top.title), trace_us()))
    end
  end

  if not changed then return end

  for i = 1, n do
    local w = windows[i]

    if w.watching and not already_told(w) then
      post(w, { type = "windows" })
    end
  end
end

--
-- Put one away. The other half of `raise`, which is what brings it back.
--
-- Here rather than in the Deskbar because `hidden` is this process's fact:
-- it decides what is composed, and a window that is not composed is not on
-- the screen. The bar asks, exactly as it asks for a raise - see
-- `handlers.raise` directly below, which unhides on its way.
--
-- A strip or the backdrop is refused. Minimising the bar would hide the one
-- thing that brings windows back, which is the same argument that gives the
-- Deskbar no minimise box of its own.
--
handlers.minimise = function(req)
  local win = by_handle[tonumber(req.window) or -1]

  if not win then
    return { ok = false, error = "no such window" }
  end

  if win.backdrop or win.strip or win.popup or win.tip then
    return { ok = false, error = "the desktop, the bar and a popup do not minimise" }
  end

  return { ok = minimise(win) }
end

--
-- Bring one to the front. What clicking a name in the Deskbar does.
--
--
-- **The pointer without a button, asked for and bounded.**
--
-- Movement goes only to a window holding a button (see where presses are
-- handled) because every movement would be a message. Cafesa3D's G, R and
-- S are Blender's: the object follows the pointer with no button held,
-- until a click puts it down. So a window may ask, for as long as such an
-- operation lasts, to be told where the pointer goes - at most once a pass,
-- as a menu's hover is, and only while it is the focused window, which is
-- the one the keyboard already goes to, so it learns nothing it could not
-- have been typed. `{ type = "track", window = h, on = true|false }`.
--
handlers.track = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  win.tracking = req.on and true or nil
  return { ok = true }
end

handlers.raise = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  raise(win)
  return { ok = true }
end

--
-- How big a picture is, so an application can lay out around it.
--
-- Asked of this process because this process is the one that has it. An
-- application that could decode it itself would have the pixels, which is
-- the thing the whole design is arranged to prevent.
--
handlers.image_size = function(req)
  local picture = picture_named(tostring(req.asset or ""))

  if not picture then
    return { ok = false, error = "no such picture, or it would not decode" }
  end

  local w_, h_ = picture:size()
  return { ok = true, w = w_, h = h_ }
end

--
-- The application has finished a frame.
--
-- Swaps which buffer is shown and damages what it said changed. Nothing is
-- copied: the compositor simply reads the other one from now on.
--
handlers.commit = function(req)
  local win = by_handle[req.window]

  if not win or not win.shared then
    return { ok = false, error = "that window does not have a shared surface" }
  end

  P.answered(win)

  --
  -- **The first frame in a region handed over at a new size** (6zz e): it
  -- takes the old one's place now, and not when it was handed over, so the
  -- window showed the old picture until there was a new one to show. It
  -- was drawn into its first buffer, which is the one shown.
  --
  if win.shared_next then
    local old = win.shared

    win.shared, win.shared_next = win.shared_next, nil
    win.src_w, win.src_h = win.shared.w, win.shared.h
    win.shared.live = 2
    sys.release(old.cap)
    damage_window(win)
  end

  win.shared.live = (win.shared.live == 1) and 2 or 1

  -- In the buffer's own coordinates, which start below a menu bar when
  -- the window has one (`strips`): the damage moves down with it, and is
  -- held to the buffer rather than to the window around it.
  local below = strips.below(win)
  local sw_ = win.src_w or win.w
  local sh_ = win.src_h or (win.h - below)
  local x = math.max(0, math.floor(tonumber(req.x) or 0))
  local y = math.max(0, math.floor(tonumber(req.y) or 0))
  local w_ = math.min(sw_ - x, math.floor(tonumber(req.w) or sw_))
  local h_ = math.min(sh_ - y, math.floor(tonumber(req.h) or sh_))

  if w_ > 0 and h_ > 0 then
    --
    -- In the surface's pixels, which at a scale are not the screen's: the
    -- damage is where they land, rounded outwards (`ui.md` 16.18).
    --
    local dw_, dh_ = win.w, win.h - below
    local x0, y0 = x * dw_ // sw_, y * dh_ // sh_
    local x1 = ((x + w_) * dw_ + sw_ - 1) // sw_
    local y1 = ((y + h_) * dh_ + sh_ - 1) // sh_

    add_damage(win.x + x0, win.y + below + y0, x1 - x0, y1 - y0)
  end

  -- The buffer the application should draw into next: the one this process
  -- has just stopped showing.
  return { ok = true, draw_into = win.shared.live == 1 and 2 or 1 }
end

--
-- A window asking for a size, rather than a person dragging one.
--
-- Same clamps and the same event as the grip, because an application that
-- resizes itself has to lay out again exactly as one that was resized by
-- hand - and having two paths that agree only by accident is how they stop
-- agreeing.
--
--
-- **A picture an application hands over, rather than one this process opens.**
--
-- `ui.image` names a picture and this process finds it: an asset compiled in,
-- or a file on the disk. **A cover inside an MP3 is neither** - it is bytes in
-- the middle of somebody else's file, and `tags.lua` reports where they are
-- rather than reading them, so that a library of a thousand songs does not
-- decode a thousand pictures to show ten.
--
-- So the application reads those bytes into a region and hands the region over
-- with a name. `gfx.png` and `gfx.jpeg` already take an address and a length,
-- and `picture_from` already maps and decodes - the only new part is
-- taking the caller's pages instead of opening a file. **Control by message,
-- data by shared memory**, which is the system's rule rather than an exception
-- here, and it needs no file nobody asked for: a cover is the song's, not a
-- thing to keep in `/Temporary` or `/Home`.
--
-- **The name carries the track**, because the cache is keyed by name: one
-- fixed name would hand every song the first song's picture.
--
local MIMES = { ["image/png"] = gfx.png, ["image/jpeg"] = gfx.jpeg }

handlers.picture = function(req, who, cap)
  --
  -- Let go of the capability on every path out, not only the happy one.
  -- A capability kept is a slot in this process's table for as long as the
  -- desktop runs. The table grows now, but a slot per picture handed over
  -- is still a leak, and when the table was sixteen slots a server that kept
  -- them refused every request after the sixteenth - which is what a PDF
  -- read in small windows found on its fifteenth read (`init.lua`).
  --
  local function done(answer)
    if cap and cap >= 0 then sys.release(cap) end

    return answer
  end

  local name = tostring(req.name or "")
  local bytes = tonumber(req.bytes) or 0
  local decode = MIMES[tostring(req.mime or "")]

  if name == "" or name:sub(1, 1) == "/" then
    return done({ ok = false,
                  error = "a handed-over picture needs a name of its own, "
                          .. "and a leading slash means a file on the disk" })
  end

  if not decode then
    return done({ ok = false,
                  error = "this decodes image/png and image/jpeg, and "
                          .. tostring(req.mime) .. " is neither" })
  end

  if bytes <= 0 or not cap or cap < 0 then
    return done({ ok = false,
                  error = "a picture needs its length and the pages holding it" })
  end

  local at, why = sys.memory_map(cap)

  if not at then
    return done({ ok = false,
                  error = "could not map the picture: " .. tostring(why) })
  end

  local ok, made = pcall(decode, at, bytes)

  if not ok or not made then
    return done({ ok = false,
                  error = tostring(made or "those bytes did not decode") })
  end

  remember_picture(name, made)

  local w, h = made:size()

  return done({ ok = true, w = w, h = h })
end

--
-- **A region at a new size, from a window that draws its own pixels**
-- (`roadmap.md` 6zz e): `{ window, w, h }` and the region's capability -
-- two surfaces that size, in its points, made after it was told its new
-- size. Held to holding them, as `watch` holds a viewer's region, mapped,
-- and kept aside until the first frame drawn in it is committed
-- (`handlers.commit`). The reply says which buffer to draw into: the
-- first.
--
handlers.surface = function(req, _, cap)
  local win = by_handle[req.window]

  local function refuse(why)
    if cap and cap >= 0 then sys.release(cap) end
    return { ok = false, error = why }
  end

  if not win or not win.shared then
    return refuse("that window does not draw its own pixels")
  end

  local w, h = math.tointeger(tonumber(req.w)), math.tointeger(tonumber(req.h))

  if not w or not h or w < 1 or h < 1 or not cap or cap < 0 then
    return refuse("a new surface needs a size and a region")
  end

  local bytes = gfx.bytes(w, h)

  if (sys.memory_size(cap) or 0) * 4096 < bytes * 2 then
    return refuse("that region does not hold two surfaces that size")
  end

  local at, why = sys.memory_map(cap)

  if not at then return refuse("it could not be mapped: " .. tostring(why)) end

  -- One handed over before this and never drawn in goes; this replaces it.
  if win.shared_next then sys.release(win.shared_next.cap) end

  win.shared_next = {
    cap = cap, w = w, h = h,
    [1] = gfx.wrap{ at = at, w = w, h = h },
    [2] = gfx.wrap{ at = at + bytes, w = w, h = h },
    live = 1,
  }

  return { ok = true, draw_into = 1 }
end

handlers.resize = function(req)
  local win = by_handle[req.window]

  if not win then return { ok = false, error = "no such window" } end

  --
  -- **What the application may ask for, which is not what the pointer may
  -- pull.** This asked `resizable`, and that answers whether a window gets
  -- a sizing grip - no for a strip, rightly: the bar across the top is not
  -- a window to be dragged into another shape. But it is a window that may
  -- ask to be another height, since its height became a choice
  -- (`roadmap.md` 5v), and asking the grip's question refused the Deskbar's
  -- own request in silence. A window that draws its own pixels still
  -- cannot be resized, because its surface is shared and has a size.
  --
  if (win.shared ~= nil and not win.resizes_itself) or win.backdrop then
    return { ok = false,
             error = "a window that draws its own pixels and did not say it "
                     .. "can make another region cannot be resized" }
  end

  local pct = win.pct or 100

  resize_window(win,
                tonumber(req.w) and scale.px(tonumber(req.w), pct) or win.w,
                tonumber(req.h) and scale.px(tonumber(req.h), pct) or win.h)

  -- A dock that changes width stays centred, and keeps its place above the
  -- edge (`place_bottom`).
  if win.strip == "bottom" then
    damage_window(win)
    place_bottom(win)
    damage_window(win)
    print(("wm: the dock at %d,%d %dx%d"):format(win.x, win.y, win.w, win.h))

    -- And told, as a drag tells a window: a floating dock that grew is
    -- centred again, and its menu opens from where it is now.
    post(win, { type = "moved", x = win.x, y = win.y })
  end

  -- A strip that changes height changes the room above everything else.
  if win.strip then recount_strips() end

  return { ok = true, w = scale.pt(win.w, pct), h = scale.pt(win.h, pct) }
end

handlers.retitle = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  local was = win.title
  local title = tostring(req.title or was)

  -- A name it already has changes nothing on screen, and the line below
  -- would say that it had.
  if title == was then return { ok = true } end

  -- Both the old tab and the new one: a shorter title leaves the tail of
  -- the longer one behind, and the tab is as wide as its text.
  damage_window(win)
  win.title = title
  damage_window(win)

  -- Said, as where it was placed is said: the log knows a window by its
  -- title, and from here on that is a different one.
  print(("wm: window %s is now %s"):format(tostring(was), title))

  return { ok = true }
end

-- How many pixels of a window stay on the screen however far it is pushed,
-- by a drag or by a new scale (`move_window` says why).
local KEEP = 48

--
-- **A window put somewhere, in the screen's pixels.** The drag and the
-- keyboard's Control-W arrows call this; an application's `move` asks in
-- points and is converted first (`handlers.move`). They were one function
-- until the scale: the drag called the application's handler with pixels,
-- which since 0.10.115 multiplies what it is given, so at 150 per cent a
-- window moved half again as far as the pointer and slid out from under it.
-- Diego, 22 September, on the ThinkPad: "if you grab a window by the
-- titlebar in 110%, you will see the mouse is off by a margin", "even worse
-- with more scale". The same shape as `resize_window` and `handlers.resize`.
--
-- Declared with `resizable` and `resize_window`, far above, because the
-- drag calls it from before this point in the file.
--
function move_window(win, x, y, quiet)
  -- The old place has to be repainted as well as the new one, or the window
  -- leaves a copy of itself behind.
  damage_window(win)
  -- A window may hang off the edge, as long as enough of it stays to grab.
  --
  -- Clamping it entirely inside the screen is the obvious thing and it is
  -- wrong: every desktop lets you push a window aside to see what is under
  -- it, and a window that stops dead at the edge cannot be pushed anywhere.
  --
  -- What must not happen is losing it. So the tab may not go above the top -
  -- it is the only handle - and KEEP pixels of the window stay on screen in
  -- every other direction, which is always enough of the tab to catch.
  win.x = math.min(math.max(x, KEEP - win.w), W - KEEP)
  win.y = math.min(math.max(y, OUT.top_of(win)), H - KEEP)
  damage_window(win)

  --
  -- And the application is told where it now is.
  --
  -- It needs to know, because a menu is a *window* and is placed on the
  -- screen rather than inside the window that opened it. Without this a
  -- window that had been dragged opened its menus where it used to be -
  -- which is exactly what happened, in the corner it started in, halfway
  -- across the screen from the button that was pressed.
  --
  -- Not while a drag is in progress: that would be a message per pointer
  -- movement, and the queue would fill with positions nobody read. `quiet`
  -- is the drag saying it will report the final position itself, on release.
  --
  if not quiet then
    post(win, { type = "moved", x = win.x, y = win.y })
  end
end

-- An application's `move`, in its points.
handlers.move = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  local pct = win.pct or 100

  move_window(win,
              tonumber(req.x) and scale.px(tonumber(req.x), pct) or win.x,
              tonumber(req.y) and scale.px(tonumber(req.y), pct) or win.y,
              req.quiet)

  return { ok = true, x = scale.pt(win.x, pct), y = scale.pt(win.y, pct) }
end

--
-- **Where a header's three go** (`roadmap.md` 6zj). A window whose header
-- is its title bar leaves room at the header's right end and says where,
-- in its points; this process draws the three there over its page - in
-- colour in front and grey behind, with their glyphs under the pointer -
-- and takes the presses on them, as it does on a tab. The window never
-- draws them, so which window is in front is shown the moment it changes,
-- from what this process already knows.
--
-- Held inside the window: a place that would put them outside it is
-- pulled back, so nothing a window says can have this process draw over
-- another.
--
handlers.lights = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  if not win.headed then
    return { ok = false, error = "this window wears a title bar" }
  end

  local x, y = tonumber(req.x), tonumber(req.y)

  if not (x and y) then
    return { ok = false, error = "the three need an x and a y" }
  end

  local pct = win.pct or 100

  x = math.max(0, math.min(scale.px(x, pct), win.w - OUT.RUN))
  y = math.max(0, math.min(scale.px(y, pct), win.h - OUT.BOX))

  OUT.damage_boxes(win)
  win.lights_at = { x = x, y = y }
  OUT.damage_boxes(win)

  print(("wm: %s's three at %d,%d in it"):format(tostring(win.title), x, y))

  return { ok = true }
end

--
-- **A press on a header's empty band, taken as a drag** (`roadmap.md`
-- 6zj). The window is what knows its band from its buttons - it hit-tests
-- its own views - so the press goes to it as every press does, and it hands
-- the rest of the press back here: until the button comes up the pointer
-- moves the window, and the window hears no more of it.
--
-- `x` and `y` are where the press landed, in its points, so the point that
-- was pressed stays under the pointer however far the pointer went while
-- this was on its way. Only for the press still held on this window: a
-- band clicked and let go before this arrived was a click.
--
handlers.move_begin = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  if not win.headed then
    return { ok = false, error = "this window is moved by its title bar" }
  end

  if ((PT.buttons or 0) & 1) == 0 or PT.grabbed ~= win or PT.dragging then
    return { ok = false, error = "no press is held on this window" }
  end

  local pct = win.pct or 100
  local x = math.max(0, math.min(scale.px(tonumber(req.x) or 0, pct), win.w))
  local y = math.max(0, math.min(scale.px(tonumber(req.y) or 0, pct), win.h))

  PT.grabbed = nil
  PT.dragging = { win = win, dx = x, dy = y, header = true }

  return { ok = true }
end

--
-- **Maximise, and back, asked by the window** - a double click on its
-- header's band, which is where a title bar's double click went.
--
handlers.maximise = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  if not resizable(win) then
    return { ok = false, error = "this window cannot be resized" }
  end

  local done = maximise(win)

  if done then
    print(("wm: %s %s"):format(tostring(win.title),
          win.restore and "maximised" or "restored"))
  end

  return { ok = done, maximised = win.restore ~= nil }
end

--
-- **At most this many in one reply, and the rest wait for the next poll.**
--
-- `post` bounds the queue at 64 events, which is a bound on *memory*.
-- `MSG_BYTES` is 2048, which is a bound on the *wire*. Nothing reconciled
-- the two, and sixty-four events do not fit in two kilobytes.
--
-- On the first real machine that was fatal and silent: every application
-- blocked in its first poll for as long as the desktop was up, and the
-- window manager was raising `value does not fit in a message` on every
-- reply. The `pcall` around `sys.reply` swallowed it - rightly wrapping,
-- wrongly discarding - so nothing was printed, and a caller parked in
-- `sys.call` has nothing left in the system that can wake it. A desktop
-- that drew perfectly and answered nobody.
--
-- Twelve, because an event is a small table and a dozen of them leave room
-- for the reply around them. It is deliberately not the queue's bound:
-- those two numbers measure different things and pretending otherwise is
-- what caused this.
--
-- **A partial batch is always correct here.** An event queue is a queue:
-- the caller polls again immediately, and what is left goes in order on the
-- next reply. What is never correct is a reply that cannot be sent.
--
--
-- **And a count was the wrong bound, the second time.** Twelve assumed an
-- event is small, and a theme event is a whole palette and five faces:
-- three of them queue when somebody picks faces in Appearance quickly, and
-- on the ThinkPad on 22 September that was `wm: reply for poll failed:
-- value does not fit in a message`, and the window's events went with it.
-- So the reply is filled while it *fits*, asked of the serialiser itself
-- (`sys.fits`) rather than estimated, and the count stays as a bound on
-- how much one pass hands over.
--
-- An event too big to go even alone can never be delivered, and waiting
-- would stop the queue behind it for good; it is dropped and said.
--
local EVENTS_PER_REPLY = 12

local function events_for(win)
  if #win.events > 0 then P.delivered(win) end

  local out, rest = {}, {}
  local reply = { ok = true, events = out }

  for _, ev in ipairs(win.events) do
    if #rest > 0 or #out >= EVENTS_PER_REPLY then
      rest[#rest + 1] = ev
    else
      out[#out + 1] = ev

      if not sys.fits(reply) then
        out[#out] = nil

        if #out == 0 then
          print(("wm: an event for %s would not fit in a message and is "
                 .. "dropped: %s"):format(tostring(win.title),
                                          tostring(ev.type)))
        else
          rest[#rest + 1] = ev
        end
      end
    end
  end

  win.events = rest

  return reply
end

--------------------------------------------------------------------------
-- The clipboard.
--
-- **Here because this is the one process every window already talks to.**
-- A clipboard is shared state between programs that must not be able to
-- reach each other, which is the same shape as the screen and the console,
-- and the answer is the same: a server holds it and everyone asks. No
-- global name, no shared page, nothing an application can reach that it
-- was not handed.
--
-- One buffer, not a ring of them. BeOS had numbered clipboards and almost
-- nothing used more than the first; a second one can be added the day
-- something wants it.
--
-- **It is capped, and the cap is a message.** `MSG_BYTES` is 2048 and the
-- text travels inside a serialised table, so a selection larger than that
-- cannot cross in one piece. `design.md` 7.4's rule says a *stream*
-- belongs in shared memory and a one-shot payload is fine as a message - a
-- copy is one-shot, so this is the right shape, and the cap is the honest
-- edge of it rather than a design mistake.
--
-- **The cap that matters is `wmproto`'s, and this one is not it.** A
-- message too big to serialise makes `fs.send` raise in the *caller*, so
-- by the time anything arrives here it has already fit; `wmproto.copy` is
-- where the text is cut and where the caller is told how much was left
-- behind. This bound exists because a server does not get to assume its
-- callers are the library - `design.md` 17, a server receives what it
-- expects rather than whatever somebody sent - and it is the same number
-- so that the two never disagree about what a full clipboard is.
--------------------------------------------------------------------------

local CLIP_MAX = 1900

local clipboard = ""

handlers.clip_put = function(req)
  local text = tostring(req.text or "")
  local taken = text:sub(1, CLIP_MAX)

  clipboard = taken

  return { ok = true, bytes = #taken, dropped = #text - #taken }
end

handlers.clip_get = function()
  return { ok = true, text = clipboard }
end

handlers.poll = function(req, who)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  -- The last time this application said anything. `collect_closing` uses it
  -- to tell a window whose process has died from one that is merely busy,
  -- and polling is the right thing to measure because it is what every
  -- window does whether or not anything is happening.
  win.last_poll = sys.ticks()

  if #win.events > 0 then
    return events_for(win)
  end

  -- Nothing yet. Hold the answer rather than sending an empty one.
  --
  -- `wait_ticks`, and the name is the fix: the field was `wait`, every
  -- caller wrote scheduler ticks into it, and this line added that to
  -- `sys.ticks()` - the counter, a quarter of a million times finer. See
  -- `user/lib/wmproto.lua`, which is now the only place the message is
  -- built.
  local wait = tonumber(req.wait_ticks) or POLL_DEFAULT

  -- **Both halves of a poll are narrated, and that is the point of them.**
  --
  -- An application that never redraws is either one that never asked, or
  -- one that asked and was never answered, and from a photograph of a
  -- frozen desktop those look identical. `wait` is in *scheduler* ticks
  -- here, so a number in the millions is the unit bug this file has been
  -- bitten by twice and the log would say so at a glance.
  if TRACE then
    note(("poll %s wait=%s"):format(tostring(win.title), tostring(wait)))
  end

  waiting[#waiting + 1] = {
    who = who, win = win, deadline = sys.ticks() + in_counter(wait),
    raw = req.raw,                      -- answered as `wmproto.h` says
  }

  return DEFER
end

--------------------------------------------------------------------------
-- Starting a drag, and answering the drop it ends in.
--
-- `{ type = "drag", window = h, kind = "files", payload = "...",
--    label = "3 items" }` - and no payload cancels one, which is what a
-- source does if it decides mid-gesture that it was not a drag after all.
--
-- The payload is a string on purpose rather than a table. This process is a
-- carrier and has no business being able to be confused by what it carries:
-- a string is one length to check, where a table is a shape, a depth and a
-- serialiser between two processes that never agreed on anything. Whoever
-- sends it and whoever receives it are the two who share a format.
--------------------------------------------------------------------------
handlers.drag = function(req)
  local win = by_handle[req.window]

  if not win then return { ok = false, error = "no such window" } end

  if req.payload == nil then
    if PT.drag and PT.drag.from == win.handle then
      add_damage(PT.x, PT.y, cursor_size())
      damage_outline(PT.outline)
      PT.drag, PT.outline = nil, nil
    end

    return { ok = true }
  end

  if type(req.payload) ~= "string" then
    return { ok = false, error = "a payload is a string" }
  end

  PT.drag = {
    from    = win.handle,
    kind    = tostring(req.kind or ""),
    payload = req.payload,
    label   = req.label and tostring(req.label) or nil,
  }

  -- The badge appears where the pointer already is, so that rectangle has
  -- to be redrawn even though nothing moved.
  add_damage(PT.x, PT.y, cursor_size())

  return { ok = true }
end

--
-- **Where a maximised window goes**, asked before a window exists: the
-- rectangle `maximise` gives one - the screen less the strip, the title bar
-- and the frame. A window that draws its own pixels opens at its size and
-- is never resized, so an application that wants to open maximised has to
-- know the size first (Cafesa3D, 26 September: "3d tools are mostly used
-- maximized"). The window's contents, in the screen's points.
--
handlers.workarea = function(req)
  -- Asked by a window that will have its header for a title bar, in a look
  -- where it will: no tab and no border to leave room for (`OUT.room`).
  if req and req.header == true and theme.title_bars == false then
    return { ok = true, x = 0, y = reserved_top, w = W,
             h = H - reserved_top - reserved_bottom, headed = true }
  end

  local w, h = OUT.maximised()

  return { ok = true, x = OUT.BORDER, y = top_limit(), w = w, h = h }
end

--
-- The destination saying what became of it, which is passed back to the
-- source so it can show the directory it no longer holds.
--
-- Only the window this process just handed a drop to, and only once. See
-- `answering` above: without that check this is a way to post an event to
-- any window whose handle you can guess.
--
handlers.dropped = function(req)
  if not (PT.answering and PT.answering.to == req.window) then
    return { ok = false, error = "nothing was dropped on you" }
  end

  post(by_handle[PT.answering.from], {
    type  = "dropped",
    ok    = req.ok and true or false,
    count = tonumber(req.count) or 0,
    error = req.error and tostring(req.error) or nil,
  })

  PT.answering = nil

  return { ok = true }
end

--
-- Answer everyone whose window has something, or whose wait is over.
--
--------------------------------------------------------------------------
-- **The frame path as a declared shape** (`user/include/wmproto.h`,
-- `docs/windowkit.md` step W2): `commit` and `poll` as fixed structs, on
-- this same endpoint - the one whose arrival ends this process's sleep -
-- told from a table by the message's tag. A C app sends them straight from
-- C, and neither side builds a table for them on the way: here the struct
-- becomes the request the table handlers below already answer, and their
-- answer becomes the struct, so commit and poll are each written once.
--------------------------------------------------------------------------

local frame = {
  TAG = 0x31454d4152464d57,                     -- "WMFRAME1"
  REQUEST = "<I4I4i4i4I4I4I4",                  -- struct wm_frame_request
  HEAD = "<i4I4I4",                             -- struct wm_frame_reply's
  EVENT = "<I2I2I2I2i4i4i4",                    -- struct wm_frame_event
  OPS = { [1] = "commit", [2] = "poll" },
  TYPES = { key = 1, mouse = 2, wheel = 3, resize = 4, close = 5 },
  ACTIONS = { press = 1, release = 2, move = 3 },
  NO_WINDOW = 1, NO_SURFACE = 2, BAD = 3,
}

function frame.whole(v)
  return math.tointeger(math.floor(tonumber(v) or 0)) or 0
end

-- A table handler's answer, as `struct wm_frame_reply`. Only the events the
-- shape has go: a move, a menu's press, anything else is a table's.
function frame.reply(reply)
  local whole = frame.whole

  if type(reply) ~= "table" or not reply.ok then
    return string.pack(frame.HEAD, frame.BAD, 0, 0)
  end

  local parts = {}

  for _, ev in ipairs(reply.events or {}) do
    local t = frame.TYPES[ev.type]

    if t and not ev.menu then
      local act, button, a, b, c = 0, 0, 0, 0, 0

      if t == 1 then
        a = whole(ev.code)
      elseif t == 2 then
        act = frame.ACTIONS[ev.action] or 3
        button = (ev.button == "right") and 2 or 1
        a, b = whole(ev.x), whole(ev.y)
      elseif t == 3 then
        a, b, c = whole(ev.x), whole(ev.y), whole(ev.n)
      elseif t == 4 then
        a, b = whole(ev.w), whole(ev.h)
      end

      parts[#parts + 1] = string.pack(frame.EVENT, t, act, button, 0, a, b, c)
    end
  end

  return string.pack(frame.HEAD, 0, whole(reply.draw_into), #parts)
         .. table.concat(parts)
end

-- An answer to a table or to a struct, whichever it was asked as.
function frame.answer(who, reply, raw)
  if raw then return pcall(sys.reply_raw, who, frame.reply(reply)) end

  return pcall(sys.reply, who, reply)
end

-- One frame request, answered or held. A server receives what it expects:
-- a short message, an operation the shape has not got, or a window that is
-- not there is answered with why, never acted on.
function frame.request(bytes, who)
  local function refuse(code)
    replied(pcall(sys.reply_raw, who, string.pack(frame.HEAD, code, 0, 0)), nil, "a frame")
  end

  if #bytes < string.packsize(frame.REQUEST) then return refuse(frame.BAD) end

  local op, window, x, y, w, h, wait = string.unpack(frame.REQUEST, bytes)
  local name, win = frame.OPS[op], by_handle[window]

  if not name then return refuse(frame.BAD) end
  if not win then return refuse(frame.NO_WINDOW) end
  if name == "commit" and not win.shared then return refuse(frame.NO_SURFACE) end

  local ok, result = pcall(handlers[name], { type = name, window = window, x = x, y = y,
                                             w = w, h = h, wait_ticks = wait,
                                             raw = true }, who)

  if ok and result == DEFER then return end

  replied(frame.answer(who, ok and result or nil, true))
end

local function answer_waiting()
  local now = sys.ticks()
  local still = {}

  for _, w in ipairs(waiting) do
    if by_handle[w.win.handle] == nil then
      -- Its window closed underneath it. An empty answer, so the
      -- application's loop notices and leaves rather than hanging on a
      -- reply nobody is going to send.
      replied(frame.answer(w.who, { ok = true, events = {} }, w.raw),
              nil, "a closed window")
    elseif #w.win.events > 0 or now >= w.deadline then
      if TRACE then
        note(("answer %s %s"):format(tostring(w.win.title),
             #w.win.events > 0 and "events" or "due"))
      end

      do
        local ok, err = frame.answer(w.who, events_for(w.win), w.raw)
        replied(ok, err, w.win.title)
      end
    else
      still[#still + 1] = w
    end
  end

  waiting = still
end

handlers.close = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  damage_window(win)
  by_handle[req.window] = nil

  -- A drag whose source or destination has just gone. The badge would
  -- otherwise stay under the pointer until the button came up, and the
  -- one-shot right to answer would outlive the window it was given to.
  if PT.drag and PT.drag.from == req.window then
    add_damage(PT.x, PT.y, cursor_size())
    damage_outline(PT.outline)
    PT.drag, PT.outline = nil, nil
  end

  if PT.answering and (PT.answering.to == req.window
                    or PT.answering.from == req.window) then
    PT.answering = nil
  end

  -- Whichever list it is in. A menu is a window in every way except where
  -- it is stacked, and this is the one place that difference has to be
  -- spelled out on the way out.
  local list = (win.kind == "menu") and menus or windows

  for i, w_ in ipairs(list) do
    if w_ == win then
      table.remove(list, i)
      break
    end
  end

  -- A strip that goes gives its room back: to the windows that open after
  -- it, and to the backdrop, which grows back up to the top of the screen.
  if win.strip then recount_strips() end

  -- Said for a window, as its opening is; a menu comes and goes with every
  -- click and is not.
  if win.kind ~= "menu" then
    print(("wm: closed %s"):format(tostring(win.title)))
  end

  --
  -- And a window takes its menus with it. Without this a menu outlives the
  -- window it belongs to and floats above a desktop with nothing behind
  -- it, still taking clicks - which is the leak the whole design was
  -- chosen to avoid, arriving by the back door.
  --
  if win.kind ~= "menu" then
    for i = #menus, 1, -1 do
      if menus[i].owner == req.window then
        local orphan = menus[i]

        damage_window(orphan)
        by_handle[orphan.handle] = nil
        table.remove(menus, i)
        orphan.surface:free()
      end
    end
  end

  win.surface:free()

  --
  -- **And the application's own region, which this process was holding.**
  --
  -- A window that draws its own pixels hands over a capability to the
  -- memory it draws into, and `open` maps it and keeps it in `win.shared`.
  -- Nothing gave it back. The surface freed above is the *compositing*
  -- one, which is this process's; the region is the application's, and a
  -- capability held after the window is gone is memory that never comes
  -- back to the machine.
  --
  -- It is per window, so it is invisible until something opens and closes
  -- them in a loop - which is exactly what the Video app does, because
  -- every File > Open relaunches the player. Diego, 21 September: "the
  -- video player ran once with the mp4 mjpeg video but not a second time",
  -- and "it looks something remained in memory". It did, and it was here.
  --
  -- The player was blameless: six plays with no window manager at all
  -- decode perfectly, and four plays each under a fresh desktop fail on
  -- the fourth. What was reported was `no moov box - not an MP4` - a short
  -- read, dressed up as a bad file, which is a separate thing to fix.
  --
  if win.shared and win.shared.cap then
    sys.release(win.shared.cap)
    win.shared = nil
  end

  -- And a region handed over at a new size and never drawn in (6zz e).
  if win.shared_next then
    sys.release(win.shared_next.cap)
    win.shared_next = nil
  end

  return { ok = true }
end


--
-- Everything the window manager did not claim goes to the window with the
-- focus, as an event it can collect whenever it gets round to asking.
-- Queued and not delivered: delivering would mean calling the application,
-- and calling it is what this process must never do.
--
local QUEUE_MAX = 64

--
-- **What a full queue gives up, and it used to be whatever came first.**
--
-- A key or a movement is one of a stream: losing one loses one. A press, a
-- release, a close or a resize is half of a state the application is
-- holding, and losing it leaves that state wrong for ever. On the first real
-- machine a serial port that was not there read as sixty-four keys a pass,
-- all posted to the focused window, and every button went down and none came
-- up: the window manager logged each release and no application received
-- one, and a full queue dropping its oldest event is the only way a posted
-- event does not arrive. `hal/pc/uart.c` has the cause; this is why the next
-- cause like it costs keystrokes rather than a wedged widget.
--
local function expendable(event)
  return event.type == "key" or event.type == "rawkey"
         or (event.type == "mouse" and event.action == "move")
end

function post(win, event)
  if not win then return end

  -- In the window's points (`scale.event`), on a copy: an event table may
  -- be on its way to more than one window.
  if win.pct and win.pct ~= 100
     and (event.x or event.y or event.w or event.h) then
    local copy = {}

    for k, v in pairs(event) do copy[k] = v end

    scale.event(copy, win.pct)
    event = copy
  end

  local events = win.events
  local last = events[#events]

  --
  -- **The pointer passing over, kept as where it is now**: a hover move
  -- not yet collected is replaced by the next, since only the latest says
  -- where the pointer is. Every one was queued - a window that asked for
  -- them (`wmproto.track`, the launcher's sidebar since 7 October) was sent
  -- a mouse's thousand a second, faster than it reads, and the queue's
  -- limit dropped them with a line in the log each (Diego's screenshot).
  --
  if event.hover and last and last.hover and last.type == event.type
     and last.action == event.action then
    events[#events] = event
    return
  end

  events[#events + 1] = event
  P.posted(win, event)

  if #events > QUEUE_MAX then
    -- An application that has stopped collecting its events is not going to
    -- start. Dropping one is better than growing without limit in a process
    -- that everything else on the screen depends on.
    local victim = 1

    for i = 1, #events do
      if expendable(events[i]) then
        victim = i
        break
      end
    end

    local lost = table.remove(events, victim)

    --
    -- Said, because a dropped event is invisible from everywhere else: the
    -- application never learns it existed, and a button that stays down
    -- looks like a bug in the button. The first five one at a time, then at
    -- each power of ten, per window - a flood is a line per order of
    -- magnitude rather than a ring full of the same sentence.
    --
    win.dropped = (win.dropped or 0) + 1

    local n = win.dropped

    if n <= 5 or n == 100 or n == 1000 or n == 10000 or n == 100000 then
      print(("wm: %s is not collecting its events; dropped a %s%s, %d so far")
            :format(tostring(win.title), tostring(lost.type),
                    lost.action and (" " .. lost.action) or "", n))
    end
  end
end

--------------------------------------------------------------------------
-- The appearance of everything.
--
-- One request changes the palette and the desktop colour, repaints the
-- whole screen, and tells every window so its widgets are redrawn in the
-- new colours too. `ui.lua` mutates its own palette table in place when it
-- gets that event, so a running application changes appearance without
-- restarting and without knowing this happened.
--
-- The window manager is where this lives because it is the one process
-- that already talks to every window. A settings *server* would be the
-- other answer and is more machinery than one palette needs.
--------------------------------------------------------------------------
--
-- Set the desktop picture. `{ type = "wallpaper", path = "/Home/x.png" }`,
-- and no path at all clears it.
--
handlers.wallpaper = function(req)
  local ok, why = wallpaper_load(req.path)

  if not ok then return { ok = false, error = why } end

  wallpaper_place()
  add_damage(0, 0, W, H)

  return { ok = true }
end

--
-- How it covers the screen: `{ type = "wallpaper_fit", fit = "fill" }` or
-- `"centre"`, Appearance's *Wallpaper size*. Placed again at once.
--
handlers.wallpaper_fit = function(req)
  local fit = (req.fit == "fill" or req.fit == "centre") and req.fit or nil

  if not fit then return { ok = false, error = "a wallpaper fills or is centred" } end

  wall_fit = fit
  wallpaper_place()
  add_damage(0, 0, W, H)
  print("wm: the wallpaper " .. (fit == "fill" and "fills the screen" or "is centred"))

  return { ok = true }
end

--
-- **A new scale, with windows open** (`roadmap.md` 5z, `ui.md` 16.18) -
-- Appearance's slider, let go. The chrome and the faces follow the scale;
-- every window keeps its size and place in points, so its pixels change:
-- one drawn by commands gets a surface of the new size and is told to draw
-- again, one drawing its own pixels is stretched to its new place, the
-- strip keeps its height in points and the desktop the room it leaves. A
-- full-screen window is the screen at any scale and is left alone.
--
-- Said in the log per window, in its new pixels, since nothing else on the
-- screen names a size.
--
function scale.rescale(pct)
  local old = scale.pct

  scale.pct = pct
  scale.chrome()
  apply_fonts(theme.fonts)

  for _, win in ipairs(windows) do
    if win.pct == old then
      local lx, ly = scale.pt(win.x, old), scale.pt(win.y, old)

      win.pct = pct
      damage_window(win)

      if win.strip then
        swap_surface(win, (win.strip == "bottom" and win.floating)
                          and scale.px(scale.pt(win.w, old), pct) or W,
                     scale.px(scale.pt(win.h, old), pct))

        if win.strip == "bottom" then place_bottom(win) end
      elseif win.backdrop then
        -- Sized from what the strip leaves, below.
      elseif win.shared then
        if win.menubar then
          win.menubar.h = strips.height()
          win.menubar.surface:free()
          win.menubar.surface = gfx.surface{ w = scale.px(win.src_w, pct),
                                             h = win.menubar.h }
        end

        win.w = scale.px(win.src_w, pct)
        win.h = scale.px(win.src_h, pct) + strips.below(win)

        if win.menubar then strips.paint(win) end
      else
        resize_window(win, scale.px(scale.pt(win.w, old), pct),
                      scale.px(scale.pt(win.h, old), pct))
      end

      if not (win.strip or win.backdrop or win.popup) then
        win.x = math.min(math.max(scale.px(lx, pct), KEEP - win.w), W - KEEP)
        win.y = math.min(math.max(scale.px(ly, pct), OUT.top_of(win)), H - KEEP)
      end

      damage_window(win)
      print(("wm: rescaled %s to %dx%d"):format(tostring(win.title),
                                                 win.w, win.h))
    end
  end

  recount_strips()

  local now = theme.current()

  for _, win in ipairs(windows) do
    post(win, { type = "theme", palette = now, desktop = theme.desktop,
                fonts = theme.fonts })
  end

  add_damage(0, 0, W, H)
  print("wm: scale " .. pct)
end

handlers.scale = function(req)
  local pct = math.tointeger(tonumber(req.pct) or 0)

  if not pct or not scale.valid(pct) then
    return { ok = false, error = "a scale is 100, 110, 120, 135, 150, 175 "
                                 .. "or 200 per cent" }
  end

  if pct ~= scale.pct then scale.rescale(pct) end

  return { ok = true, pct = scale.pct }
end

--
-- Preferences, telling the manager what the power button or the Super key
-- now does. The fields are the files' own words, and anything else is
-- ignored rather than stored.
--
handlers.keys = function(req)
  -- Focus following the pointer, and after how long (`wm/pointer.lua`).
  if req.focus_follows ~= nil then
    OUT.focus_follows = req.focus_follows == true
    PT.dwell = nil
    print("wm: focus follows the pointer " .. (OUT.focus_follows and "on" or "off"))
  end

  if math.type(req.focus_delay_ms) == "integer" then
    OUT.focus_delay_ms = math.max(250, math.min(3000, req.focus_delay_ms))
  end

  for _, pair in ipairs({ { "power", { off = 1, menu = 1, nothing = 1 } },
                          { "super", { menu = 1, nothing = 1 } } }) do
    local v = req[pair[1]]

    if v ~= nil and pair[2][v] then OUT.keys[pair[1]] = v end
  end

  return { ok = true }
end

handlers.theme = function(req)
  if req.palette then
    local ok, err = theme.apply(req.palette)

    if not ok then
      return { ok = false, error = tostring(err) }
    end

    --
    -- **Said, because nothing said it.** A theme request is a person
    -- pressing something, and this handler answered every one of them in
    -- silence - so a look that was chosen and a look that was never sent
    -- read the same in the log. Diego, on the ThinkCentre M700 with
    -- Preferences open: "the preferences pane that is usable but does
    -- nothing to the system", and the log could not tell him which half of
    -- the path was missing.
    --
    -- The window colour rather than a name, because a palette arrives as a
    -- table of resolved colours and not as the word somebody picked - see
    -- the note below about why a name stopped working.
    --
    print(("wm: theme applied, window #%06x desktop #%06x")
          :format((theme.window or 0) & 0xffffff,
                  (theme.desktop or 0) & 0xffffff))

    -- A look's own corner and shadow (Night's are rounder), in force at
    -- once rather than at the next scale change.
    OUT.corner = OUT.want_corner and scale.px(theme.metrics.corner or 0) or 0
    OUT.shadow = OUT.want_shadow and scale.px(theme.metrics.shadow or 0) or 0
    add_damage(0, 0, W, H)
  end

  -- The desktop colour is chosen separately from the palette it sits with,
  -- so a light theme over a dark desktop is a thing somebody can have.
  if req.desktop then
    theme.override { desktop = req.desktop }
  end

  --
  -- **Rounded corners and shadows, each on or off.**
  --
  -- Diego, 23 September 2026, after running it: "i can already tell the
  -- drop shadows are super expensive so put them in an optional appearance
  -- menu option", and the same for the corners.
  --
  -- He is right about the cost and it is worth writing down where it comes
  -- from. A shadow is a band around the window - about `2 * spread *
  -- (w + h)` pixels - and every one of them is a distance and an alpha
  -- blend, redrawn whenever anything under it changes. A window of 800 by
  -- 520 with a 14-pixel shadow is some thirty-eight thousand blended pixels
  -- a frame, against a blit of the window itself which is a memcpy. Under
  -- TCG that is felt immediately; on the ThinkPad it is not. **Neither is a
  -- reason to decide for somebody**, which is exactly why these are
  -- settings rather than a number somebody like me picked.
  --
  -- Off is the default for the shadow and on for the corner, because one
  -- costs a band per frame and the other costs 256 pixels.
  --
  if req.corner ~= nil then
    OUT.want_corner = req.corner and true or false
    OUT.corner = OUT.want_corner
                 and scale.px(theme.metrics.corner or 0) or 0
    add_damage(0, 0, W, H)
  end

  if req.shadow ~= nil then
    OUT.want_shadow = req.shadow and true or false
    OUT.shadow = OUT.want_shadow
                 and scale.px(theme.metrics.shadow or 0) or 0
    add_damage(0, 0, W, H)
  end

  -- The font travels with the palette, because they are the same decision
  -- from the user's side and arrive from the same window. Applied here and
  -- forwarded, so a window drawing its own pixels changes too.
  local font_why = apply_fonts(req.fonts)

  --
  -- The whole palette, not its name.
  --
  -- A name only works while every window already holds the same palettes,
  -- which was true when two were compiled in and stopped being true the
  -- moment a theme could be a file. `theme.apply` takes either, so this is
  -- the one line that had to change.
  --
  local now = theme.current()

  -- A look with no title bars takes them off, and one with them puts them
  -- back - and each window is told which it has, so its header draws or
  -- stops drawing the room for the three.
  for _, win in ipairs(windows) do
    OUT.rehead(win)
    post(win, { type = "theme", palette = now, desktop = theme.desktop,
                fonts = theme.fonts, headed = win.headed or false })
  end

  -- The menu bars this process draws above direct windows wear it too.
  for _, win in ipairs(windows) do
    if win.menubar then strips.paint(win) end
  end

  -- Menus wear the theme too, and they are not in `windows`.
  for _, m in ipairs(menus) do
    post(by_handle[m.owner], { type = "theme", palette = now,
                               desktop = theme.desktop, fonts = theme.fonts })
  end

  add_damage(0, 0, W, H)

  --
  -- `held` is what this process actually loaded, against `fonts`, which is
  -- what it hands to applications. They are the same table's worth of
  -- information only when nothing went wrong, and a desktop where they
  -- differ draws every window's text in a face its application never
  -- measured - so the difference is worth being able to ask about rather
  -- than only to look at.
  --
  return { ok = true, palette = theme.name, desktop = theme.desktop,
           fonts = theme.fonts, held = in_force, font_why = font_why,
           held_why = startup_font_why }
end

--
-- Whether a window can be resized at all.
--
-- One that draws its own pixels can when it said it can (`resizable`, at
-- open). Its buffers are a region the *application* allocated, sized for
-- exactly its dimensions, so this process cannot make them bigger: it
-- changes the frame, goes on showing the old picture stretched into it,
-- tells the application, and the application hands over a region the new
-- size (`handlers.surface`, `roadmap.md` 6zz e). An application that did
-- not say so may not be listening for that, and gets no grip - which is the
-- honest way to say no: a control that is not there cannot be pressed and
-- be ignored.
--
function resizable(win)
  --
  -- Chrome is never resized, and saying so *here* rather than at the two
  -- call sites is the point.
  --
  -- This function is asked twice about every window: once by the compositor,
  -- which draws a grip on anything that answers yes, and once by the pointer,
  -- which starts a drag on the same corner. The bar across the top answered
  -- yes - it sends drawing commands rather than holding a shared surface, so
  -- it looked like an ordinary application - and grew a sizing grip in its
  -- own bottom-right corner, sixteen pixels of window chrome on a thing that
  -- is not a window.
  --
  -- The `bare` predicate in the composite loop learned the same lesson an
  -- hour earlier and I put it in one place there; this is the other half of
  -- it. A window with no frame has no corner to pull.
  --
  if win.backdrop or win.strip or win.popup or win.tip then return false end

  return win.shared == nil or win.resizes_itself == true
end

--
-- A new size, and a new surface to go with it.
--
-- The old contents are not carried over. It would be a blit and it would be
-- wrong: the application lays out for the size it is given, so what is on
-- screen a moment after a resize should be what it drew for the new size,
-- not the old picture stretched or cropped underneath it. The window is
-- filled with its background and the application is told; the next frame it
-- sends is the right one.
--
function resize_window(win, w, h)
  w = math.floor(w)
  h = math.floor(h)

  -- Not smaller than a window, and not bigger than the screen it has to fit
  -- inside along with its own decoration.
  local most_w, most_h = OUT.room(win)

  if w < scale.MIN_W then w = scale.MIN_W end
  if h < scale.MIN_H then h = scale.MIN_H end
  if w > most_w then w = most_w end
  if h > most_h then h = most_h end

  if w == win.w and h == win.h then return false end

  return swap_surface(win, w, h)
end

local function to_focused(c)
  post(focused_window(), { type = "key", code = c })
end

--
-- A key transition, to whichever window has the focus.
--
-- A *second* event type rather than a field on `key`, and additive on
-- purpose: everything that exists reads `key` and gets characters, exactly
-- as it did. An application that needs to know a key is held reads
-- `rawkey` instead and ignores the character stream. Nothing has to be
-- changed to keep working, and nothing that wants this has to reconstruct
-- it from characters, which is impossible anyway.
--
-- `code` is the board's own keycode, undecoded. This process knows which
-- window is listening; it does not know what a keycode means, and an
-- application that wants "the W key" is asking about the key rather than
-- about a letter.
--
-- Only the focused window, which is the whole reason this goes through
-- here: a process that could ask the kernel directly could watch every key
-- in the system. This one already owns input and already knows who is in
-- front.
--
-- **A key's release goes where its press went**, and so does its repeat,
-- whoever is in front by then. An application that reads `rawkey` holds
-- which keys are down, and a release sent to another window leaves one
-- down in it for ever: Cafesa3D's Ctrl O opens the Open panel, which takes
-- the focus before Ctrl comes up, and the next Z after the panel closed
-- was Ctrl Z - undoing the scene just opened (`testing.md` 18.216). A game
-- whose W is held while Super Tab moves the focus walks on for the same
-- reason. A window closed meanwhile is posted to and never reads it.
--
-- **`at` is the counter when this read the key**, so a window that plays a
-- note from it can say how long the sound took (`roadmap.md` 4i).
--
--
-- **A picture of the screen, on a key** (Diego, 3 October 2026: "i need a
-- way to grab screenshots", "so i can send you screenshots without taking
-- photos from my mobile"): Print Screen - a full-size keyboard's - and
-- Control Alt 1, his of 22 September; Super Shift 3 is with the Super
-- bindings. The key starts `screenshot`, which borrows the screen and saves
-- it, so this loop never waits on the encoding or the disk. Taken here, so
-- no window sees the press; Control and 1 type nothing anyway (`keys.c`).
--
local KEY_SYSRQ, KEY_1 = 99, 2

local function capture_key(code, down)
  local held = OUT.chord.held
  local chord = code == KEY_1 and (held[29] or held[97]) and (held[56] or held[100])

  if code ~= KEY_SYSRQ and not chord then return false end

  if down then
    handlers.launch{ program = "screenshot" }
    print("wm: a screenshot asked for")
  end

  return true
end

OUT.capture_key = capture_key

local raw_to_focused

do
  local pressed_in = {}                      -- a key's code -> its press's window

  function raw_to_focused(code, down)
    local win = pressed_in[code] or focused_window()

    pressed_in[code] = down and win or nil
    post(win, { type = "rawkey", code = code, down = down, at = sys.ticks() })
  end
end

--
-- **The volume keys are the system's**, taken here before any window sees
-- them, as Super is: a key that changes the whole machine's volume is not an
-- application's to receive, and a game holding the focus must not swallow
-- it. The keyboard driver maps them - e0 20, e0 2e and e0 30, measured
-- through QEMU's PS/2 keyboard (`testing.md` 18.92) - and they arrive here as
-- raw events only, never as characters.
--
-- Volume up and down move the master by a sixteenth, and unmute, as every
-- laptop does; mute silences everything and keeps the level, so unmuting
-- comes back to it (`audio.c`'s `master_muted`).
--
-- **A call from the key path, and why this one cannot deadlock.** Nothing
-- may block in here: a synchronous call from the key handler to a window
-- once waited on a window that was waiting on this loop, and the desktop
-- stopped. `/Devices/audio` is the audio server, which never sends to the
-- window manager, so there is no cycle for this to close - and it answers
-- a `set` without touching the device.
--
-- Said in the log, and shown on the screen by the level bar (`osd.show`).
--
local KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP = 113, 114, 115
local VOLUME_STEP = 16                  -- a sixteenth of 256
local have_audio, audio = pcall(use, "/Kosmos/Libraries/audio.lua")

local function volume_key(code, down)
  if code ~= KEY_MUTE and code ~= KEY_VOLUMEDOWN and code ~= KEY_VOLUMEUP then
    return false
  end

  if not down then return true end      -- the release is the system's too

  local which = (code == KEY_MUTE) and "mute"
                or (code == KEY_VOLUMEUP) and "up" or "down"

  local now = have_audio and audio.stats()

  if not now then
    print("wm: volume " .. which .. " - no sound device to answer")
    return true
  end

  local ok, why

  if code == KEY_MUTE then
    ok, why = audio.set{ master_muted = not now.master_muted }
  else
    local step = (code == KEY_VOLUMEUP) and VOLUME_STEP or -VOLUME_STEP
    local level = math.max(0, math.min(256, now.master + step))

    ok, why = audio.set{ master = level, master_muted = false }
  end

  local after = ok and audio.stats() or now

  if not ok then
    print("wm: volume " .. which .. " - " .. tostring(why))
  elseif after.master_muted then
    print(("wm: volume muted, %d of 256 kept"):format(after.master))
  else
    print(("wm: volume %s, %d of 256"):format(which, after.master))
  end

  --
  -- **Drawing the bar must not take the key with it.** The volume has
  -- already changed by here; the first version called `gfx.surface(w, h)`,
  -- which is not its shape (a table, `{ w =, h = }`), and the error that
  -- raised took the window manager's key handling down with it - the next
  -- two presses were never seen. Nothing may break in the key path, so a
  -- failure to draw is said and the keys go on working.
  --
  if ok then
    local drawn, oops = pcall(osd.show, "sound", after.master / 256,
                              after.master_muted)

    if not drawn then
      print("wm: the level bar could not be drawn: " .. tostring(oops))
    end
  end

  return true
end

--
-- **The keys the firmware reports rather than the keyboard**: the
-- brightness keys and the power button, which `hal/pc/ec.c` hears as ACPI
-- events once the machine is in ACPI mode and hands on as key codes - F5
-- and F6 on the ThinkPad, and its power button.
--
-- Brightness is `volume_key`'s arithmetic on the backlight driver: sixteen
-- steps of 0 to 256, snapped to the nearest step first so a level set some
-- other way comes back onto the grid, and the Display bar shown with what
-- the driver says it now holds. `/Devices/backlight` is a driver that never
-- sends to this process, so the call cannot close a cycle - the rule
-- `volume_key` states for `/Devices/audio`.
--
-- **The power button shuts down**, exactly as the Deskbar's Shut Down does:
-- every window told, then the machine off. Holding it for four seconds
-- still forces the machine off, because that is the chipset's.
--
-- One table, for the reason `osd` is one.
--
local machine_keys = {
  POWER = 116, BRIGHTNESSDOWN = 224, BRIGHTNESSUP = 225,
  STEP = 16,
}

machine_keys.have_backlight, machine_keys.backlight =
  pcall(use, "/Kosmos/Libraries/backlight.lua")

function machine_keys.brightness(up)
  local which = up and "up" or "down"
  local b = machine_keys.have_backlight and machine_keys.backlight
  local now, why

  if b then now, why = b.get() else why = "no backlight driver" end

  if not now then
    print("wm: brightness " .. which .. " - " .. tostring(why))
    return
  end

  local step = machine_keys.STEP
  local at = (now + step // 2) // step + (up and 1 or -1)
  local after

  after, why = b.set(math.max(0, math.min(256, at * step)))

  if not after then
    print("wm: brightness " .. which .. " - " .. tostring(why))
    return
  end

  print(("wm: brightness %s, %d of 256"):format(which, after))

  local drawn, oops = pcall(osd.show, "display", after / 256, false)

  if not drawn then
    print("wm: the level bar could not be drawn: " .. tostring(oops))
  end
end

function machine_keys.take(code, down)
  if code == machine_keys.POWER then
    if down then
      -- Shut down, the menu - which has Restart and Shut Down in it, so it
      -- is the "ask" - or nothing, as Preferences' Power page says.
      if OUT.keys.power == "nothing" then
        print("wm: the power button - set to do nothing")
      elseif OUT.keys.power == "menu" then
        print("wm: the power button - the menu")
        OUT.open_kosmos_menu()
      else
        print("wm: the power button - shutting down")
        handlers.power({ action = "off" })
      end
    end

    return true
  end

  if code ~= machine_keys.BRIGHTNESSUP
     and code ~= machine_keys.BRIGHTNESSDOWN then
    return false
  end

  if down then machine_keys.brightness(code == machine_keys.BRIGHTNESSUP) end

  return true
end

--------------------------------------------------------------------------
-- Closing, and ending what will not close.
--
-- Two steps, because they are two different things. Asking a window to
-- close is a message: an application that is listening saves what it was
-- doing and goes, which is the only version that can ever be correct.
-- Ending the process is what happens when nobody is listening, and it
-- cannot be polite because there is nobody to be polite to.
--
-- The grace period is what separates them. A second is far longer than a
-- healthy application needs to notice a message - a window polls its
-- events every pass - and short enough that a wedged one does not sit
-- there being clicked at.
--
-- The kernel does the ending, and only for a child: `sys.kill` is the
-- authority a parent already has by being able to wait for one. This
-- process started these programs, so this process may end them, and
-- nothing else may.
--------------------------------------------------------------------------

OUT.close_grace = COUNTER_HZ               -- a second, in counter units

--
-- How long a window may say nothing before this asks whether it is still
-- alive. Five seconds, in counter units.
--
local silent_grace = OUT.close_grace * 5

--------------------------------------------------------------------------
-- Ending an application, on somebody else's behalf.
--
-- `SYS_KILL` lets a parent end a child and nothing else, which is right and
-- is why `procs` could not do it: it did not start anything. The process
-- that *did* start every application is this one, so this is where the
-- authority lives - and asking whoever holds it is the whole shape of the
-- system rather than a workaround for it.
--
-- Only a process with a window here. A server was started by init and this
-- has no business ending it; init would be the one to ask, and nothing has
-- a reason to yet.
--------------------------------------------------------------------------
--
-- Start, stop, or read the profile. `/Kosmos/Programs/frames.lua` is the client.
--
-- Reading does not stop the measurement and does not reset it, so a long
-- run can be sampled while it happens; `on = true` is what clears the
-- counters. Times are in counter ticks, because this process has no
-- business deciding what a tick is worth - `/Devices/cpu` says, and the
-- program that prints the report is where that division belongs.
--
handlers.profile = function(req, who)
  if req.on == true then
    P.reset(COUNTER_HZ)
    P.pass_busy = 0
    P.profiling = true

    --
    -- `run_for` counter ticks and then the answer, which is how `frames`
    -- waits without spinning. Deferred exactly as `poll` is: the reply is
    -- this process's to send later, and until it does the client is a
    -- blocked thread costing nothing.
    --
    if req.run_for then
      P.waiting = { who = who,
                          deadline = sys.ticks() + req.run_for }
      return DEFER
    end

    return { ok = true, profiling = true }
  elseif req.on == false then
    P.profiling = false
    P.waiting = nil
    return { ok = true, profiling = false }
  end

  if not P.prof then
    return { ok = false, error = "nothing measured yet: start it with on=true" }
  end

  return P.report()
end

handlers.end_process = function(req)
  local pid = tonumber(req.pid)

  if not pid then
    return { ok = false, error = "no such process" }
  end

  for _, win in ipairs(windows) do
    if win.pid == pid then
      -- Asked first, as the close box does: a window that is listening
      -- tidies up and goes, and one that is not is taken by force on a
      -- later pass.
      win.closing = sys.ticks() + OUT.close_grace
      post(win, { type = "close" })

      if sys.kill(pid) then
        return { ok = true, ended = true }
      end

      return { ok = true, ended = false }
    end
  end

  return { ok = false,
           error = "that is not an application this desktop started" }
end

--
-- Windows whose process has gone, and the reason this exists.
--
-- **A dead application's window stayed on the screen for ever**, fully
-- drawn, exactly as it was when the process died - because the compositor
-- owns the pixels and nothing ever told this loop otherwise. It is not a
-- cosmetic leak: the window, its surface and its handle are all still held,
-- and the desktop is showing something that is not there.
--
-- It is also the reason an afternoon went into a bug that did not exist. A
-- window that has died and a window that is busy look *identical* from the
-- outside, so the first question anyone asks - "is it still running?" - had
-- no way to be answered by looking. Now it does: if the window is gone, it
-- had died; if it is still there, it is working.
--
-- Only asked about windows that have said nothing for five seconds, and the
-- process list is fetched at most once per pass, because a message per
-- window per pass to answer a question that is almost always "yes" is the
-- kind of cost that turns a diagnostic into a regression.
--
local function collect_dead(now)
  local suspect = false

  for i = 1, #windows do
    local w = windows[i]

    if w.pid and w.last_poll and (now - w.last_poll) > silent_grace then
      suspect = true
      break
    end
  end

  if not suspect then return end

  local list = sys.processes and sys.processes() or nil

  if not list then return end

  local alive = {}

  for i = 1, #list do alive[list[i].id] = true end

  for i = #windows, 1, -1 do
    local win = windows[i]

    if win.pid and win.last_poll and not alive[win.pid]
       and (now - win.last_poll) > silent_grace then
      -- Said out loud, always - not only under `trace`. A window vanishing
      -- on its own is the desktop doing something a person did not ask for,
      -- and the reason belongs somewhere they can read it afterwards.
      print(("wm: %s stopped answering and its process is gone"):format(
            tostring(win.title)))

      handlers.close{ window = win.handle }
    end
  end
end

local function collect_closing()
  local now = sys.ticks()

  collect_dead(now)

  for i = #windows, 1, -1 do
    local win = windows[i]

    if win.closing and now > win.closing then
      -- It had its chance. Take the window off the screen first, so the
      -- desktop is usable the instant this is decided, and then end the
      -- process behind it.
      handlers.close{ window = win.handle }

      if win.pid then
        sys.kill(win.pid)
      end

      --
      -- **Said where a person sees it** (`roadmap.md`, *Notifications*): an
      -- alert, from the system, as the drawing has it. The notification
      -- server answers at once and holds nothing back, so this call is not a
      -- wait the desktop can be caught in.
      --
      pcall(function()
        use("/Kosmos/Libraries/notify.lua").post{
          title = tostring(win.title) .. " stopped answering",
          body = "It was asked to close and did not, so it was ended.",
          alert = true }
      end)
    end
  end
end

--
-- The menu under a point, if any. Front to back, like windows.
--
local function menu_at(x, y)
  for i = #menus, 1, -1 do
    local m = menus[i]

    if x >= m.x and x < m.x + m.w and y >= m.y and y < m.y + m.h then
      return m
    end
  end

  return nil
end

--
-- The popup on the screen, if there is one: the frontmost, since a press
-- outside it closes it and the next one closes the next.
--
function OUT.popup()
  for i = #windows, 1, -1 do
    local w = windows[i]

    if w.popup and not w.hidden then return w end   -- a tip is not one
  end

  return nil
end

--
-- **The pointer over the dock, told to it** (Diego, 3 October: "hovering
-- over the icons in the dock app icons should tell the name of the app").
-- A window hears the pointer with no button held only while it has the
-- focus (`handlers.track`), and the dock never has it - so the dock, if it
-- asked to track, hears it while the pointer is over it, focus or not, and
-- one move at -1, -1 when the pointer leaves it.
--
function OUT.hover_dock(x, y, skip)
  for i = #windows, 1, -1 do
    local d = windows[i]

    if d.strip == "bottom" and d.tracking and d ~= skip then
      local inside = not d.hidden and x >= d.x and x < d.x + d.w
                     and y >= d.y and y < d.y + d.h

      if inside then
        d.hovering = true
        post(d, { type = "mouse", action = "move", hover = true,
                  x = x - d.x, y = y - d.y })
      elseif d.hovering then
        d.hovering = nil
        post(d, { type = "mouse", action = "move", hover = true, x = -1, y = -1 })
      end

      return
    end
  end
end

-- That popup, when a press at `x, y` lands outside it - which closes it.
function OUT.popup_outside(x, y)
  local pop = OUT.popup()

  if pop and not (x >= pop.x and x < pop.x + pop.w and y >= pop.y and y < pop.y + pop.h) then
    return pop
  end

  return nil
end

--
-- Every menu, gone.
--
-- Called when a press lands outside them, when the owner is raised away,
-- and when the owner dies. It goes through `handlers.close` rather than
-- deleting the record, so a menu leaves by the same path a window leaves
-- by - which is the reason for building a menu as a window in the first
-- place and would be wasted by tearing one down by hand here.
--
--
-- **And the owner is told which went** (`menus_gone`): it keeps a list of
-- what it has open, and a list nobody corrected is a Kosmos button that
-- stayed lit after its menu was dismissed - the dock's, 3 October. The
-- handles rather than "all of them", so a menu the owner opened after this
-- and before reading it is not forgotten with them.
--
local function dismiss_menus(owner_handle)
  local n = #menus
  local gone = {}

  for i = n, 1, -1 do
    local m = menus[i]

    if owner_handle == nil or m.owner == owner_handle then
      local list = gone[m.owner or 0] or {}

      gone[m.owner or 0] = list
      list[#list + 1] = m.handle

      -- pcall because this runs from `pointer_pass`, which the main loop
      -- calls bare rather than inside the pcall that wraps handlers. An
      -- error here would take the desktop with it.
      pcall(handlers.close, { window = m.handle })
    end
  end

  for owner, list in pairs(gone) do
    local win = by_handle[owner]

    if win then post(win, { type = "menus_gone", menus = list }) end
  end

  return #menus < n
end

--
-- Which window a point is in, front to back.
--
-- A minimised one is skipped. It keeps its place in the stack, its surface
-- and its contents - an application drawing into it is not interrupted, the
-- same as one buried under another window - but it is not on the screen,
-- and a window you cannot see must not be a window you can click.
--
local function window_at(x, y)
  -- A banner first, which is drawn in front of everything (`OUT.order`).
  for i = #windows, 1, -1 do
    local win = windows[i]

    if win.banner and not win.hidden
       and x >= win.x and x < win.x + win.w and y >= win.y and y < win.y + win.h then
      return win, win.x, win.y
    end
  end

  -- Then the dock, which is in front of every window.
  for i = #windows, 1, -1 do
    local win = windows[i]

    if win.strip == "bottom" and not win.hidden
       and x >= win.x and x < win.x + win.w and y >= win.y and y < win.y + win.h then
      return win, win.x, win.y
    end
  end

  for i = #windows, 1, -1 do
    local win = windows[i]
    local fx, fy, fw, fh = frame_of(win)

    if not win.hidden and not win.tip
       and x >= fx and x < fx + fw and y >= fy and y < fy + fh
       and not (y < fy + OUT.TAB_H and win.kind ~= "menu" and not win.backdrop
                and not win.strip and not win.popup and x >= fx + tabs.width(win)) then
      return win, fx, fy
    end
  end

  return nil
end

--
-- **A window's shadow, in the rectangle being composed**, drawn just before
-- the window itself so everything above it paints over it.
--
-- Here and not in `draw_window`, because the shadow lies *outside* the
-- frame and `draw_window` is handed only the frame's visible part - which
-- held a shadow clipped to it to nothing at all (Diego, 24 September: "the
-- new drop shadow does not work"). Clipped to all of `r` instead, and cast
-- by a window whose frame is hidden too, since its shadow can still show
-- beside whatever hides it.
--
-- A bare window casts none: the backdrop is the thing everything sits on
-- and the strip is chrome, and a shadow under either would be a dark band
-- across a desktop that has nothing above it. Nor does a menu or a window
-- that fills the screen.
--
function OUT.cast_shadow(win, r)
  if OUT.shadow <= 0 or win.hidden or win.backdrop or win.strip
     or win.fullscreen or win.kind == "menu" or win.tip then
    return
  end

  local sx, sy, sw, sh = OUT.shadowed(win)

  if sx >= r.x + r.w or sx + sw <= r.x or sy >= r.y + r.h
     or sy + sh <= r.y then
    return
  end

  local fx, fy, fw, fh = frame_of(win)

  --
  -- **Deeper in front, for a window with no tab** (`roadmap.md` 6zj): a
  -- tab's colour said which window has the focus, and without one the
  -- three and the shadow say it - the front one's as it always was, the
  -- rest lighter. The same spread, so what a shadow damages is unchanged.
  --
  local alpha = (win.headed and win ~= focused_window()) and 55 or nil

  back:shadow(fx, fy, fw, fh, OUT.corner, OUT.shadow, alpha,
              r.x, r.y, r.w, r.h)
end

-- The window whose contents are under a point, for the wheel: none over a
-- title bar or a menu.
function OUT.wheel_target(x, y)
  local win = window_at(x, y)

  if not win or win.kind == "menu" or y < win.y then return nil end

  return win
end

-- The window whose title-bar three the pointer is over (`OUT.light`).
-- Here rather than beside them because it asks `window_at`, which is here.
function OUT.boxes_under(x, y)
  local win = window_at(x, y)

  if not win or win.kind == "menu" or win.backdrop or win.strip
     or win.fullscreen or win.pinned or win.popup or win.tip then
    return nil
  end

  local bx, by, bw, bh = OUT.boxes_rect(win)

  if bx and x >= bx and x < bx + bw and y >= by and y < by + bh then
    return win
  end

  return nil
end

--
-- The pointer's own bookkeeping, shared with the pointer's pass in
-- `/Kosmos/Libraries/wm/pointer.lua`, which counts its transitions in it.
--
--   said     how many button transitions have been logged (bounded)
--   replay   the one table every replayed transition is written through,
--            because a table per click per pass is the allocation habit
--            this loop cannot afford
--   lost     how often a dropped transition has been mentioned
--
local pointer_log = { said = 0, replay = {}, lost = 0 }

-- The pointer's pass - `/Kosmos/Libraries/wm/pointer.lua` (`roadmap.md`
-- 6zn). Every function handed here has its body by now.
--
local pointer_pass = use("/Kosmos/Libraries/wm/pointer.lua"){
  PT = PT,
  OUT = OUT,
  W = W,
  H = H,
  COUNTER_HZ = COUNTER_HZ,
  add_damage = add_damage,
  boxes_x = boxes_x,
  by_handle = by_handle,
  cursor_size = cursor_size,
  damage_outline = damage_outline,
  dismiss_menus = dismiss_menus,
  focused_window = focused_window,
  frame_of = frame_of,
  maximise = maximise,
  menu_at = menu_at,
  menus = menus,
  minimise = minimise,
  move_window = move_window,
  pointer_log = pointer_log,
  post = post,
  raise = raise,
  resizable = resizable,
  resize_window = resize_window,
  scale = scale,
  strips = strips,
  window_at = window_at,
}

-- Keys the window manager keeps - `/Kosmos/Libraries/wm/keys.lua`
-- (`roadmap.md` 6zn).
--
local key = use("/Kosmos/Libraries/wm/keys.lua"){
  EDIT_KEYS = EDIT_KEYS,
  KEY_CTRL_C = KEY_CTRL_C,
  KEY_ESC = KEY_ESC,
  KEY_PREFIX = KEY_PREFIX,
  KEY_TAB = KEY_TAB,
  OUT = OUT,
  focused_window = focused_window,
  handlers = handlers,
  move_window = move_window,
  post = post,
  raise = raise,
  to_focused = to_focused,
  windows = windows,
}

--------------------------------------------------------------------------
-- Start whatever was asked for, each in a process of its own, each handed
-- this endpoint under a name. They cannot reach it any other way and they
-- cannot pass it anywhere this process did not.
--------------------------------------------------------------------------

-- `name` or `name:arguments`, comma separated. The colon is so a program
-- started here can be given something to work on - `wm gallery,setprop:...`
-- - without this having to understand a shell's quoting rules, which it is
-- not and should not become.
--
-- **With nothing asked for, a desktop**: the Tracker that draws it and the
-- Deskbar that starts things.
--
-- It used to be the Deskbar alone, on the reasoning that a desktop with no
-- way to start anything is one you can only look at. True, and it left out
-- the other half: a desktop with nothing *behind* the windows is one you
-- cannot put anything on.
--
-- **The Tracker is the desktop**, which is BeOS's arrangement and Finder's
-- and Explorer's - one program is the file manager and the backdrop both,
-- and closing it closes the desktop. Here the backdrop was reachable only
-- by knowing to type `wm tracker:desktop`, so nobody had one.
--
-- What that cost was not decoration. **The desktop is where a drag lands.**
-- With no backdrop the area behind the windows belongs to no window at all:
-- `window_at` finds nothing there and a file dragged out of a Tracker
-- window is answered "nothing there takes a drop", which reads exactly like
-- drag and drop being unimplemented.
--
-- Only the empty case changes. Every harness and every scripted run names
-- what it wants, so what they start is what they started before.
--
local wanted = tostring(args or ""):match("^%s*(.-)%s*$")

--
-- **The entries of that list**, split at each comma that is not inside
-- quotes: `writer:"/Home/My Pictures/a, b.write",deskbar` is two. What
-- follows a name's colon is that program's argument string as written,
-- quotes and all, and the program reads it with `files.words` like any
-- other (`testing.md` 18.414).
--
local function entries(list)
  local out, from, quoted, i = {}, 1, false, 1

  while i <= #list do
    local c = list:sub(i, i)

    if quoted and c == "\\" then
      i = i + 1                         -- `\"` and `\\` are not the end
    elseif c == '"' then
      quoted = not quoted
    elseif c == "," and not quoted then
      out[#out + 1] = list:sub(from, i - 1)
      from = i + 1
    end

    i = i + 1
  end

  out[#out + 1] = list:sub(from)

  return ipairs(out)
end

--
-- `trace` first, so it is a setting rather than a program: it has to be out
-- of the list before the loop below tries to start `/Kosmos/Programs/trace.lua`, and it
-- has to be *set* before the first window opens or the opening is the one
-- thing the trace misses.
--
do
  local rest = {}

  for _, entry in entries(wanted) do
    entry = entry:match("^%s*(.-)%s*$")

    if entry == "trace" then
      TRACE = true
    elseif entry ~= "" then
      rest[#rest + 1] = entry
    end
  end

  wanted = table.concat(rest, ",")

  if wanted == "" then wanted = "notifications,desktop,deskbar" end

  if TRACE then
    print(("wm: tracing, screen %dx%d"):format(W, H))
  end
end

--
-- **A viewer's pointer and keys, on the lent endpoint** (`vncd`, 7b).
--
-- The pointer is where on the screen, in pixels - which is what a viewer
-- sends and what this process speaks - through the same `pointer_pass` a
-- mouse goes through, with the screen as its range. A press, a drag and
-- the wheel (`wheel`, notches) mean what they mean from a mouse.
--
-- A key is the key's code, down or up, as a keyboard's event, and the
-- characters it made - what the kernel makes of a keyboard's keys, which
-- `vncd` makes of a viewer's - through the same two paths the console's
-- input takes: the event to the chord, the volume and machine keys and the
-- focused window, and each character to `key`. The desktop cannot tell it
-- from the keyboard, which is the point.
--
remote.handlers.pointer = function(req)
  local x = math.floor(tonumber(req.x) or -1)
  local y = math.floor(tonumber(req.y) or -1)
  local buttons = math.floor(tonumber(req.buttons) or 0)

  if x < 0 or y < 0 or x >= W or y >= H or buttons < 0 or buttons > 7 then
    return { ok = false, error = "a pointer is inside the screen, with three buttons" }
  end

  pointer_pass({ x = x, y = y, buttons = buttons,
                 wheel = math.max(-8, math.min(8, math.floor(tonumber(req.wheel) or 0))),
                 min_x = 0, max_x = W - 1, min_y = 0, max_y = H - 1 })

  -- The mouse at rest must not take the pointer straight back (the loop).
  remote.moved = true

  return { ok = true }
end

remote.handlers.key = function(req)
  local code = math.floor(tonumber(req.code) or 0)
  local chars = type(req.chars) == "string" and req.chars or ""

  if code < 0 or code > 0x3FF or #chars > 16 then
    return { ok = false, error = "a key is a code to 1023 and a few characters" }
  end

  if code > 0 then
    local down = req.down == true

    OUT.chord.held[code] = down or nil

    if (code == 125 or code == 126) and down then
      OUT.chord.super_moved = false
    end

    if not volume_key(code, down) and not machine_keys.take(code, down)
       and not capture_key(code, down) then
      raw_to_focused(code, down)
    end
  end

  for i = 1, #chars do
    key(chars:byte(i))
  end

  key(nil)                  -- the end of what was read (`wm/keys.lua`)

  return { ok = true }
end

for _, entry in entries(wanted) do
  entry = entry:match("^%s*(.-)%s*$")

  if entry ~= "" then
    local name, argument = entry:match("^([^:]+):(.*)$")
    name = name or entry

    local path = fs.program(name)
    local ok, err, id = run(path, argument or "", true,
                            { ["/Running/wm"] = ep })

    if not ok then
      print(("wm: could not start %s: %s"):format(path, tostring(err)))
    else
      -- Said even when it worked. "Started" and "drew a window" are two
      -- events and the gap between them is where a desktop that looks hung
      -- actually is, so both are in the log and the missing one names the
      -- program that never arrived.
      print(("wm: started %s as %s"):format(path, tostring(id)))
      pending_pid = id
    end
  end
end

--
-- **The screen's server, when the Servers window keeps it to start with the
-- machine** - started here and not by the shell with the others, because
-- this is what lends it the desktop, and a `vncd` the shell started would
-- hold nothing to show (`remote`, above).
--
do
  local kept = prefs.read("servers")
  local vnc = type(kept) == "table" and kept.vnc

  if type(vnc) == "table" and vnc.at_start == true then
    handlers.launch({ program = "vncd", wait = false,
                      args = tostring(math.floor(tonumber(vnc.port) or 5900)) })
  end
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

add_damage(0, 0, W, H)

--
-- How long a pass is allowed to sleep for, in *scheduler* ticks.
--
-- One of them, so this loop wakes a hundred times a second when nothing is
-- happening. An input interrupt cuts the sleep short, so this is not the
-- latency of a keystroke - it is only how often an idle desktop wakes up to
-- find there is still nothing to do.
--
-- Scheduler ticks and not the counter `sys.ticks()` returns. They differ by
-- a factor of six hundred thousand here, and passing one for the other asks
-- for a sleep of several hours - which looks exactly like the desktop
-- having hung, because it has.
--
--
-- One tick used to be written here as the literal `1`, which was fine for
-- as long as a tick was 10 ms and stopped being fine the moment it was not.
-- The tick rate is a kernel decision this program does not get a say in, so
-- it asks rather than assuming: a pass waits about ten milliseconds for
-- input, whatever that is in ticks today.
--
local PASS = math.max(1, ((sys.info() or {}).tick_hz or 100) // 100)

--
-- **How long this pass may sleep, which is not always `PASS`.**
--
-- Every window waiting on a poll has a deadline it asked for, and those are
-- answered once a pass. Sleeping the full `PASS` when one of them is due
-- sooner means answering it late - so a window that asked for four
-- milliseconds got eight, and the interval it named was not the interval it
-- got.
--
-- This is the second half of the unit fix above and useless without it:
-- while every deadline was already past, the soonest was always now, and
-- this would have returned zero for ever and turned the loop into a spin.
--
-- Nothing waiting means nothing to be late for, and the pass sleeps the
-- whole `PASS`. An idle desktop is still idle, which is what `PASS` is for.
--
local function sleep_for()
  local shortest = PASS
  local now = sys.ticks()
  local scale = counter_per_tick()

  -- An application being read off the disk is read a window a pass, so a
  -- pass that slept would be a load that crawled.
  if #launching > 0 then return 0 end

  --
  -- **Rounded up, and that is the difference between idle and a core at a
  -- hundred per cent** (`testing.md` 18.345). It was rounded down, so a
  -- deadline less than a tick away was a sleep of nought: the wait came
  -- straight back, the deadline had not passed, and round again - 68,000
  -- passes a second on the M700, for as long as any window polled every
  -- tick. A Terminal does, while a program runs in it, and on 2 October one
  -- was running `neofetch`, stuck on the network: the window manager and
  -- the console held a processor between them for as long as it waited.
  -- Rounded up, the pass sleeps to the tick the deadline falls in and is at
  -- most that tick late - and a tick is the unit the window asked in.
  --
  for i = 1, #waiting do
    local left = -((now - waiting[i].deadline) // scale)

    if left < shortest then
      shortest = left
    end
  end

  -- Never negative: a deadline already past is answered by this very pass,
  -- a few lines down, and sleeping through it would be the whole bug again.
  return math.max(0, shortest)
end

while OUT.running do
  passes = passes + 1
  flush_notes()

  --
  -- **Who is actually running, once a second, under `trace` only.**
  --
  -- Every instrument so far has asked "is this part stuck", and each time
  -- the answer was no: the compositor loops, the applications paint, the
  -- filesystem answers the shell. What none of them could say is where the
  -- processor is *going* - and on the first real machine the compositor
  -- managed nine passes a second while measuring four milliseconds per
  -- pass, which means it was not running for most of the time and neither
  -- was anything else that was being watched.
  --
  -- `ticks` is cumulative scheduler ticks charged to a process, so the
  -- difference between two of these lines is the share it took. That is the
  -- one question left and nothing in the system was answering it.
  --
  if TRACE and passes % 250 == 0 then
    local list = sys.processes and sys.processes() or nil

    for i = 1, (list and #list or 0) do
      local e = list[i]

      if (e.ticks or 0) > 0 then
        print(("wm: cpu %s(%s) ticks=%s state=%s"):format(
              tostring(e.name), tostring(e.id), tostring(e.ticks),
              tostring(e.state)))
      end
    end
  end
  -- 1. Input, always first - and this is where the pass sleeps if there is
  -- none. One call for keys and the pointer together, because the console
  -- has to be asked anyway and two round trips to learn nothing is one
  -- more than necessary.
  -- `heap` moves with the stage boundaries and `heap_at_start` does not:
  -- one is what the last stage ended with, the other is what the pass began
  -- with, and the collection check below needs the second.
  local t, heap, heap_at_start

  P.due()

  P.measuring = P.profiling

  if P.measuring then
    P.prof.passes = P.prof.passes + 1
    heap = collectgarbage("count")
    heap_at_start = heap
    t = sys.ticks()
  end

  step("wait")
  local input = fs.wait_input("/Devices/console", sleep_for()) or {}

  -- Idle, and charged as such: this is the loop asleep with nothing to do,
  -- and counting it as work would make an empty desktop look busy.
  if P.measuring then t, heap = P.charge("wait", t, heap, true) end

  step("keys")
  for _, c in ipairs(input.keys or {}) do
    key(c)
  end

  if input.keys and #input.keys > 0 then key(nil) end

  -- The same presses as transitions, for whoever wants them that way.
  for _, ev in ipairs(input.events or {}) do
    OUT.chord.held[ev.code] = ev.down or nil

    -- A fresh Super is a fresh chance to open the menu.
    if (ev.code == 125 or ev.code == 126) and ev.down then
      OUT.chord.super_moved = false
    end

    if not volume_key(ev.code, ev.down)
       and not machine_keys.take(ev.code, ev.down)
       and not capture_key(ev.code, ev.down) then
      raw_to_focused(ev.code, ev.down)
    end
  end

  if P.measuring then t, heap = P.charge("keys", t, heap) end

  step("messages")
  -- 2. Whatever the applications have asked for, and not one message more
  -- than has already arrived.
  while true do
    --
    -- The third value is a capability that came *with* the request, at
    -- whatever index the kernel put it in this process's table. Only `open`
    -- uses it, to receive the shared region a direct window draws into -
    -- and forgetting to take it here is exactly how that arrived as a
    -- window that opened and stayed blank.
    --
    local req, who, cap, raw = sys.receive(ep, true, nil, frame.TAG)
    if not req then break end

    if raw then
      frame.request(req, who)
      goto next_request
    end

    local handler = handlers[req.type]
    local reply

    if not handler then
      reply = { ok = false, error = "no such operation: " .. tostring(req.type) }
    else
      local ok, result = pcall(handler, req, who, cap)
      reply = ok and result or { ok = false, error = tostring(result) }
    end

    -- A handler that took responsibility for its own answer, which `poll`
    -- does when there is nothing to report yet.
    --
    -- **And an answer that cannot go becomes one that can.** A reply too
    -- large for a message was dropped, and its caller - in `fs.send`, with
    -- nothing else that will ever wake it - waited for ever: the `windows`
    -- list did that to `tile` and the Deskbar on 28 September. So a failed
    -- reply is followed by a small one saying why, which reaches a caller
    -- that is still there and fails as harmlessly as the first for one that
    -- is not.
    if reply ~= DEFER then
      local sent, err = pcall(sys.reply, who, reply)

      if not replied(sent, err, req.type) then
        pcall(sys.reply, who, { ok = false,
                                error = "the answer to " .. tostring(req.type)
                                        .. " did not fit in a message" })
      end
    end

    ::next_request::
  end

  -- A window of each application being read off the disk (`launching`).
  step_launches()

  if P.measuring then t, heap = P.charge("messages", t, heap) end

  -- 2b. What was lent (`remote`, above): a viewer's screen, pointer and
  -- keys. Answered here, before the pointer's own pass, so a viewer's click
  -- lands in the same pass a mouse's would.
  while remote.ep do
    local req, who, cap = sys.receive(remote.ep, true)

    if not req then break end

    local handler = type(req) == "table" and remote.handlers[req.type]
    local reply

    if not handler then
      if cap and cap >= 0 then sys.release(cap) end

      reply = { ok = false, error = "not something lent: " .. tostring(type(req) == "table" and req.type) }
    else
      local ok, result = pcall(handler, req, who, cap)

      reply = ok and result or { ok = false, error = tostring(result) }
    end

    pcall(sys.reply, who, reply)
  end

  step("pointer")
  -- 3. The pointer, before the picture: a click can raise a window and a
  -- drag can move one, and both are damage that this pass should draw.
  --
  -- **The transitions first, each at the place it happened, and then where
  -- the pointer is now.**
  --
  -- `input.pointer` says what is held *now*, and that is the wrong question
  -- to ask about a click: a press and a release that both happened while
  -- this loop was busy leave it saying nothing happened at all. That is the
  -- bug Diego found on the ThinkPad - "the mouse buttons be unrespiosnive
  -- under certaun scenarios" - and it bites here, on the desktop, because
  -- this is where a pass can take a dozen messages.
  --
  -- `input.clicks` is every transition since the last pass, in order, from
  -- a queue the board now keeps (`hal/pointer_edges.c`). Replaying them
  -- through `pointer_pass` needs no new code in it: it already works out a
  -- press or a release by comparing against the last state it saw, so
  -- feeding it the states in the order they really happened produces
  -- exactly the presses and releases that really happened.
  --
  -- The final call then sees the current state, which the last edge has
  -- already brought it to, so it dispatches nothing and only settles the
  -- position.
  if input.pointer then
    for _, c in ipairs(input.clicks or {}) do
      -- One table, reused: a click is rare, but a table per click per pass
      -- on the frame path is the habit this loop cannot afford.
      pointer_log.replay.x = c.x
      pointer_log.replay.y = c.y
      pointer_log.replay.buttons = c.buttons
      pointer_log.replay.min_x = input.pointer.min_x
      pointer_log.replay.max_x = input.pointer.max_x
      pointer_log.replay.min_y = input.pointer.min_y
      pointer_log.replay.max_y = input.pointer.max_y
      pointer_pass(pointer_log.replay)
    end

    -- Said rather than counted silently: a transition that did not fit
    -- anywhere is a click nobody will ever see, and the whole point of this
    -- path is that such a thing should be impossible.
    if (input.pointer.clicks_lost or 0) > 0 and pointer_log.lost < 5 then
      pointer_log.lost = pointer_log.lost + 1
      print(("wm: %d button transition(s) lost")
            :format(input.pointer.clicks_lost))
    end
  end

  --
  -- **The mouse, unless a viewer has the pointer and the mouse is at rest.**
  -- `pointer_pass` goes by what it saw last, from wherever it came, so a
  -- mouse lying still would put the pointer back where the mouse is on the
  -- next pass after every move a viewer made. So after a viewer's move the
  -- mouse is passed only once it moves, presses or turns its wheel - and
  -- with no viewer, every pass, as always.
  --
  local mouse, seen = input.pointer, remote.physical

  if mouse then
    local still = mouse.x == seen.x and mouse.y == seen.y
                  and mouse.buttons == seen.buttons and (mouse.wheel or 0) == 0

    seen.x, seen.y, seen.buttons = mouse.x, mouse.y, mouse.buttons

    if not (remote.moved and still) then
      remote.moved = false
      pointer_pass(mouse)
    end
  end

  -- Focus following a pointer at rest (`wm/pointer.lua`).
  OUT.follow_tick()

  if P.measuring then t, heap = P.charge("pointer", t, heap) end

  -- 3b. Whoever asked to be told the list changed, before the answers go.
  tell_watchers()

  step("waiting")
  -- 4. Anybody who has been waiting long enough, or now has something.
  answer_waiting()

  if P.measuring then t, heap = P.charge("waiting", t, heap) end

  -- 5. Anything that was asked to close and did not, and the slots of
  -- anything that has already gone.
  collect_closing()

  --
  -- A process that has exited keeps its slot until somebody collects its
  -- exit code, which is what makes an exit code readable at all. Nobody was
  -- collecting these: closing two applications left two processes in the
  -- table for ever, and a desktop is exactly the thing that starts and ends
  -- programs all day.
  --
  -- Non-blocking, so a desktop with nothing to collect does not stop.
  --
  while true do
    local gone = sys.wait(true)

    if not gone then break end

    -- A program that ended before it opened a window is no longer starting.
    stop_starting(function(s) return s.pid == gone end)
  end

  if #starting > 0 then
    local now = sys.ticks()

    for i = #starting, 1, -1 do
      local s = starting[i]

      if now - s.since > (s.failed and FAILED_COUNTS or STARTING_COUNTS) then
        table.remove(starting, i)
      end
    end
  end

  if P.measuring then t, heap = P.charge("collect", t, heap) end

  step("compose")
  osd.tick()

  if watcher and sys.ticks() - watcher.asked > WATCH_LAPSE then
    unwatch("not asked for five seconds")
  end

  -- 6. The picture, cursor included.
  compose()

  if P.measuring then
    P.charge("compose", t, heap)

    local b = P.prof.busy
    b.total = b.total + P.pass_busy
    if P.pass_busy > b.max then b.max = P.pass_busy end
    P.bucket(b.hist, P.pass_busy)
    P.shown()

    --
    -- A pass the heap ended smaller than it started is a pass a collection
    -- finished in. That is not every collection - an incremental step that
    -- only marked will not show - but it is every one that freed anything,
    -- which is the kind that takes the time.
    --
    if collectgarbage("count") < heap_at_start then
      local gc = P.prof.gc
      gc.collections = gc.collections + 1
      if P.pass_busy > gc.worst then gc.worst = P.pass_busy end
      P.bucket(gc.hist, P.pass_busy)
    end

    P.pass_busy = 0
  end
end

-- Given back, which repaints: the console has no scrollback, so it starts
-- again from the top rather than restoring something nobody kept - and the
-- display's pointer with it, which the console has no use for.
if OUT.hw_cursor then gfx.cursor_hide() end

sys.screen_take(false)

back:free()

for _, win in ipairs(windows) do
  win.surface:free()
end

sys.destroy(ep)
