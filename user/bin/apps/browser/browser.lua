-- kosmos: application
-- kosmos: icon App_NetSurf
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- kosmos: opens html favorite
--
-- A web browser.
--
--   wm browser                       the page inside the image
--   wm browser:/Home/notes.html      a file on this machine
--   wm browser:asset:tutorial/cafesa3d/index.html
--                                    a page the image carries
--   wm browser:10.0.2.2:8000/        a server on the host running QEMU
--   wm browser:188.184.67.127/       somewhere on the internet, by number
--   wm browser:https://example.com/  over TLS
--
--   arrows            scroll a line
--   space / b         a screen down, a screen up
--   g / G             the top, the bottom
--   Control-L         type an address
--   r                 load it again
--
--   Super T, Super W      a new tab, and this one closed
--   Super Shift ] and [   the next tab, and the one before
--   Super 1 to 9          a tab by its place, 9 the last
--   Super L, Super R      type an address, load it again
--   Super [ and ]         back and forward
--
-- **A direct window** (`gfx.md` 19.4), and that is the whole difference
-- between this and what it replaced. The first version showed a page as a
-- list of wrapped lines in a widget, every line the same size in the same
-- face, because an application on the ordinary path sends *drawing
-- commands* and the compositor rasterises them - with the four faces the
-- desktop chose, which is right for a dialog and hopeless for a document
-- that wants a heading at 28 pixels and a paragraph at 16.
--
-- So this process owns its pixels. `web_paint.c` lays the document out
-- once and paints a band of it, three screens tall, around where it is
-- read; a frame is one blit of the view out of that band. Scrolling inside
-- the band re-runs nothing, and scrolling out of it paints the next band -
-- the runs already laid out, drawn again, with no layout and no line
-- breaking - which is how a page as long as the web's is drawn wherever it
-- is read (`roadmap.md` 6zz j).
--
-- What it costs is that there are no widgets. A direct window owns every
-- pixel, so a button would have nothing to draw into: the chrome here is
-- rectangles this file knows the position of, and a click is a comparison
-- against them. That is the trade the mode makes, and it is the reason the
-- chrome is deliberately small - back, forward, reload, home, an address
-- and a status line, which is NetSurf's own row and nothing more.
--
-- **It needs nothing running anywhere to be tried.** `wm browser` opens on
-- a page compiled into this file, parsed and painted by the same engine a
-- fetched one is - so a new build of Kosmos can be looked at without first
-- starting a server on the computer running QEMU, which is a thing an
-- operating system has no business asking for. Anything beginning with a
-- slash is read from the namespace instead of the network, for the same
-- reason.
--
-- **Names work**, through `/Network`'s resolver - a query, a reply, and the
-- compression pointers a real server answers with. **So does `https`**
-- (`roadmap.md` 6zz c), through the TLS Kit: the request is `http.lua`'s,
-- shared with `fetch`, and a certificate that does not check out is refused
-- with a page saying why and an **Open anyway** - for this window only,
-- remembered nowhere, and the page then says Not secure for as long as it
-- is open (`docs/browser.html`).
--
-- This comment said "no DNS, so a remote address is four numbers" for
-- months after the resolver was written, and so did the help page below and
-- `ping`. The resolver had one caller - the address bar four hundred lines
-- down - and no command a person could type, so nothing ever contradicted
-- the prose.

local ui    = use("/Kosmos/Libraries/ui.lua")
local http  = use("/Kosmos/Libraries/http.lua")
local httpcache = use("/Kosmos/Libraries/httpcache.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local favorites = use("/Kosmos/Libraries/favorites.lua")
local history = use("/Kosmos/Libraries/history.lua")
local prefs = use("/Kosmos/Libraries/browserprefs.lua")

--
-- **Its settings** (`roadmap.md` 6zz d5), from `/Home/Preferences/browser/
-- settings`: read as the window opens, and changed and written by the
-- Settings page, which is a tab like any other (`about:settings`).
--
local setting = prefs.read()
local theme = ui.theme

--
-- **What was fetched, kept** (`roadmap.md` 6zz k): pages and pictures, in
-- `/Home/Cache/Browser` - Diego's choice - and used again for as long as
-- their servers said they may be, or asked about with their validators.
-- One for the process, so a picture on every page of a site is fetched
-- once. Reload asks the server about everything it shows (`reloading`).
--
local CACHE = httpcache.open{ dir = "/Home/Cache/Browser" }
local reloading = false

--------------------------------------------------------------------------
-- The window, and what is where in it.
--------------------------------------------------------------------------

local W, H                           -- the window's size, which changes

local TABS = 40                      -- the tabs along the top: the title bar
local HEAD = ui.layout.head          -- the header: buttons, the address, its rule
local STAT = 26                      -- the status line along the bottom
local SBAR = 16                      -- the scrollbar down the right
local PAD  = 8                       -- white margin either side of the page

local FAVS = 30                      -- the favorites bar, under the header
local SIDE = 268                     -- the sidebar of favorites and history

local VIEW_X, VIEW_Y, VIEW_H, VIEW_W, PAGE_W

--
-- **The favorites** (`roadmap.md` 6zz d3), read from `/Home/Favorites`:
-- every one's address, for the star, and the folder's own entries, for the
-- bar - which is shown while there is anything to show on it. Whether the
-- sidebar is open is the window's, as the drawing has it.
--
local kept = {}
local bar_list = {}
local side_open = false

--
-- **The window's size, and what follows from it**: set as it opens, and
-- again whenever it is resized (`roadmap.md` 6zz e - Diego, 1 October: "make
-- sure our browser new design is resizable") - or the favorites bar comes
-- or goes, or the sidebar opens or closes, each of which moves the page.
--
local function geometry(w, h)
  W, H = w, h
  VIEW_X = side_open and SIDE or 0
  VIEW_Y = TABS + HEAD + ((#bar_list > 0 and setting.bar) and FAVS or 0)
  VIEW_H = H - VIEW_Y - STAT
  VIEW_W = W - VIEW_X - SBAR
  PAGE_W = VIEW_W - PAD * 2
end

geometry(900, 640)

--
-- How much of the page is painted at once: three screens, the one being
-- read and one either side of it.
--
-- This was the whole page, up to sixteen megabytes of it, which is about
-- seven screens at this width - and a document taller than that was cut off
-- and said to be. Wikipedia's Dam article is 52,803 pixels, thirty screens,
-- and a page of one to ten megabytes is an ordinary page. So the page is
-- laid out whole, which is boxes and runs rather than pixels, and painted a
-- band at a time: six megabytes, whatever the page's length.
--
local BAND_SCREENS = 3

local PAPER = 0xffffffff             -- what `web_paint.c` fills a page with

--
-- The kit is only in an image built with `WEB=1`, so its absence is an
-- ordinary state rather than a failure: the window opens and says which
-- build it is running on. An application that raised here would be a broken
-- entry in the Deskbar of every ordinary image.
--
local web

do
  local have, kit = pcall(use, "/Kosmos/Kits/web")

  if have and type(kit) == "table" then web = kit end
end

--
-- **NetSurf's layout** (`roadmap.md` 6zz j): the page laid out and drawn by
-- the engine NetSurf wrote for exactly these libraries, set up once with its
-- own default stylesheets. Without them - an image built before they were
-- carried - the pages are laid out by `web_paint.c`, as they were.
--
local NS

do
  local sheet = sys.asset("netsurf/default.css")

  NS = web ~= nil and web.setup ~= nil and sheet ~= nil
       and web.setup(sheet, sys.asset("netsurf/quirks.css")) == true

  -- The zoom Settings chose, for every page this window lays out.
  if NS and web.zoom then web.zoom(setting.zoom) end
end

--
-- **Its tabs are its title bar** (`roadmap.md` 6zz d2, `docs/browser.html`):
-- in a look with no title bars - Plex's - the window manager draws close,
-- minimise and zoom at the strip's right end and a press on its empty band
-- moves the window, as Groove's bar does; in a look that keeps them, the
-- window wears its tab above like anything else.
--
local win

do
  local made, why = ui.window{
    title = "Browser", w = W, h = H, x = 80, y = 60, direct = true,
    resizable = true, header = true,
  }

  if not made then
    print("browser: " .. tostring(why))
    return
  end

  win = made
end

if not win:surface() then
  print("browser: this window did not get a shared surface")
  return
end

--------------------------------------------------------------------------
-- Addresses.
--
-- Taken apart by `http.split`, which `fetch` uses too: a scheme or none -
-- `http://` typed or left off, `https://` - a host, a port and a path. It
-- used to be split here, and `http://` typed in the bar was looked up as a
-- machine called `http:` (Diego, 30 September: "cannot look up http:: 12").
--------------------------------------------------------------------------

--
-- A link's address, against the page it was found on.
--
-- Enough of RFC 3986 to follow a link and no more: a scheme this browser
-- does not speak is refused by name rather than attempted, an absolute path
-- keeps the host, and a relative one is taken from the directory the page
-- came from - each keeping the page's scheme, so a link on an `https` page
-- stays `https`. `..` is not collapsed, which a real resolver does and which
-- no page here has needed yet.
--
local function resolve(base, href)
  href = tostring(href or ""):gsub("^%s+", ""):gsub("%s+$", "")

  if href == "" then return nil, "that link has no address" end

  -- A fragment names a place in this page, and nothing here scrolls to an
  -- element yet.
  if href:sub(1, 1) == "#" then
    return nil, "that link points into this page, and there is no anchor yet"
  end

  local scheme = href:match("^(%a[%w+.%-]*):")

  if scheme then
    scheme = scheme:lower()

    if scheme == "asset" then return href end

    if scheme ~= "http" and scheme ~= "https" then
      return nil, ("this browser speaks http and https, not %s"):format(scheme)
    end

    local rest = href:gsub("^%a[%w+.%-]*:", ""):gsub("^//", "")

    return (scheme == "https" and "https://" or "") .. rest
  end

  -- The page's own scheme, kept for whatever it links to.
  local prefix, rest = base:match("^(%a[%w+.%-]*://)(.*)$")

  if not prefix then prefix, rest = "", base end

  if prefix:lower() == "http://" then prefix = "" end

  if href:sub(1, 2) == "//" then return prefix .. href:sub(3) end

  local host = rest:match("^([^/]+)") or rest

  if href:sub(1, 1) == "/" then return prefix .. host .. href end

  return prefix .. ((rest:match("^(.*/)") or (host .. "/")) .. href)
end

--------------------------------------------------------------------------
-- The page this browser starts on, which is inside it.
--
-- **A browser that can only show remote pages cannot be tried.** There was
-- nothing to look at without a server somewhere - so trying a new build of
-- Kosmos meant starting one on the host first, which is a thing an
-- operating system should never ask of the computer running it. Every
-- browser ever written ships a start page for this reason.
--
-- A Lua string rather than a file, because a released image has no disk
-- under it: `run-kosmos.sh` passes no drive, so `/Home` is an empty ramfs at
-- boot and a file put there would have to come from somewhere. This comes
-- from nowhere. It is parsed and laid out by exactly the same engine a
-- fetched page is, so it is also the fastest check that the renderer works
-- at all - `wm browser`, and there is either a page or there is not.
--------------------------------------------------------------------------

local START = [[
<!doctype html>
<html><head><meta charset="utf-8"><title>Kosmos</title></head><body>
<h1>Kosmos</h1>
<p>This page is inside the image. Nothing was fetched to show it and
nothing needs to be running anywhere - it is parsed by hubbub, walked
through libdom and painted by <code>web_paint.c</code>, which is the whole
engine, so if you can read this then the engine works.</p>

<h2>What it can do</h2>
<ul>
  <li>Headings that step down, with a rule under the big two.</li>
  <li><strong>Bold</strong>, <em>italic</em> and <code>monospace</code>
      inside a sentence, sharing one baseline.</li>
  <li>Lists with markers, quotations, and <code>pre</code> that keeps its
      spaces.</li>
  <li>Links you can click. Accented Latin: naive becomes na&iuml;ve,
      Angstrom becomes &Aring;ngstr&ouml;m.</li>
</ul>

<h2>Somewhere to go</h2>
<p>A name or an address, then a path. Type one in the bar above and press
Return. A file on this machine works too - anything beginning with a slash
is read from the namespace rather than the network:</p>
<pre>  example.com/              a name, looked up through /Network
  https://example.com/      the same, over TLS
  10.0.2.2:8000/            a server on the computer running QEMU
  188.184.67.127/           somewhere on the internet, by number
  /Home/notes.html          a file on this machine</pre>
<p><code>host example.com</code> at a prompt asks the same resolver on its
own, which is how to tell a name that will not resolve from a machine that
will not answer.</p>

<h2>What it cannot do</h2>
<ul>
  <li><strong>No HTTP/2, no cookies, no JavaScript.</strong> https works,
      TLS 1.2 through BearSSL, checked against Mozilla's roots.</li>
  <li><strong>Half a cascade.</strong> Colours, faces and sizes are the
      stylesheet's; margins, widths and floats are parsed and not used.</li>
  <li>Pictures, PNG and JPEG, each on a line of its own; no forms, no box
      model.</li>
</ul>

<blockquote>A word is where a font change, a link's hit rectangle and a
selection all attach. That is why layout keeps its runs.</blockquote>
</body></html>
]]

local HOME = "about:start"

--
-- **The page a new tab opens on** (`docs/browser.html`): the favorites, and
-- what was open lately - the newest pages of the history on the disk (d4),
-- each once.
--
local NEWTAB = "about:newtab"
local LATELY_SHOWN = 12

--
-- What a day of history is called where it is shown: Today, Yesterday, or
-- its date.
--
local function day_words(day)
  local now = clock.now()

  if not now then return day end
  if day == history.day_of(now) then return "Today" end
  if day == history.day_of(clock.at(now.epoch - 86400)) then return "Yesterday" end

  local _, m, d = day:match("^(%d+)%-(%d+)%-(%d+)$")

  return d and ("%d %s"):format(tonumber(d), clock.FULL_MONTHS[tonumber(m)] or m) or day
end

--------------------------------------------------------------------------
-- State.
--------------------------------------------------------------------------

local paper                  -- a band of the page, painted and scrolled by blit
local paper_h  = 0           -- how tall that surface is
local band_top = 0           -- the page's row at the band's first
local content_h = 0          -- how tall the document turned out to be
local top      = 0           -- the page's row at the top of the view

--
-- The document's pictures: where the layout put each box, where its bytes
-- come from, and `kept` - the picture decoded and scaled to its box, false
-- when it could not be had, nil until a band holding it is first painted.
--
-- Kept rather than drawn once and dropped, because a band is painted again
-- whenever the view comes back to it, and a JPEG decoded twice is the
-- slowest thing on the page. Scaled to the box, so what is kept is what is
-- shown: a photograph of 4000 by 3000 in a box of 220 by 165 keeps 145 KB
-- rather than 48 MB.
--
local pictures = {}
local kept_bytes = 0

-- Whether this document is NetSurf's to lay out - it is, unless NetSurf
-- could not - and which of its pictures have been asked for.
local ns_doc = false
local ns_tried = {}

-- Its SVGs, by the picture's number: the SVG as read, and the size it was
-- last drawn at - an SVG is drawn at its box's size rather than stretched
-- to it, so a box that changes size has it drawn again.
local ns_svgs = {}

-- And the rest of its pictures, by number: each as it was decoded, its own
-- size, and the size it was last scaled to. A picture shown at a size not
-- its own - Wikipedia's logo is 100 pixels drawn at 50 - was scaled again
-- every time its band was painted, which was most of a band's pictures'
-- cost (`roadmap.md` 6zz h); it is scaled once, to its box, and the band
-- draws it at its own size.
local ns_raster = {}

local paint_band             -- below, once the pictures can be fetched

--
-- The document, kept rather than closed the moment it is painted.
--
-- The boxes live on it - `link_at` asks the layout which word is under a
-- point - so closing it would leave a page you can read and cannot click.
-- One document at a time: the previous one is closed when the next arrives,
-- which is what bounds this rather than hoping nobody opens many pages.
--
local doc

local said = ""

--
-- Where the time went, along the bottom right.
--
-- Not decoration and not a debug switch left in by accident: this is the
-- only place in the system that can say whether a page is slow because of
-- the network, the parser, layout or the blitter, and those four want
-- entirely different work. `CLAUDE.md` is clear that a number from QEMU is
-- for detecting a regression rather than for claiming a speed - what it is
-- honestly good for is *attribution*, which is the question here.
--
local timing = ""

local HZ = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- Tenths of a millisecond, because a frame is single-digit milliseconds and
-- whole ones would round most of this to zero.
local function since(t0)
  return ((sys.ticks() - t0) * 10000) // HZ
end

local function tenths(n)
  return ("%d.%d"):format(n // 10, n % 10)
end

--
-- What the last repaint cost, split three ways, and the worst so far.
--
-- Split because the total answers nothing. A repaint is chrome this file
-- draws, one blit of the visible band out of the page, and a `commit` -
-- which is a *synchronous* message to the window manager, so its cost is an
-- IPC round trip plus however long that process took to get to it. Those
-- want completely different work, and which of them dominates is not
-- something to guess at.
--
-- It was worth asking. On this Mac's own cores a frame is 5.7 ms, of which
-- the blit is 2.8 and the commit is 2.8 - and the commit handler swaps an
-- index and records a rectangle. Half of every frame is spent *waiting*
-- rather than computing, which is not a thing any amount of faster drawing
-- would have found.
--
-- The worst case rather than the average, because responsiveness is a
-- promise about the worst case and always was.
--
-- In tenths of a millisecond: the frame, its page, its commit, the worst
-- frame so far - and `kb`, below, in one table, since a chunk of Lua holds
-- two hundred locals at most and this one is near it.
local frame_cost = { ms = 0, worst = 0, blit = 0, commit = 0, kb = 0 }

--
-- And what the key itself cost when it took the view out of the band: the
-- next band painted, and its pictures fetched if it is the first time. Not
-- part of a frame - it happens before one - and the only work a scroll
-- does that is more than a blit.
--
local band_ms

--
-- And what a repaint *allocates*, in hundredths of a kilobyte: `frame_cost.kb`.
--
-- `CLAUDE.md` records why this is measured next to the time rather than
-- instead of it: the desktop's frame profile found the language question
-- worth about a ninth of a pass and the allocation question worth four
-- times the worst-case collector pause. A process on a deadline is judged
-- by `gc_pause_max`, and the collector runs when it chooses.
--

local address = { text = HOME, caret = #HOME, from = 0 }
local here                                          -- what is on screen
local back, forward = {}, {}

--
-- How the page on screen came - `http.lua`'s table, or nil for one from
-- this machine - and the hosts this window was told to open anyway.
--
-- **Open anyway is this window's and nobody else's**, and it is forgotten
-- when the window closes: the drawing agreed "for this tab only ... and
-- nothing is remembered". A table in this process is exactly that.
--
local came
local anyway = {}

--
-- **A page on its way**: which host, how many bytes of how many, while
-- it arrives - nil otherwise. Diego, 30 September, watching Wikipedia's Dam
-- article sit on "waiting for en.wikipedia.org ..." for minutes: "the
-- browser needs a progress bar for loading pages", "somehwre in the status
-- panel" (`roadmap.md` 6zz i).
--
local loading

--
-- **What this browser says it is**, which is what a site decides what to
-- send on. Diego, 30 September: "to browse websites we need to send user
-- agent that aligns to what our browser is capable of" - so it was
-- NetSurf's, the engine's true name. And 1 October: "we have a very basic
-- browser so we need to announce that to the server", "send simple ones",
-- and to change it as the browser grows: so Settings chooses it, and the
-- default is a plain browser's, Lynx's, which measured is the one sites
-- send simpler pages - every choice still saying Kosmos
-- (`browserprefs.lua`). Changed in Settings, it is changed here.
--
local AGENT = prefs.agent(setting, (sys.build and sys.build() or {}).version)

local dragging               -- the scrollbar thumb, while it is held

-- Text a character shorter, never inside one; and text cut to a width with
-- an ellipsis - a tab's title, a favorite's name, the status line.
local function shorter(text)
  local n = #text

  while n > 0 and (text:byte(n) & 0xC0) == 0x80 do n = n - 1 end

  return text:sub(1, n - 1)
end

local function cut_to(text, room)
  if gfx.measure(text) <= room then return text end

  while text ~= "" and gfx.measure(text .. "...") > room do
    text = shorter(text)
  end

  return text .. "..."
end

--------------------------------------------------------------------------
-- **The header, in the kit's own widgets** (`roadmap.md` 6zz d1, as
-- `docs/browser.html` draws it).
--
-- Back, forward and reload as the kit's line-icon buttons; the address in
-- the kit's own field, drawn - when nobody is typing in it - as the drawing
-- has it: how the page came first, then the host dark and the rest dim;
-- and at the far end the sidebar, for favorites and history (d3, d4), and
-- the menu. Painted into this window's own pixels by `ui.paint_view`, as
-- Cafesa3D's panels are, and pressed and typed into through the kit's own
-- routing - so they behave as every other window's widgets do, and follow
-- the look. They replace a row of bevelled buttons and a well this file
-- drew itself, and the keys it took for the well by hand.
--
-- `theme` throughout: the palette is the desktop's, so a browser window is
-- not the one thing on screen that ignores the appearance setting. The
-- *page* is white whatever the desktop is, because that is what the
-- document asked for.
--------------------------------------------------------------------------

local go_back, go_forward, go_home, reload      -- filled in further down

local header = ui.view{ x = 0, y = TABS, w = W, h = HEAD }

function header:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, self.h - 1, self.w, 1, theme.line)
end

-- In one table, for the reason `frame_cost` gives.
local hb = {
  back    = ui.iconbutton{ icon = "back" },
  forward = ui.iconbutton{ icon = "forward" },
  reload  = ui.iconbutton{ icon = "reload" },
  side    = ui.iconbutton{ icon = "sidebar" },
  menu    = ui.iconbutton{ icon = "more" },
}
local field    = ui.field{ text = HOME, hint = "Search, or type an address" }

--
-- **The star**, at the field's right end (`docs/browser.html`): filled and
-- gold when the page is a favorite, a dim outline when it is not, and a
-- press makes it one or not (6zz d3). Not there while the field is typed
-- in, or on a page there is nothing to keep of.
--
local star_b = ui.iconbutton{ icon = "star" }

function star_b:draw(g)
  local on = here ~= nil and kept[here] ~= nil

  if self.pressed then g:fill_round(0, 0, self.w, self.h, theme.line_soft, 6) end

  if self.focused and self.keyed then
    g:frame_round(0, 0, self.w, self.h, theme.ring, 6)
  end

  g:line_icon((self.w - 15) // 2, (self.h - 15) // 2, on and "starred" or "star",
              on and 0xffd49b17 or theme.text_dim)        -- the drawing's gold
end

for _, v in ipairs({ hb.back, hb.forward, hb.reload, field, star_b, hb.side, hb.menu }) do
  header:add(v)
end

--
-- How the page on screen came, in the field's words and with its icon:
-- Secure for a certificate that checked out, Not encrypted for plain HTTP,
-- Refused, Not secure for one opened anyway, This machine for a page no
-- network had a part in - and nothing for the page inside the image.
--
local function how_came()
  if here == nil or here == HOME or here == NEWTAB then return nil end
  if here == prefs.PAGE then return "settings", "Settings" end
  if came == nil then return "plain", "This machine" end
  if came.refused then return "refused", "Refused" end
  if came.scheme ~= "https" then return "plain", "Not encrypted" end
  if came.secure then return "secure", "Secure" end

  return "refused", "Not secure"
end

-- The address in two: the host, which is what matters and is drawn dark,
-- and the rest, drawn dim. The scheme goes, since the field has just said
-- how the page came.
local function address_parts(text)
  local bare = tostring(text or ""):gsub("^%a[%w+.-]*://", "")
  local host = bare:match("^[^/]*") or ""

  return host, bare:sub(#host + 1)
end

-- **Taken to be typed in**: the page's address, all of it chosen, so what
-- is typed replaces it.
function field:take_address()
  self.text = address.text
  self.caret = #self.text + 1
  self.all = true
end

--
-- **And given the keys**, by Control-L, Super L or a new tab: it takes the
-- address the moment it is given them. It took it when a frame next found
-- it newly focused, and a frame runs after a batch of keys - while a page
-- arrived, the keys typed after Control-L came as one batch, and either
-- went into the old text and were written over or, the field's own flag
-- not settled yet, were put on the end of it (6zz d6's checks found both).
-- A press on the field is still the frame's to notice, as it always was.
--
function field:take_keys()
  win:focus_on(self)
  self:take_address()
  self.had = true
end

do
  local field_draw = field.draw

  function field:draw(g)
    if self.focused then return field_draw(self, g) end

    local h = self.h

    g:fill_round(0, 0, self.w, h, theme.sunken, 7)
    g:frame_round(0, 0, self.w, h, theme.line_soft, 7)

    local x = 9
    local kind, words = how_came()

    if kind then
      local ink = kind == "secure" and theme.good
                  or kind == "refused" and theme.bad or theme.text_dim
      local bw = 4 + 15 + 5 + gfx.measure(words, "label") + 7

      g:fill_round(4, 3, bw, h - 6, theme.mix(theme.sunken, ink, 140), 6)
      g:line_icon(8, (h - 15) // 2, kind, ink)
      g:text(28, (h - gfx.height("label")) // 2, words, ink, nil, "label")
      x = 4 + bw + 8
    end

    -- Cut to end before the star, which sits inside the field's right end.
    local host, rest = address_parts(address.text)
    local room = self.w - x - (star_b.hidden and 8 or (star_b.w + 8))
    local hw = gfx.measure(host, "label")

    if hw > room then
      host = cut_to(host, room)
      hw, rest = gfx.measure(host, "label"), ""
    end

    g:text(x, (h - gfx.height("label")) // 2, host, theme.text, nil, "label")
    g:text(x + hw, (h - gfx.height()) // 2, cut_to(rest, room - hw), theme.text_dim)
  end
end

--
-- Placed along the window's width, and again whenever it changes: three at
-- the start, two at the end, the field between them taking what is left.
-- Said, for whoever drives the window from outside (`tools/run_browser.py`),
-- as centres in the window.
--
local function lay_out_header()
  local gap, edge = ui.layout.head_gap, ui.layout.head_edge
  local x = edge

  header.w = W

  for _, b in ipairs({ hb.back, hb.forward, hb.reload }) do
    b.x, b.y = x, (HEAD - 1 - b.h) // 2
    x = x + b.w + gap
  end

  local r = W - edge

  for _, b in ipairs({ hb.menu, hb.side }) do
    b.x, b.y = r - b.w, (HEAD - 1 - b.h) // 2
    r = b.x - gap
  end

  field.x = x + 4
  field.w = math.max(40, r - 4 - field.x)
  field.y = (HEAD - 1 - field.h) // 2

  star_b.x = field.x + field.w - star_b.w - 3
  star_b.y = field.y + (field.h - star_b.h) // 2

  local function at(v)
    return ("%d,%d"):format(v.x + v.w // 2, header.y + v.y + v.h // 2)
  end

  print(("browser: header back %s forward %s reload %s field %s menu %s star %s "
         .. "side %s, %d tall, below tabs %d tall")
        :format(at(hb.back), at(hb.forward), at(hb.reload), at(field), at(hb.menu),
                at(star_b), at(hb.side), HEAD, TABS))
end

--
-- What the status line says on its left - where the link under the
-- pointer goes, or what the browser last said - cut to end before the
-- page's costs begin on its right.
--
-- It ran underneath them once a page's line began with how it came -
-- "Refused: the certificate is signed by nobody this machine trusts" and
-- then the counts. Cut a character at a time, never inside one, and worked
-- out again only when the words or the costs change rather than every
-- frame of a scroll.
--
local pointing               -- the link under the pointer, while it is
local status_for, status_cut = nil, ""


local function status_text()
  local left = pointing or said
  local key = left .. "\0" .. timing .. "\0" .. W .. tostring(setting.costs)

  if key == status_for then return status_cut end

  local right = (timing ~= "" and setting.costs) and gfx.measure(timing) + 20 or 0
  local room = W - 20 - right
  local text = left

  if gfx.measure(text) > room then
    while text ~= "" and gfx.measure(text .. "...") > room do
      text = shorter(text)
    end

    text = text .. "..."
  end

  status_for, status_cut = key, text
  return text
end

--------------------------------------------------------------------------
-- **Tabs, and they are the title bar** (`roadmap.md` 6zz d2, as
-- `docs/browser.html` draws them).
--
-- Each tab is a page with its own back and forward: everything above that
-- says which page is shown - the document, its band, where it is read, its
-- pictures, how it came, what was opened anyway - is the shown tab's, and a
-- tab not shown keeps its own in its table (`stow`, `unstow`). What it does
-- not keep is its band, which is three screens of pixels: that is painted
-- again when the tab is shown, from the layout and the pictures it kept,
-- and laid out again only if the window changed size meanwhile.
--
-- The strip is the kit's views, painted into this window's pixels as the
-- header is; a press on a tab shows it, on its cross closes it, and on the
-- strip's empty band moves the window, where the look has no title bars
-- (`moves_window`). The window manager draws the three at its right end,
-- told where by `place_lights`.
--------------------------------------------------------------------------

local show_tab, close_tab, new_tab           -- filled in further down

local tabs, current = {}, nil

local function index_of(t)
  for i, u in ipairs(tabs) do
    if u == t then return i end
  end
end

-- A tab's page: the shown one's is in the locals above, the others' in
-- their tables.
local function tab_at(t)
  if t == current then return here end
  return t.here or t.pending
end

--
-- What a tab is called: the host it is on its way to while a page arrives,
-- then the page's title, and the address when the page has none.
--
local function tab_name(t)
  local at = t.going or tab_at(t)

  if not t.going and t.title and t.title ~= "" then return t.title end
  if at == nil or at == NEWTAB then return "New tab" end

  local host = address_parts(at)

  return host ~= "" and host or at
end

--
-- **A favicon until there are favicons**: the host's first letter on a
-- colour the host chooses, the same one every time - which is all a
-- favicon is for, telling tabs apart at a glance. The page inside the image
-- and a new tab are Kosmos's.
--
local favicon_of

do
  local FAVICON = { 0xff8b1e1e, 0xff1a1a1a, 0xff2f8a55, 0xffc0392b,
                    0xff555555, 0xff2a55c9, 0xff7a4fb5, 0xffb7791f }

  function favicon_of(at)
    at = at or NEWTAB

    -- The browser's own pages - the one inside the image, a new tab,
    -- Settings - are Kosmos's.
    if at == HOME or at == NEWTAB or at:match("^about:") then return "K", FAVICON[1] end

    local host = address_parts(at):gsub("^www%.", "")

    if host == "" then host = at:match("([^/]+)/*$") or at end

    local sum = 0

    for i = 1, #host do sum = (sum * 31 + host:byte(i)) % 65521 end

    return (host:match("%w") or "?"):upper(), FAVICON[sum % #FAVICON + 1]
  end
end

local function favicon(t)
  return favicon_of(t.going or tab_at(t))
end

-- A favicon drawn: its square and its letter, `size` pixels.
local function draw_favicon(g, x, y, size, at)
  local letter, ground = favicon_of(at)
  local face = ui.sized("label", size <= 13 and 9 or 10)

  g:fill_round(x, y, size, size, ground, 3)
  g:text(x + (size - gfx.measure(letter, face)) // 2,
         y + (size - gfx.height(face)) // 2, letter, 0xffffffff, nil, face)
end

local strip = ui.view{ x = 0, y = 0, w = W, h = TABS }
strip.moves_window = true

--
-- The strip a shade darker than the window, so the shown tab stands out of
-- it in the header's own colour and runs into the header without a line -
-- darker by more in a dark look, whose window is nearly black already. Not
-- `sunken`, which is white in the light looks: the strip came out lighter
-- than the header, the drawing upside down, and the suite took it for the
-- page's paper.
--
local function strip_ground()
  local w = theme.window
  local lum = (((w >> 16) & 0xff) * 299 + ((w >> 8) & 0xff) * 587
               + (w & 0xff) * 114) // 1000

  return theme.mix(w, 0xff000000, lum > 128 and 65 or 250)
end

function strip:draw(g)
  g:fill(0, 0, self.w, self.h, strip_ground())
  g:fill(0, self.h - 1, self.w, 1, theme.line)
end

local new_b = ui.iconbutton{ icon = "plus" }

strip:add(new_b)


local tab_view, lay_out_tabs

do
  -- As wide as the drawing's and no wider, and narrower as more are open:
  -- at the least, a tab is its favicon.
  local TAB_MOST, TAB_LEAST, TAB_GAP = 206, 28, 2

  function tab_view(t)
    local v = ui.view{ x = 0, y = 0, w = TAB_MOST, h = TABS }

    v.tab = t

    -- Its cross: on the shown tab always, and on the others while they are
    -- wide enough to say what they are as well.
    local function crossed(self)
      return self.w >= 60 and (t == current or self.w >= 90)
    end

    function v:draw(g)
      local on = t == current
      local top = 6
      local h = TABS - top

      if on then
        -- Rounded above, square where it meets the header, which paints over
        -- what reaches below the strip.
        g:fill_round(0, top, self.w, h + 8, theme.window, 8)
      else
        g:fill(self.w - 1, top + 9, 1, h - 18, theme.line)
      end

      local fx = (self.w < 60) and (self.w - 14) // 2 or 10

      draw_favicon(g, fx, top + (h - 14) // 2, 14, t.going or tab_at(t))

      if self.w < 60 then return end

      local cross = crossed(self)
      local room = self.w - 32 - (cross and 26 or 10)

      g:text(32, top + (h - gfx.height()) // 2, cut_to(tab_name(t), room),
             on and theme.text or theme.text_dim)

      if cross then
        g:line_icon(self.w - 24, top + (h - 15) // 2, "close", theme.text_dim)
      end
    end

    -- A press shows it; a click on its cross closes it, and only if the
    -- pointer is still on the cross when it lets go.
    function v:mouse(action, x, y)
      local on_cross = crossed(self) and x >= self.w - 28 and x < self.w
                       and y >= 0 and y < self.h

      if action == "press" then
        self.crossing = on_cross

        if not on_cross then show_tab(t) end

        return true
      elseif action == "release" then
        if self.crossing and on_cross then close_tab(t) end

        self.crossing = nil
        return true
      end

      return false
    end

    return v
  end

  --
  -- Placed along the width the window has, and again only when that, the
  -- number of tabs or the title bar changes - every frame asks, so a look
  -- that takes the title bars off moves the tabs out of the three's way.
  -- Said, for whoever drives the window from outside, as places in it: the
  -- first tab's middle, the new tab button's and the empty band's.
  --
  local tabs_laid = {}

  function lay_out_tabs()
    local lights = win.headed and win.lights or nil

    if tabs_laid.w == W and tabs_laid.n == #tabs and tabs_laid.lights == lights then
      return
    end

    tabs_laid.w, tabs_laid.n, tabs_laid.lights = W, #tabs, lights
    strip.w = W

    local right = lights and (W - ui.layout.lights_in - lights.w - 12) or (W - 8)
    local room = right - 8 - (new_b.w + 8)
    local n = math.max(1, #tabs)
    local each = math.max(TAB_LEAST, math.min(TAB_MOST,
                                              (room - TAB_GAP * (n - 1)) // n))
    local x = 8

    for _, t in ipairs(tabs) do
      t.view.x, t.view.y, t.view.w, t.view.h = x, 0, each, TABS
      x = x + each + TAB_GAP
    end

    new_b.x = math.min(x + 2, right - new_b.w)
    new_b.y = (TABS - new_b.h) // 2

    if lights then
      win:place_lights(strip, W - ui.layout.lights_in - lights.w,
                       (TABS - lights.h) // 2)
    end

    print(("browser: tabs %d, each %d wide, the first at %d,%d, new %d,%d, "
           .. "band %d,%d")
          :format(#tabs, each, 8 + each // 2, TABS // 2,
                  new_b.x + new_b.w // 2, new_b.y + new_b.h // 2,
                  (new_b.x + new_b.w + right) // 2, TABS // 2))
  end
end

--------------------------------------------------------------------------
-- **The favorites bar and the sidebar** (`roadmap.md` 6zz d3, as
-- `docs/browser.html` draws them).
--
-- The bar is under the header while `/Home/Favorites` has anything in it:
-- each favorite its favicon and its name, a folder its folder and its name -
-- pressed, a favorite is shown and a folder opens as a menu of what it
-- holds - and what does not fit behind the dots at its end. The sidebar is
-- down the left, opened by its button or Super Y: the favorites as a tree,
-- folders and all, and what was open lately, a press on either showing it.
-- Both are the kit's views, painted into this window's pixels as the header
-- is; what a press on them does is further down, where `visit` is.
--------------------------------------------------------------------------

local open_entry, open_address           -- filled in further down
local toggle_favorite, toggle_side, open_settings, set_zoom

local favbar = ui.view{ x = 0, y = TABS + HEAD, w = W, h = FAVS }
favbar.hidden = true

function favbar:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, self.h - 1, self.w, 1, theme.line)
end

local bar_more = ui.iconbutton{ icon = "more" }
local bar_rest = {}                      -- what the bar had no room for
local lay_out_bar

--
-- `do`, here and below, so that what only one function uses ends with it:
-- Lua holds a chunk to two hundred locals at once, and this one is near it.
--
do
  local BAR_NAME = 150                     -- the widest a name on the bar is drawn

  local function bar_item(e)
    local name = cut_to(e.name, BAR_NAME)
    local v = ui.view{ x = 0, y = 3, w = 8 + 14 + 6 + gfx.measure(name) + 8,
                       h = FAVS - 7 }

    v.entry = e

    function v:draw(g)
      if self.pressed then g:fill_round(0, 0, self.w, self.h, theme.line_soft, 6) end

      if e.folder then
        g:line_icon(7, (self.h - 15) // 2, "folder", theme.accent)
      else
        draw_favicon(g, 8, (self.h - 13) // 2, 13, e.address)
      end

      g:text(8 + 14 + 6, (self.h - gfx.height()) // 2, name, theme.text)
    end

    -- Shown on letting go over it, as a button is.
    function v:mouse(action, x, y)
      if action == "press" then
        self.pressed = true
        return true
      elseif action == "release" then
        self.pressed = false

        if x >= 0 and x < self.w and y >= 0 and y < self.h then open_entry(e, self) end

        return true
      end

      return false
    end

    return v
  end

  --
  -- Placed along the width there is - again when that or the favorites
  -- change - and said, for whoever drives the window from outside.
  --
  function lay_out_bar()
    favbar.hidden = #bar_list == 0 or not setting.bar
    favbar.y, favbar.w = TABS + HEAD, W
    favbar.children = {}
    bar_rest = {}

    if favbar.hidden then return end

    local items = {}

    for _, e in ipairs(bar_list) do items[#items + 1] = bar_item(e) end

    local all = 8

    for _, v in ipairs(items) do all = all + v.w + 2 end

    local room = (all <= W - 8) and (W - 8) or (W - 8 - bar_more.w - 6)
    local x = 8

    for i, v in ipairs(items) do
      if x + v.w > room then
        for j = i, #items do bar_rest[#bar_rest + 1] = items[j].entry end
        break
      end

      v.x = x
      favbar:add(v)
      x = x + v.w + 2
    end

    if #bar_rest > 0 then
      bar_more.x, bar_more.y = W - 8 - bar_more.w, (FAVS - bar_more.h) // 2
      favbar:add(bar_more)
    end

    local first = favbar.children[1]

    print(("browser: favorites bar %d of %d, the first at %d,%d")
          :format(#bar_list - #bar_rest, #bar_list,
                  first and first.x + first.w // 2 or 0, favbar.y + FAVS // 2))
  end
end

local side = ui.view{ x = 0, y = 0, w = SIDE, h = 100 }
side.hidden = true

function side:draw(g)
  g:fill(0, 0, self.w, self.h, strip_ground())
  g:fill(self.w - 1, 0, 1, self.h, theme.line)
end

local side_seg = ui.segments{ items = { "Favorites", "History" } }
local side_find = ui.field{ hint = "Search history" }
local side_tree = ui.tree{}

side_seg:fit()
side:add(side_seg)
side:add(side_find)
side:add(side_tree)

-- History's half has its search field above the tree; Favorites' has none.
local function lay_out_side()
  side.hidden = not side_open
  side.y, side.h = VIEW_Y, VIEW_H
  side_seg.x, side_seg.y = (SIDE - side_seg.w) // 2, 10
  side_find.hidden = side_seg.on ~= 2
  side_find.x, side_find.y, side_find.w = 8, side_seg.y + side_seg.h + 8, SIDE - 16

  local below = side_find.hidden and side_seg or side_find

  side_tree.x, side_tree.y = 8, below.y + below.h + 8
  side_tree.w, side_tree.h = SIDE - 16, math.max(40, side.h - side_tree.y - 8)
end

local fill_side

do
  -- The favorites as the tree's rows, a folder's asked for when it opens.
  local function fav_nodes(dir)
    local out = {}

    for _, e in ipairs(favorites.read(dir)) do
      if e.folder then
        out[#out + 1] = { text = e.name, children = function() return fav_nodes(e.path) end }
      else
        local host = address_parts(e.address)

        out[#out + 1] = { text = e.name, note = host ~= "" and host or nil,
                          address = e.address }
      end
    end

    if #out == 0 and not dir then
      out[1] = { text = "None yet: the star keeps one", quiet = true }
    end

    return out
  end

  --
  -- **The history, by day, searched as typed** (d4): each day a heading -
  -- Today, Yesterday, its date - and under it its pages, newest first, with
  -- the time each was last shown.
  --
  local function history_nodes(text)
    local out = {}

    for _, d in ipairs(history.search(text)) do
      out[#out + 1] = { text = day_words(d.day), heading = true }

      for _, e in ipairs(d.pages) do
        out[#out + 1] = { text = e.title ~= "" and e.title or e.address,
                          note = e.time, address = e.address }
      end
    end

    if #out == 0 then
      out[1] = { text = text ~= "" and "Nothing like it" or "Nothing opened yet",
                 quiet = true }
    end

    return out
  end

  function fill_side()
    side_tree.top = 1

    if side_seg.on == 1 then
      side_tree.roots = fav_nodes()
      return
    end

    side_tree.roots = history_nodes(side_find.text)

    -- Said, with where its first page is - under the first day's heading.
    local pages = 0

    for _, n in ipairs(side_tree.roots) do
      if n.address then pages = pages + 1 end
    end

    local row = ui.theme.metrics.row

    print(("browser: history, %d pages, searched for \"%s\", the first at %d,%d, "
           .. "the field at %d,%d")
          :format(pages, side_find.text, side_tree.x + 40,
                  VIEW_Y + side_tree.y + 2 + row + row // 2,
                  side_find.x + side_find.w // 2, VIEW_Y + side_find.y + side_find.h // 2))
  end
end

side_seg.on_change = function()
  lay_out_side()
  fill_side()
end

side_find.on_change = function() fill_side() end
side_tree.on_select = function(_, node)
  if node.address then open_address(node.address) end
end

--------------------------------------------------------------------------
-- **Settings** (`roadmap.md` 6zz d5, `docs/browser.html`): a page of the
-- browser's own, in a tab like any other - `about:settings` - in
-- Preferences' cards (`ui.cards`), two columns as the drawing has them, one
-- in a narrow window, and scrolled together by the wheel when they are
-- taller than the page. What its rows do is further down
-- (`fill_settings`), where everything they change is.
--------------------------------------------------------------------------

local settings_page = ui.view{ x = 0, y = 0, w = 100, h = 100 }
settings_page.hidden = true
settings_page.scroll, settings_page.tall = 0, 0

local fill_settings                      -- below

function settings_page:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
end

function settings_page:wheel(n)
  local most = math.max(0, self.tall - self.h)

  self.scroll = math.max(0, math.min(most, self.scroll - n * 48))

  for _, c in ipairs(self.children) do c.y = c.top - self.scroll end

  return true
end

--------------------------------------------------------------------------
-- The scrollbar, which is also where scrolling is bounded.
--------------------------------------------------------------------------

local function reach()
  return math.max(0, content_h - VIEW_H)
end

-- The track the kit's pill runs in (`ui.lua`, `thumb_of`): two pixels in
-- at each end, and a thumb never shorter than sixteen.
local function thumb()
  local track = VIEW_H - 4
  local shown = content_h
  local last  = reach()

  if track < 8 or shown <= VIEW_H then
    return nil, track                     -- nothing to scroll: no pill
  end

  local h = math.max(16, (track * VIEW_H) // shown)
  local y = VIEW_Y + 2 + ((track - h) * top) // last

  return y, h
end

--
-- **The kit's pill**, in the kit's colour, over the list's own ground - one
-- scrollbar in every application (`roadmap.md` 6u: "Let's just have 1
-- scrollbars style go all the os", "pill"). This page is drawn into a
-- surface of the browser's own, so it cannot call the kit's; it draws the
-- same thing, and the arrow buttons it had went with every other one.
--
local function draw_scrollbar(s)
  local ty, th = thumb()

  s:fill(W - SBAR, VIEW_Y, SBAR, VIEW_H, theme.sunken)

  if ty then
    s:fill_round(W - 5 - 6, ty, 6, th, theme.mix(theme.sunken, theme.text_dim, 450), 3)
  end
end

--------------------------------------------------------------------------
-- One frame.
--------------------------------------------------------------------------

local function frame()
  local s = win:surface()

  if not s then return end

  local began = sys.ticks()
  local held = collectgarbage("count")

  --
  -- The header: the kit's widgets, painted into these pixels - back and
  -- forward dim when there is nowhere to go, and the field told the page's
  -- address whenever it is taken to be typed in, all of it chosen.
  --
  hb.back.disabled = #back == 0
  hb.forward.disabled = #forward == 0

  if field.focused and not field.had then field:take_address() end

  field.had = field.focused

  -- The star, on a page there is something to keep of and while nobody is
  -- typing an address over it.
  star_b.hidden = field.focused or here == nil or here == NEWTAB

  -- The tabs, then the header - which paints over the foot of the shown
  -- tab, so the two read as one.
  lay_out_tabs()
  ui.paint_view(strip, s, 0, 0)
  ui.paint_view(header, s, 0, TABS)

  if not favbar.hidden then ui.paint_view(favbar, s, 0, TABS + HEAD) end

  -- A page on its way, along the header's rule: filled against the length
  -- the server gave; with none, the status line's words say how much.
  if loading and loading.total and loading.total > 0 then
    s:fill(0, TABS + HEAD - 2, math.min(W, W * loading.got // loading.total), 2,
           theme.accent)
  end

  --
  -- The page.
  --
  -- The chrome is measured out of the split rather than into it: it came
  -- back at 0.1 ms against a 2.8 ms blit, which is a number that answers
  -- nothing and takes room on the line from the two that do.
  local drew_chrome = sys.ticks()

  -- Settings in place of a page, its own widgets over the whole of it.
  settings_page.hidden = here ~= prefs.PAGE

  if not settings_page.hidden then
    ui.paint_view(settings_page, s, VIEW_X, VIEW_Y)
  else
    s:fill(VIEW_X, VIEW_Y, VIEW_W, VIEW_H, paper and PAPER or theme.window)

    if paper then
      local rows = math.min(VIEW_H, band_top + paper_h - top)

      if rows > 0 then
        s:blit(paper, 0, top - band_top, PAGE_W, rows, VIEW_X + PAD, VIEW_Y)
      end
    else
      local lines = web
                    and { "Nothing loaded." }
                    or  { "This image was built without the web libraries.",
                          "",
                          "    make WEB=1 qemu",
                          "",
                          "builds one that has them." }

      for i, line in ipairs(lines) do
        s:text(VIEW_X + PAD + 4, VIEW_Y + 16 + (i - 1) * (gfx.height() + 4), line,
               theme.text_dim)
      end
    end
  end

  if side_open then ui.paint_view(side, s, 0, VIEW_Y) end

  if settings_page.hidden then draw_scrollbar(s) end

  --
  -- The status line, the page's benchmark (`docs/browser.html`): where the
  -- link under the pointer goes, or what the browser last said, on its
  -- left; what the page cost on its right - a click on it opens the
  -- breakdown.
  --
  s:fill(0, H - STAT, W, STAT, theme.window)
  s:fill(0, H - STAT, W, 1, theme.line)

  local sy = H - STAT + (STAT - gfx.height()) // 2

  s:text(10, sy, status_text(), theme.text_dim)

  if timing ~= "" and setting.costs then
    s:text(W - 10 - gfx.measure(timing), sy, timing, theme.text_dim)
  end

  local drew_all = sys.ticks()

  frame_cost.blit = ((drew_all - drew_chrome) * 10000) // HZ

  win:commit()

  frame_cost.commit = since(drew_all)
  frame_cost.ms = since(began)

  -- A negative delta means the collector ran inside this frame, which says
  -- nothing about what the frame allocated. Kept rather than clamped,
  -- because seeing one is itself the answer to a question.
  frame_cost.kb = math.floor((collectgarbage("count") - held) * 100)

  if frame_cost.ms > frame_cost.worst then frame_cost.worst = frame_cost.ms end
end

local function say(text)
  said = text
  frame()
end

--------------------------------------------------------------------------
-- A document, laid out.
--
-- Laid out whole, into nothing, and then painted a band at a time into a
-- surface three screens tall. Layout is where the lines are broken and the
-- boxes placed, and it happens once a document; painting is the glyphs,
-- and happens for the band being read (`paint_band`, below).
--------------------------------------------------------------------------

local laid_ms, painted_ms = 0, 0

-- The size the document was laid out at, and its zoom, so a tab shown
-- again after the window changed size or the zoom changed knows to lay it
-- out again.
local laid_for

-- Of the paint, what went on the band's pictures: fetched, decoded and the
-- page laid out again around them. It was counted as painting, and on
-- Wikipedia it was nearly all of it (`roadmap.md` 6zz h).
local pictures_ms = 0

--
-- An address as NetSurf takes one, and one it gives back as this browser
-- writes it. NetSurf joins links by the rules for URLs, so a file of this
-- machine's goes to it as a `file:` URL; what comes back loses `http://`,
-- which the address bar has never shown, and `file://` becomes the path.
--
local ns_address

do
  local KNOWN = { http = true, https = true, asset = true, about = true,
                  file = true, kosmos = true }

  function ns_address(text)
    if text == nil then return nil end
    if text:sub(1, 1) == "/" then return "file://" .. text end

    local scheme = text:match("^(%a[%w+.%-]*):")

    -- An address the bar keeps without its scheme is http's - and a host
    -- with a port looks like a scheme to the pattern, so only the ones this
    -- browser speaks count as one.
    if scheme and KNOWN[scheme:lower()] then return text end

    return "http://" .. text
  end
end

local function from_ns(url)
  if url == nil then return nil end
  if url:match("^file:///") then return url:sub(8) end
  if url:lower():match("^http://") then return url:sub(8) end

  return url
end

local function forget_pictures()
  for _, p in ipairs(pictures) do
    if p.kept then p.kept:free() end
  end

  pictures, kept_bytes = {}, 0
end

local function lay_out(doc)
  if paper then
    paper:free()
    paper = nil
  end

  forget_pictures()
  paper_h, band_top, content_h, top = 0, 0, 0, 0
  ns_doc, ns_tried, ns_svgs, ns_raster = false, {}, {}, {}

  --
  -- Timed apart from the painting, and the split is the measurement:
  -- timing them together would answer "the page took 90 ms" and leave the
  -- only useful question - *which half* - unanswered.
  --
  -- NetSurf's, unless it could not: then `web_paint.c`'s, and the status
  -- line says why.
  --
  local t0 = sys.ticks()

  if NS then
    local tall, why = doc:ns_layout(PAGE_W, VIEW_H, ns_address(here))

    if tall then
      content_h, ns_doc = tall, true
    else
      say("NetSurf could not lay this page out: " .. tostring(why))
    end
  end

  if not ns_doc then
    content_h = doc:render(nil, PAGE_W)
  end

  laid_ms = since(t0)
  laid_for = PAGE_W * 65536 + VIEW_H + setting.zoom * 4294967296

  -- The whole page with a margin under it when that is less than a band,
  -- so a short page is painted once and never again.
  local tall = math.max(VIEW_H, math.min(content_h + 16, VIEW_H * BAND_SCREENS))

  local made = pcall(function()
    paper = gfx.surface{ w = PAGE_W, h = tall }
  end)

  if not made or not paper then
    paper = nil
    return nil, ("no memory for a %dx%d band of the page"):format(PAGE_W, tall)
  end

  paper_h = tall

  if not ns_doc then
    for _, im in ipairs(doc:images()) do
      pictures[#pictures + 1] = { im = im, where = resolve(here or "", im.src) }
    end
  end

  return content_h
end

--------------------------------------------------------------------------
-- One load: connect, ask, read until the far end hangs up, parse, lay out.
--
-- The read loop is `fetch`'s, including the read *after* the loop: a close
-- and the last bytes can arrive in the same segment, and stopping at
-- `closed` would lose them.
--------------------------------------------------------------------------

--
-- One document off the network, following redirects.
--
-- Returns the body, or nil having said why. Redirects are followed because
-- without them the internet is unreachable in practice rather than in
-- theory: `188.184.67.127/` is a 301 to a path, and a browser that stops
-- there fetches twenty-one bytes and paints an empty page, which is what
-- this did. Bounded, because a pair of pages can point at each other.
--
--
-- A page refused for its certificate, as a page of its own.
--
-- Laid out by the same engine as any other, so it needs nothing the
-- browser does not already have; its two links are the browser's own
-- (`kosmos:back`, `kosmos:anyway`), which a click answers rather than
-- fetches. The words are `docs/browser.html`'s.
--
local function escaped(text)
  return (tostring(text):gsub("[&<>\"]", { ["&"] = "&amp;", ["<"] = "&lt;",
                                           [">"] = "&gt;", ['"'] = "&quot;" }))
end

local function refused_page(text, reason)
  local why = tostring(reason):gsub("^the certificate", "its certificate")

  return ([[<!doctype html>
<html><head><meta charset="utf-8"><title>Not opened</title></head><body>
<h1>This page was not opened: %s</h1>
<p>You asked for <b>%s</b>, and the certificate the server answered with
did not check out. Anybody between here and there could be answering
instead, so nothing was sent to it.</p>
<p><a href="kosmos:back">Go back</a></p>
<p><a href="kosmos:anyway">Open anyway</a></p>
<p>Open anyway loads it for this tab only; it says Not secure for as long
as it is open, and nothing is remembered.</p>
</body></html>]]):format(escaped(why), escaped(text))
end

--
-- **A new tab's page**: the favorites as tiles, the folder's own as the bar
-- shows them, and what was open lately, newest first - each a link, laid
-- out by the same engine as any other page and followed the same way. An
-- address as NetSurf takes one, so a file of this machine comes back as the
-- path it was.
--
local function newtab_page()
  local tiles, rows = {}, {}

  for _, e in ipairs(bar_list) do
    if e.address then
      local letter, ground = favicon_of(e.address)

      tiles[#tiles + 1] = ('<a class="tile" href="%s"><span class="fav" '
                           .. 'style="background: #%06x">%s</span>%s</a>')
                          :format(escaped(ns_address(e.address)), ground & 0xffffff,
                                  escaped(letter), escaped(e.name))
    end
  end

  local lately = history.lately(LATELY_SHOWN)

  for _, l in ipairs(lately) do
    local host = address_parts(l.address)
    local when = day_words(l.day)

    rows[#rows + 1] = ('<p class="it"><a href="%s">%s</a> <small>%s, %s</small></p>')
                      :format(escaped(ns_address(l.address)),
                              escaped(l.title ~= "" and l.title or l.address),
                              escaped(host ~= "" and host or l.address),
                              escaped(when == "Today" and l.time
                                      or when == "Yesterday" and "yesterday" or when))
  end

  if #rows == 0 then
    rows[1] = "<p>Nothing yet: the pages you open are listed here.</p>"
  end

  print(("browser: a new tab's page, %d favorites, %d lately, the newest %s")
        :format(#tiles, #lately, lately[1] and lately[1].address or "none"))

  local favs = #tiles > 0
               and ("<h3>Favorites</h3>\n<p class=\"tiles\">" .. table.concat(tiles, "\n")
                    .. "</p>\n")
               or ""

  return ([[<!doctype html>
<html><head><meta charset="utf-8"><title>New tab</title>
<style>
body { margin: 36px 48px; font-family: sans-serif; color: #1d1f24; }
h3 { font-size: 13px; color: #74787f; margin: 0 0 10px 0; }
p.it { margin: 0; padding: 7px 4px; border-bottom: 1px solid #e3e5e9; }
small { color: #74787f; }
a { color: #1d1f24; text-decoration: none; }
p.tiles { margin: 0 0 26px 0; }
a.tile { display: inline-block; width: 104px; margin: 0 10px 10px 0;
         padding: 12px 6px 10px 6px; text-align: center; font-size: 12px;
         background: #f4f5f7; border: 1px solid #e3e5e9; vertical-align: top; }
a.tile span.fav { display: block; width: 34px; height: 34px; margin: 0 auto 8px auto;
                  line-height: 34px; color: #ffffff; font-weight: bold; font-size: 14px; }
</style></head><body>
%s<h3>Lately</h3>
%s
<p><small>Or search, or type an address, above. <a href="about:start">The page inside this image</a>.</small></p>
</body></html>]]):format(favs, table.concat(rows, "\n"))
end

--
-- A page's bytes from the network, and how they came, following redirects.
--
-- The request is `http.lua`'s. What this adds is the browser's half: a
-- redirect followed, and a certificate refusal turned into the page above
-- rather than a line in the status bar - it is something to decide, not
-- only something to know.
--
-- The words beside the bar: the host, how much of how much, and the part.
local function loading_words()
  local kb = function(n) return (n + 1023) // 1024 end

  if loading.total and loading.total > 0 then
    return ("Loading %s - %d of %d KB, %d%%"):format(loading.host, kb(loading.got),
           kb(loading.total), 100 * loading.got // loading.total)
  end

  return ("Loading %s - %d KB so far"):format(loading.host, kb(loading.got))
end

local function fetch(text, page, post)
  --
  -- The page says each step on the status line and moves the address with
  -- a redirect; a picture does neither. A picture that redirected used to
  -- move both, and `here` with them - so every link on the page after it
  -- resolved against the picture's address rather than the page's.
  --
  local tell = page and say or function() end

  for _ = 1, 5 do
    local parts, bad = http.split(text)

    if not parts then
      tell(bad)
      return nil
    end

    -- Only the page itself moves the bar: its pictures, fetched after it,
    -- would flicker it once each.
    local progress

    if page then
      progress = function(got, total)
        loading = { host = parts.hostport, got = got, total = total }
        say(loading_words())
      end
    end

    local reply, why, how = http.get(parts, {
      say = page and say or nil,
      agent = AGENT,
      progress = progress,
      anyway = parts.scheme == "https" and anyway[parts.hostport] or nil,
      body = post and post.body,
      content_type = post and post.type,
      cache = CACHE,
      revalidate = reloading,
    })

    if page then loading = nil end

    if not reply then
      if how.refused and page then return refused_page(text, how.refused), how end

      tell(why)
      return nil
    end

    local status, head, body = http.parse(reply)

    if status >= 300 and status < 400 then
      local to = head:match("\r\n[Ll][Oo][Cc][Aa][Tt][Ii][Oo][Nn]:%s*([^\r\n]+)")

      if not to then
        tell(("it answered %d and said nowhere to go"):format(status))
        return nil
      end

      local next_at, wrong = resolve(text, to)

      if not next_at then
        tell(("it redirected to %s: %s"):format(to, tostring(wrong)))
        return nil
      end

      tell(("%d, following to %s"):format(status, next_at))
      text = next_at

      -- A form POSTed and answered "see over there" is fetched from there
      -- with a GET, as every browser does; 307 and 308 send it again.
      if status ~= 307 and status ~= 308 then post = nil end

      if page then
        here = next_at
        address.text = next_at
        address.caret = #next_at
        address.from = 0
      end
    else
      -- The charset the server said the page is in, which the parser holds
      -- to over anything the page says of itself (`roadmap.md` 6zz j7).
      how.charset = head:match("\r\n[Cc][Oo][Nn][Tt][Ee][Nn][Tt]%-[Tt][Yy][Pp][Ee]:"
                               .. "[^\r\n]-[Cc][Hh][Aa][Rr][Ss][Ee][Tt]%s*=%s*\"?"
                               .. "([%w%-_:%.]+)")
      return body, how
    end
  end

  tell("too many redirects")

  return nil
end

--
-- How a page came, in the drawing's words: Secure, Not secure, Not
-- encrypted, Refused - or from this machine, which no network had a part in.
--
local function how_said(how)
  if how == nil then return "From this machine" end

  if how.refused then return "Refused: " .. how.refused end

  local words

  if how.scheme ~= "https" then
    words = "Not encrypted"
  elseif how.secure then
    words = "Secure"
  else
    words = "Not secure: " .. tostring(how.reason or "the certificate did not check out")
  end

  -- Kept from before: as it was, or after the server said it still is.
  if how.cached == "fresh" then
    words = words .. ", from the cache"
  elseif how.cached == "revalidated" then
    words = words .. ", from the cache, checked"
  end

  -- A page that stopped before the length the server gave says so first.
  if how.short then
    words = ("Cut short at %d%s KB - %s"):format(how.short.got // 1024,
            how.short.want and (" of %d"):format(how.short.want // 1024) or "",
            words)
  end

  return words
end

--------------------------------------------------------------------------
-- Pictures: fetched from wherever their address says, decoded by `gfx`,
-- and stretched into the boxes `web_paint.c` left for them.
--------------------------------------------------------------------------

-- A picture's bytes, from the three places a page itself comes from.
local function bytes_at(where)
  if where:match("^asset:") then return sys.asset(where:sub(7)) end

  if where:sub(1, 1) == "/" then
    local got = fs.read(where)

    return type(got) == "string" and got or nil
  end

  return fetch(where)
end

--
-- **The network's pictures together** (`roadmap.md` 6zz g): every one the
-- band holds, fetched with `http.get_many` - side by side, names
-- remembered - where each was fetched after the one before. gnu.org's page
-- spent nine seconds of ten waiting on its twelve that way. A picture that
-- redirects is followed on its own; one the image carries or a file on this
-- machine is read as before.
--
local function fetch_pictures(wanted, noun)
  local urls, seen, got = {}, {}, {}

  for _, w in ipairs(wanted) do
    local where = w.where

    if where and not where:match("^asset:") and where:sub(1, 1) ~= "/"
       and not seen[where] then
      seen[where] = true
      urls[#urls + 1] = where
    end
  end

  if #urls == 0 then return got end

  say(("fetching %d %s ..."):format(#urls, noun or "pictures"))

  local results = http.get_many(urls, {
    agent = AGENT,
    anyway = function(parts)
      return parts.scheme == "https" and anyway[parts.hostport] or nil
    end,
    cache = CACHE,
    revalidate = reloading,
  })

  for i, r in ipairs(results) do
    if r[1] then
      local status, _, body = http.parse(r[1])

      if status >= 300 and status < 400 then
        got[urls[i]] = fetch(urls[i])
      elseif status >= 200 and status < 300 and not (r[3] and r[3].short) then
        got[urls[i]] = body
      end
    end
  end

  return got
end

--
-- The pictures a band holds, fetched and decoded the first time a band
-- holding them is painted, and drawn into it.
--
-- Each one into its box, with its shape kept and centred if the box is
-- another shape. PNG or JPEG, told apart by their first bytes rather than
-- by a name, which a server is free to get wrong. One that cannot be had
-- leaves its grey box and is counted, and the page is still the page.
--
-- Bilinear when a picture is scaled, because a screenshot shrunk by nearest
-- neighbour loses whole rows of text. At its own size the two are the same.
--
-- Decoded once and kept, each scaled to its box, for as long as the page is
-- open - within an eighth of the memory that would be free without them. Past
-- that the ones farthest from the band are let go, and fetched again if the
-- view comes back to them, which is a cost in time rather than a page that
-- fails for having too many pictures.
--
local function in_band(p)
  local im = p.im

  return im.y + im.h > band_top and im.y < band_top + paper_h
end

-- Never one the band holds, which is about to be drawn.
local function keep_within(room)
  if kept_bytes <= room then return end

  local centre, far = band_top + paper_h // 2, {}

  for _, p in ipairs(pictures) do
    if p.kept and not in_band(p) then far[#far + 1] = p end
  end

  table.sort(far, function(a, b)
    return math.abs(a.y + a.h // 2 - centre) > math.abs(b.y + b.h // 2 - centre)
  end)

  for _, p in ipairs(far) do
    if kept_bytes <= room then break end

    kept_bytes = kept_bytes - p.w * p.h * 4
    p.kept:free()
    p.kept = nil
  end
end

local function band_pictures()
  local wanted = {}

  for _, p in ipairs(pictures) do
    if p.kept == nil and in_band(p) then wanted[#wanted + 1] = p end
  end

  if #wanted > 0 and setting.images then
    -- The status line says so while they come, and then says again what
    -- it said: a band painted while scrolling is not news.
    local before = said
    local fetched = fetch_pictures(wanted)

    for _, p in ipairs(wanted) do
      local im, where = p.im, p.where
      local bytes = where and (fetched[where] or ((where:match("^asset:")
                    or where:sub(1, 1) == "/") and bytes_at(where)))
      local decode = bytes and ((bytes:sub(1, 4) == "\x89PNG" and gfx.png)
                                or (bytes:sub(1, 2) == "\xff\xd8" and gfx.jpeg))
      local ok, pic = false, nil

      if decode then ok, pic = pcall(decode, bytes) end

      p.kept = false

      if ok and pic then
        local pw, ph = pic:size()
        local k = math.min(im.w / pw, im.h / ph)
        local w, h = math.floor(pw * k + 0.5), math.floor(ph * k + 0.5)

        if w > 0 and h > 0 and pcall(function()
             p.kept = gfx.surface{ w = w, h = h }
           end) and p.kept then
          p.kept:stretch(pic, 0, 0, pw, ph, 0, 0, w, h, nil, true)
          p.x, p.y, p.w, p.h = im.x + (im.w - w) // 2, im.y + (im.h - h) // 2, w, h
          kept_bytes = kept_bytes + w * h * 4
        else
          p.kept = false
        end

        pic:free()
      end
    end

    keep_within((((sys.info() or {}).pages_free or 16384) * 4096 + kept_bytes) // 8)
    said = before
  end

  local drawn, missing = 0, 0

  for _, p in ipairs(pictures) do
    if in_band(p) then
      if p.kept then
        paper:blit(p.kept, 0, 0, p.w, p.h, p.x, p.y - band_top)
        drawn = drawn + 1
      else
        missing = missing + 1
      end
    end
  end

  return drawn, missing
end

--
-- An SVG drawn at `w` by `h`, kept no larger than a page could show.
local function svg_surface(svg, w, h)
  local most = math.max(PAGE_W, VIEW_H) * 2

  if w > most or h > most then
    local k = most / math.max(w, h)

    w, h = math.max(1, math.floor(w * k)), math.max(1, math.floor(h * k))
  end

  local made, pic = pcall(gfx.surface, { w = w, h = h })

  if not made or not pic then return nil end

  -- A picture there is no memory to draw is a picture missing, not a
  -- browser stopped.
  if not pcall(svg.draw, svg, pic) then
    pic:free()
    return nil
  end

  return pic, w, h
end

--
-- A picture's bytes decoded, PNG or JPEG by their first bytes and SVG by
-- its tag; nil for anything else. Kept no wider than the page, which is as
-- wide as NetSurf will ever draw it - a photograph of 4000 pixels in a
-- column of 250 does not keep 48 MB - and its own size said beside it,
-- which is what the layout measures a picture the page gave no size to by.
-- An SVG is drawn at its own size here, and at its box's once the layout
-- has given it one (`pictures_to_boxes`); `k` is which picture it is.
--
local function picture_of(bytes, k)
  if bytes and web and bytes:sub(1, 1024):find("<svg", 1, true) then
    local svg = web.svg(bytes)

    if not svg then return nil end

    local sw, sh = svg:size()
    local w = math.min(sw, PAGE_W)
    local pic, dw, dh = svg_surface(svg, w, math.max(1, sh * w // sw))

    if pic and k then ns_svgs[k] = { svg = svg, w = dw, h = dh } end

    return pic, sw, sh
  end

  local decode = bytes and ((bytes:sub(1, 4) == "\x89PNG" and gfx.png)
                            or (bytes:sub(1, 2) == "\xff\xd8" and gfx.jpeg))
  local ok, pic = false, nil

  if decode then ok, pic = pcall(decode, bytes) end
  if not ok or not pic then return nil end

  local pw, ph = pic:size()

  if pw > PAGE_W then
    local h = math.max(1, ph * PAGE_W // pw)
    local made, small = pcall(gfx.surface, { w = PAGE_W, h = h })

    if made and small then
      small:stretch(pic, 0, 0, pw, ph, 0, 0, PAGE_W, h, nil, true)
      pic:free()
      pic = small
    end
  end

  return pic, pw, ph
end

--
-- Each SVG drawn again at its box's size, where the layout gave it one that
-- is not the size it was drawn at: its own, or the page's width, until the
-- page said otherwise; and each other picture scaled to its box once. The
-- natural size goes back unchanged, so the layout does not move.
--
local function pictures_to_boxes()
  if not next(ns_svgs) and not next(ns_raster) then return end

  local most = math.max(PAGE_W, VIEW_H) * 2

  for k, o in ipairs(doc:ns_objects()) do
    local s, r = ns_svgs[k], ns_raster[k]

    if s and o.w > 0 and o.h > 0 and (o.w ~= s.w or o.h ~= s.h) then
      local pic = svg_surface(s.svg, o.w, o.h)

      if pic then
        local sw, sh = s.svg:size()

        doc:ns_picture(k, pic, sw, sh)
        s.w, s.h = o.w, o.h
      end
    elseif r and not o.background and o.w > 0 and o.h > 0
           and o.w <= most and o.h <= most
           and (o.w ~= r.w or o.h ~= r.h) then
      -- Scaled from the picture as decoded, not from the last scaling,
      -- so a box that changes size again loses nothing.
      local made, scaled = pcall(gfx.surface, { w = o.w, h = o.h })

      if made and scaled then
        local sw, sh = r.pic:size()

        scaled:stretch(r.pic, 0, 0, sw, sh, 0, 0, o.w, o.h, nil, true)
        doc:ns_picture(k, scaled, r.pw, r.ph)
        r.w, r.h = o.w, o.h
      end
    end
  end
end

--
-- NetSurf's pictures in the band: the ones its layout asked for whose boxes
-- reach into it, fetched side by side the first time, handed over, and the
-- page laid out and the band drawn again - a picture the page gave no size
-- to takes its own, and can move others into the band, so up to three
-- rounds. Returns how many in the band were drawn and how many could not
-- be had.
--
local function ns_band_pictures()
  local function in_view(o)
    return o.y + math.max(o.h, 1) > band_top and o.y < band_top + paper_h
  end

  for _ = 1, 3 do
    local wanted = {}

    for k, o in ipairs(doc:ns_objects()) do
      if not o.arrived and not ns_tried[k] and in_view(o) then
        wanted[#wanted + 1] = { k = k, where = from_ns(o.url) }
      end
    end

    -- Settings' Load images off: the boxes laid out, nothing asked for.
    if #wanted == 0 or not setting.images then break end

    local before = said
    local fetched = fetch_pictures(wanted)
    local arrived = 0

    for _, w in ipairs(wanted) do
      local where = w.where
      local bytes = fetched[where] or ((where:match("^asset:")
                    or where:sub(1, 1) == "/") and bytes_at(where))
      local pic, pw, ph = picture_of(bytes, w.k)

      ns_tried[w.k] = true

      if pic and doc:ns_picture(w.k, pic, pw, ph) then
        arrived = arrived + 1

        if not ns_svgs[w.k] then
          local sw, sh = pic:size()

          ns_raster[w.k] = { pic = pic, pw = pw, ph = ph, w = sw, h = sh }
        end
      end
    end

    said = before

    if arrived == 0 then break end

    local tall = doc:ns_layout(PAGE_W, VIEW_H, ns_address(here))

    if tall then content_h = tall end

    band_top = math.max(0, math.min(band_top, content_h + 16 - paper_h))
    top = math.max(0, math.min(top, math.max(0, content_h - VIEW_H)))
    pictures_to_boxes()
    doc:ns_paint(paper, PAGE_W, paper_h, band_top)
  end

  local drawn, missing = 0, 0

  for k, o in ipairs(doc:ns_objects()) do
    if in_view(o) then
      if o.arrived then
        drawn = drawn + 1
      elseif ns_tried[k] then
        missing = missing + 1
      end
    end
  end

  return drawn, missing
end

--
-- The band starting `at` rows down the page, painted: its runs, which the
-- layout already placed, and its pictures. Returns how many of those were
-- drawn and how many could not be had.
--
paint_band = function(at)
  band_top = math.max(0, math.min(at, content_h + 16 - paper_h))

  if ns_doc then
    doc:ns_paint(paper, PAGE_W, paper_h, band_top)
  else
    doc:render(paper, PAGE_W, paper_h, band_top)
  end

  local t0 = sys.ticks()
  local shown, missing

  if ns_doc then
    shown, missing = ns_band_pictures()
  else
    shown, missing = band_pictures()
  end

  pictures_ms = since(t0)
  return shown, missing
end

--
-- The page's linked stylesheets, fetched side by side before it is laid out
-- (`roadmap.md` 6zz j4) - the cascade is made from them all, in the order
-- the page gives them, at the first layout. Wikipedia keeps every one of its
-- rules in these; without them its menus and its languages were twelve
-- screens of lists before the article.
--
local function fetch_sheets(d)
  if not NS then return 0 end

  local wanted = d:ns_sheets(ns_address(here))

  if #wanted == 0 then return 0 end

  local list, n = {}, 0

  for _, sheet in ipairs(wanted) do list[#list + 1] = { where = from_ns(sheet.url) } end

  local got = fetch_pictures(list, "stylesheets")

  for i, sheet in ipairs(wanted) do
    local where = list[i].where
    local text = got[where] or ((where:match("^asset:")
                 or where:sub(1, 1) == "/") and bytes_at(where))

    if text and d:ns_sheet(sheet.n, text) then n = n + 1 end
  end

  return n
end

local function load_page(text, post)
  if web == nil then
    say("this image has no web kit - build it with `make WEB=1`")
    return false
  end

  --
  -- Three ways to get a document, and only one of them is the network.
  --
  -- `about:start` is the page compiled into this file, `/anything` is read
  -- from the namespace, and the rest is fetched. A browser that could only
  -- do the third could not be tried without a server somewhere, which is
  -- what this arrangement exists to fix.
  --
  here = text
  address.text = (text == NEWTAB or text == prefs.PAGE) and "" or text
  address.caret = #address.text
  address.from = 0

  local body, fetched_ms, how

  --
  -- **Settings is no document**: the browser's own widgets, over the page
  -- (`settings_page`). What was shown is let go as any page is when the
  -- next one comes, and Back goes to it.
  --
  if text == prefs.PAGE then
    if doc then doc:close() end

    if paper then paper:free() end

    forget_pictures()
    doc, came, paper, paper_h, band_top, content_h, top = nil, nil, nil, 0, 0, 0, 0
    ns_doc, ns_tried, ns_svgs, ns_raster = false, {}, {}, {}
    current.title, timing = "Settings", ""
    win:retitle("Browser - Settings")
    fill_settings()
    say("Settings")
    print("browser: showing about:settings, Settings")
    return true
  end

  if text == HOME then
    say("the page inside this image")
    body, fetched_ms = START, 0

  elseif text == NEWTAB then
    say("a new tab")
    body, fetched_ms = newtab_page(), 0

  elseif text == prefs.ROOTS then
    --
    -- **The authorities this machine trusts**, Settings' Show: the names
    -- the build read out of Mozilla's bundle (`ca/roots.txt`), and the
    -- ones a person added in `/Home/Preferences/Authorities`.
    --
    local names = sys.asset("ca/roots.txt") or ""
    local rows, as_of = {}, nil

    for line in names:gmatch("[^\n]+") do
      if not as_of and line:match("^as of ") then
        as_of = line:sub(7)
      else
        rows[#rows + 1] = "<li>" .. escaped(line) .. "</li>"
      end
    end

    local own = {}

    for _, name in ipairs(fs.list(http.AUTHORITIES) or {}) do
      own[#own + 1] = "<li>" .. escaped(name) .. "</li>"
    end

    body, fetched_ms = ([[<!doctype html>
<html><head><meta charset="utf-8"><title>Trusted authorities</title></head><body>
<h1>Trusted authorities</h1>
<p>A page over HTTPS is shown as Secure when its certificate was signed by one
of these.</p>
<h2>Added here, in /Home/Preferences/Authorities</h2>
<ul>%s</ul>
<h2>Mozilla's %d, as of %s</h2>
<ul>%s</ul>
</body></html>]]):format(#own > 0 and table.concat(own) or "<li>None</li>", #rows,
                      escaped(as_of or "the build"), table.concat(rows)), 0
    say("the authorities this machine trusts")

  elseif text:match("^asset:") then
    --
    -- A page the image carries - Cafesa3D's tutorial is one - read where it
    -- lies. Nothing is copied anywhere to be shown, so it is always the
    -- page that came with this build, and its links and pictures resolve
    -- beside it the same way.
    --
    body = sys.asset(text:sub(7))

    if not body then
      say(("this image carries no %s"):format(text:sub(7)))
      return false
    end

    fetched_ms = 0
    say(("read %d bytes from the image"):format(#body))

  elseif text:sub(1, 1) == "/" then
    local began = sys.ticks()
    local got, why = fs.read(text)

    if type(got) ~= "string" then
      say(("cannot read %s: %s"):format(text, tostring(why or "not a file")))
      return false
    end

    body, fetched_ms = got, since(began)
    say(("read %d bytes from %s"):format(#body, text))

  else
    local fetch_from = sys.ticks()

    body, how = fetch(text, true, post)

    if body == nil then return false end

    fetched_ms = since(fetch_from)
  end

  say(("parsing %d bytes..."):format(#body))

  local parse_from = sys.ticks()
  local fresh, bad = web.parse(body, how and how.charset)
  local parsed_ms = since(parse_from)

  if not fresh then
    local short = how and how.short
                  and (" - cut short at %d%s KB"):format(how.short.got // 1024,
                       how.short.want and (" of %d"):format(how.short.want // 1024) or "")
                  or ""

    say(("%d bytes%s, and it did not parse: %s"):format(#body, short, tostring(bad)))
    return false
  end

  -- The one before it, and only once this one exists: a page that fails to
  -- parse should leave what is on screen alone rather than blank it.
  if doc then doc:close() end

  doc = fresh
  came = how

  local title = doc:title()

  current.title = title

  -- **In the history** (d4), on the disk: every page shown in any tab,
  -- but a new tab's own and one refused for its certificate.
  if text ~= NEWTAB and text ~= prefs.ROOTS and not (came and came.refused) then
    history.record(text, title, clock.now())
  end

  --
  -- `retitle`, not `win.title = ...`. The title bar belongs to the desktop
  -- and is drawn by it, so changing the field here changes a local copy and
  -- nothing on screen - which is exactly what the first screenshot showed:
  -- a window called "Browser" displaying a page called something else.
  --
  win:retitle(title and ("Browser - " .. title) or "Browser")

  local counts = ("%d bytes, %d paragraphs, %d links, %d headings")
                 :format(#body, doc:count("p"), doc:count("a"),
                         doc:count("h1") + doc:count("h2") + doc:count("h3"))

  local sheets_from = sys.ticks()

  fetch_sheets(doc)
  fetched_ms = fetched_ms + since(sheets_from)

  local drawn, why_not = lay_out(doc)
  local shown, missing = 0, 0

  if drawn then
    local t1 = sys.ticks()

    shown, missing = paint_band(0)
    painted_ms = since(t1)
  end

  if not drawn then
    say(counts .. " - " .. tostring(why_not))
    return false
  end

  --
  -- Five numbers, in the order the bytes go through them. Read as
  -- proportions rather than as speeds: this is QEMU, and `CLAUDE.md` is
  -- clear about what a number from QEMU is worth. What it is worth is
  -- knowing which of the five to work on - and the band's pictures are
  -- their own, since fetching them was being read as painting.
  --
  timing = ("fetch %s  parse %s  layout %s  pictures %s  paint %s ms")
           :format(tenths(fetched_ms), tenths(parsed_ms), tenths(laid_ms),
                   tenths(pictures_ms),
                   tenths(math.max(0, painted_ms - pictures_ms)))

  say(how_said(came) .. " - " .. counts)

  if missing > 0 then
    say(("%d of %d pictures could not be read or decoded"):format(missing,
        shown + missing))
  end

  -- For whoever opened it from outside - Cafesa3D's suite asks for its
  -- tutorial and waits to hear the page arrive (`tools/run_cafesa3d.py`).
  -- The pictures are the first band's, which are the ones fetched before
  -- the page is shown.
  print(("browser: showing %s, \"%s\", %d pixels tall, %d pictures, %d missing, %s")
        :format(text, title or "", content_h, shown, missing, how_said(came)))

  -- Its costs as the status line has them, for whoever measures from
  -- outside - a page from the cache is a fetch of nothing (6zz k).
  print("browser: took " .. timing)

  --
  -- And what drawing its first band spent, by kind (`roadmap.md` 6zz h):
  -- the band last painted, which with pictures is the last of up to three.
  -- What is left of the whole is NetSurf walking its boxes.
  --
  local c = ns_doc and doc:ns_costs()

  if c then
    local function ms(t) return tenths((t * 10000) // HZ) end
    local rest = c.whole.ticks - c.fills.ticks - c.text.ticks
                 - c.pictures.ticks - c.scaled.ticks - c.shapes.ticks
                 - c.other.ticks

    print(("browser: painted in %s ms - fills %s (%d), text %s (%d), "
           .. "pictures %s (%d), scaled %s (%d), shapes %s (%d), "
           .. "clips %s (%d), boxes %s")
          :format(ms(c.whole.ticks), ms(c.fills.ticks), c.fills.calls,
                  ms(c.text.ticks), c.text.calls, ms(c.pictures.ticks),
                  c.pictures.calls, ms(c.scaled.ticks), c.scaled.calls,
                  ms(c.shapes.ticks), c.shapes.calls,
                  ms(c.other.ticks), c.other.calls, ms(rest)))
  end

  return true
end

--
-- **The tabs that are open, kept** (d5), for "When the browser opens: The
-- tabs it had": their addresses and which is shown, written as they change
-- - a page shown, a tab closed - whatever Settings says now, so choosing it
-- later has something to open. A new tab's own page is nothing to keep.
--
local function keep_tabs()
  local list, shown = {}, 1

  for _, t in ipairs(tabs) do
    local at = (t == current) and here or (t.here or t.pending)

    if at and at ~= NEWTAB then
      list[#list + 1] = at

      if t == current then shown = #list end
    end
  end

  prefs.save_tabs(list, shown)
end

--
-- One load, with the tab saying where it is going while it goes - its
-- title is the host's until the page arrives with one of its own.
--
--
-- **A page that says to go somewhere else** - `<meta http-equiv="refresh"
-- content="0; url=...">` (`roadmap.md` 6zz, meta refresh). DuckDuckGo's
-- front page, to a browser that runs no scripts, is a hidden body and one
-- of these to its page without them, and this browser showed the hidden
-- body: nothing. Gone to at once when it says so, as a redirect is - the
-- page that said it is not kept in the history - and five of those in a
-- row at most, since two pages can send each other round; after its
-- seconds otherwise, while the same page is still shown (`on_frame`, below).
-- A page that only refreshes itself at once is not followed.
--
local function refresh_of(text)
  if not doc then return nil end

  for _, m in ipairs(doc:meta()) do
    if tostring(m.http_equiv or ""):lower() == "refresh" then
      local seconds, to = http.refresh(m.content)

      if seconds then
        if not to then return seconds, text end

        local whole = ns_doc and web.join and web.join(ns_address(text), to)

        return seconds, whole and from_ns(whole) or resolve(text, to) or to
      end
    end
  end

  return nil
end

local function load(text, post, refreshed)
  -- A favorite's file - Tracker hands the browser one it opens - is the
  -- page it keeps.
  if type(text) == "string" and text:sub(1, 1) == "/" then
    text = favorites.address_of(text) or text
  end

  current.going = text
  current.refresh = nil
  frame()

  local ok = load_page(text, post)

  current.going = nil
  keep_tabs()

  local seconds, to = nil, nil

  if ok then seconds, to = refresh_of(text) end

  if seconds and seconds < 1 and to ~= text and (refreshed or 0) < 5 then
    print(("browser: refreshed to %s"):format(to))
    return load(to, nil, (refreshed or 0) + 1)
  elseif seconds and seconds >= 1 then
    current.refresh = { at = sys.ticks() + seconds * HZ, to = to, from = text }
    say(("this page goes to %s in %d s"):format(to, seconds))
    print(("browser: goes to %s in %d s"):format(to, seconds))
  end

  return ok
end

--------------------------------------------------------------------------
-- Where we have been.
--
-- Two stacks and one current address, which is the whole of it: going back
-- moves the current one onto the forward stack, and following a new address
-- throws that stack away. Nothing here knows about the network.
--------------------------------------------------------------------------

--
-- `post`, for a form sent by POST, is its body and type. History keeps the
-- address alone, so Back to such a page asks for it again with a GET - as a
-- page that came by a link would be.
--
local function visit(text, post)
  if here then back[#back + 1] = here end

  forward = {}
  load(text, post)
end

go_back = function()
  if #back == 0 then
    say("nothing to go back to")
    return
  end

  if here then forward[#forward + 1] = here end

  load(table.remove(back))
end

go_forward = function()
  if #forward == 0 then
    say("nothing to go forward to")
    return
  end

  if here then back[#back + 1] = here end

  load(table.remove(forward))
end

reload = function()
  reloading = true
  load(here or address.text)
  reloading = false
end

go_home = function()
  visit(setting.home)
end

--------------------------------------------------------------------------
-- Scrolling.
--------------------------------------------------------------------------

local function scroll_to(y)
  local was = top

  top = math.max(0, math.min(y, reach()))
  band_ms = nil

  -- Out of the band, and the band moves to put the view in its middle, so
  -- the next screen either way is already painted.
  if paper and (top < band_top or top + VIEW_H > band_top + paper_h) then
    local t0 = sys.ticks()

    paint_band(top - (paper_h - VIEW_H) // 2)
    band_ms = since(t0)
  end

  return top ~= was
end

local function scroll_by(dy)
  return scroll_to(top + dy)
end

--------------------------------------------------------------------------
-- Events.
--
-- A direct window draws its own pixels, so `window:paint` has nothing to
-- send and returns early - but the event loop is still the kit's, and it
-- routes keys and clicks through the view tree. So there is one view here,
-- the size of the window, and it never draws: it exists to be the thing
-- events arrive at.
--------------------------------------------------------------------------

--
-- **What a form did** (`roadmap.md` 6zz j6): the part of the page it
-- changed drawn again - a field's text and its caret, a checkbox - and a
-- form it sent fetched, as a link is followed. NetSurf's own form code
-- keeps the fields, edits their text and encodes what is sent; the page
-- says what changed and what was sent, and this is the browser doing what
-- it says.
--
local function form_changed()
  if not (doc and ns_doc) then return end

  local x, y, w, h = doc:ns_dirty()

  if x and paper then doc:ns_paint(paper, PAGE_W, paper_h, band_top, x, y, w, h) end

  local url, body, kind = doc:ns_sent()

  if url then
    say(("sending the form to %s"):format(url))
    visit(from_ns(url), body and { body = body, type = kind } or nil)
  end
end

-- The caret out of the page's field, for the address bar to have it.
local function page_blur()
  if doc and ns_doc then
    doc:ns_blur()
    form_changed()
  end
end

--
-- **The page laid out again at the window's new size** (6zz e): the same
-- box tree NetSurf already built, at the new width - which is what a
-- reflow is, and the reason resizing waited for NetSurf's layout - its
-- pictures kept and scaled once to their new boxes, the band made again
-- for the new view, and painted around where the page was being read.
--
--
-- The band made for the view as it is, and painted around where the page is
-- read: after a reflow, and when a tab is shown again, whose band went when
-- another was shown.
--
local function make_band()
  if paper then
    paper:free()
    paper = nil
  end

  local tall = math.max(VIEW_H, math.min(content_h + 16, VIEW_H * BAND_SCREENS))
  local made = pcall(function()
    paper = gfx.surface{ w = PAGE_W, h = tall }
  end)

  if not made or not paper then
    paper, paper_h = nil, 0
    say(("no memory for a %dx%d band of the page"):format(PAGE_W, tall))
    return false
  end

  paper_h = tall
  top = math.max(0, math.min(top, math.max(0, content_h - VIEW_H)))
  paint_band(top - (paper_h - VIEW_H) // 2)
  return true
end

local function reflow()
  if not doc then return end

  if ns_doc then
    local tall = doc:ns_layout(PAGE_W, VIEW_H, ns_address(here))

    if tall then content_h = tall end
  else
    content_h = doc:render(nil, PAGE_W) or content_h
  end

  laid_for = PAGE_W * 65536 + VIEW_H + setting.zoom * 4294967296

  if ns_doc then pictures_to_boxes() end

  if not make_band() then return end

  -- Said, for whoever is watching it happen (`tools/run_browser.py`) -
  -- and what it is drawn into, which is the region the kit handed over.
  local sw, sh = win:surface():size()

  print(("browser: laid out again at %dx%d, %d pixels tall, drawn at %dx%d")
        :format(PAGE_W, VIEW_H, content_h, sw, sh))
end

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

--
-- The wheel: three lines of the page a notch - forty pixels each, as an
-- arrow moves it - away from the person up (`roadmap.md` 5zv).
--
function sink:wheel(n)
  if scroll_by(-n * 3 * 40) then frame() end
  return true
end

function sink:key(c)
  -- A field on the page with the caret has the keyboard - every letter,
  -- space and arrow - and what it does not want (Control-L) goes on.
  if ns_doc and doc and doc:ns_focused() and doc:ns_key(c) then
    form_changed()
    frame()
    return true
  end

  local screen = VIEW_H - gfx.height() * 2

  if c == ui.UP then scroll_by(-40)
  elseif c == ui.DOWN then scroll_by(40)
  elseif c == 32 then scroll_by(screen)              -- space
  elseif c == 98 then scroll_by(-screen)             -- b
  elseif c == 103 then scroll_to(0)                  -- g
  elseif c == 71 then scroll_to(reach())             -- G
  elseif c == 12 then                                -- Control-L
    page_blur()
    field:take_keys()
  elseif c == 114 then reload()                      -- r
  elseif c == 91 then go_back()                      -- [
  elseif c == 93 then go_forward()                   -- ]
  else return false end

  --
  -- The *previous* repaint's cost, shown by this one. A frame cannot report
  -- its own total before it has drawn the line it would report it on, and
  -- while a key is held down one frame behind is the same number.
  --
  timing = ("frame %s (page %s, commit %s) worst %s ms, %s KB")
           :format(tenths(frame_cost.ms), tenths(frame_cost.blit),
                   tenths(frame_cost.commit), tenths(frame_cost.worst),
                   ("%d.%02d"):format(frame_cost.kb // 100, frame_cost.kb % 100))
           .. (band_ms and (", band %s ms"):format(tenths(band_ms)) or "")

  -- Returned as taken, so the kit repaints - which is this window's
  -- `frame`, below (`on_paint`).
  return true
end

local function scrollbar_press(y)
  local ty, th = thumb()

  if not ty then
    return
  elseif y < ty then
    scroll_by(-(VIEW_H - gfx.height() * 2))
  elseif y >= ty + th then
    scroll_by(VIEW_H - gfx.height() * 2)
  else
    dragging = y - ty
  end
end

--
-- The thumb, while it is held.
--
-- The pointer's offset *into* the thumb is remembered at the press, so the
-- page does not jump the moment the drag starts: what is under the pointer
-- stays under it, which is the one thing a scrollbar has to get right.
--
local function scrollbar_drag(y)
  local track = VIEW_H - 4
  local _, th = thumb()
  local room = track - th

  if room <= 0 then return end

  scroll_to(((y - dragging - VIEW_Y - 2) * reach()) // room)
end

--
-- A click on the page, which may be a click on a link.
--
-- The layout kept its boxes, so this is a comparison rather than a search:
-- the click is turned into a point on the *page* - the tall surface the
-- document was laid out into, which is `top` pixels above the top of the
-- view - and `link_at` answers with whatever href covers it.
--
-- Following it is a `visit`, so it joins the history like anything typed.
--
local function page_press(x, y)
  if not doc or not paper then return end

  -- A form's field first: a click there is the field's, and a click
  -- anywhere else takes the caret out of whichever had it.
  if ns_doc then
    local did = doc:ns_click(x - VIEW_X - PAD, y - VIEW_Y + top)

    form_changed()

    if did then return end
  end

  local href

  if ns_doc then
    href = doc:ns_link_at(x - VIEW_X - PAD, y - VIEW_Y + top)
  else
    href = doc:link_at(x - VIEW_X - PAD, y - VIEW_Y + top)
  end

  if not href then return end

  -- The refused page's two links, which are this browser's rather than
  -- anybody's address.
  if href == "kosmos:back" then
    go_back()
    return
  elseif href == "kosmos:anyway" then
    local parts = here and http.split(here)

    if parts and parts.scheme == "https" then
      anyway[parts.hostport] = true
      reload()
    end

    return
  end

  local where, why

  if ns_doc then
    -- Whole already: NetSurf joined it to the page as it laid the page
    -- out. A link into this page - `#top` - has nowhere to go yet.
    where = from_ns(href)

    if where:find("#", 1, true)
       and where:gsub("#.*$", "") == tostring(here or ""):gsub("#.*$", "") then
      where, why = nil, "that link points into this page, and there is no anchor yet"
    end
  else
    where, why = resolve(here or address.text, href)
  end

  if not where then
    say(("%s: %s"):format(href, why))
    return
  end

  visit(where)
end

--
-- The page's costs, broken down: opened by a click on the right of the
-- status line, where they are - the five numbers the line shows, and what
-- the last paint spent by kind (`roadmap.md` 6zz h).
--
local function breakdown(x)
  local items = {}

  for part in timing:gmatch("[^%s][^%s]*%s+[%d.]+") do
    items[#items + 1] = { text = part .. " ms" }
  end

  local c = ns_doc and doc and doc:ns_costs()

  if c then
    local function ms(t) return tenths((t * 10000) // HZ) end

    items[#items + 1] = { separator = true }
    items[#items + 1] = { text = ("painting %s ms, of which"):format(ms(c.whole.ticks)) }

    for _, k in ipairs({ "fills", "text", "pictures", "scaled", "shapes" }) do
      items[#items + 1] = { text = ("    %s %s ms, %d"):format(k, ms(c[k].ticks),
                                                           c[k].calls) }
    end
  end

  if #items > 0 then
    win:open_menu((win.origin_x or 0) + math.max(0, x - 120),
                  (win.origin_y or 0) + H - STAT - 24 * #items - 8, items)
  end
end

function sink:mouse(action, x, y)
  if action == "press" then
    if y >= VIEW_Y and y < VIEW_Y + VIEW_H and x >= W - SBAR then
      scrollbar_press(y)
    elseif y >= VIEW_Y and y < VIEW_Y + VIEW_H and x >= VIEW_X then
      page_press(x, y)
    elseif timing ~= "" and setting.costs and x >= W - 10 - gfx.measure(timing) then
      breakdown(x)
    end
  elseif action == "move" then
    if dragging then scrollbar_drag(y) end
  elseif action == "release" then
    dragging = nil
  end

  -- Taken, so the kit repaints: `on_paint`, below.
  return true
end

win:add(sink)

--
-- **The header's widgets, wired**, now that what they do exists: going
-- back and forward, reloading, an address typed and Return pressed, and
-- the menu - home, reload, and the cache emptied (`roadmap.md` 6zz k).
-- Added after the page, so a press on them is theirs.
--
--
-- **And the keyboard back to the page** after a press on one, as every
-- browser does: a button that kept the focus took the next space for a
-- press of itself, and the page that should have scrolled went back
-- instead - which is how the suite found it.
--
local function then_page(act)
  return function(...)
    win:focus_on(sink)
    act(...)
  end
end

hb.back.on_click = then_page(function() go_back() end)
hb.forward.on_click = then_page(function() go_forward() end)
hb.reload.on_click = then_page(function() reload() end)

--
-- **An address, or words to search for** (`roadmap.md` 6zz d6): what looks
-- like an address is gone to, and anything else searched for at the engine
-- Settings chose - DuckDuckGo's page without scripts unless it says Google
-- (`browserprefs.destination`).
--
field.on_enter = function(_, text)
  local where, searched = prefs.destination(text, setting.search)

  if not where then return end

  win:focus_on(sink)

  if searched then
    print(("browser: searching for \"%s\" at %s")
          :format(tostring(text):match("^%s*(.-)%s*$"), where))
  end

  visit(where)
end

-- Escape gives the page the keys back, the address as it was.
do
  local field_key = field.key

  function field:key(c)
    -- A press on the field and typing in the same batch of keys: the
    -- address taken before the first of them rather than at the frame after
    -- (`take_keys` says why that is too late).
    if not self.had then
      self:take_address()
      self.had = true
    end

    if c == 27 then
      win:focus_on(sink)
      return true
    end

    return field_key(self, c)
  end
end

hb.menu.on_click = function()
  win:focus_on(sink)
  --
  -- **On the screen, from the window's corner**: `open_menu` places a
  -- window of its own, so it takes the screen's coordinates, as every other
  -- application's call adds `origin_x` to say. These four said the window's,
  -- and opened displaced by wherever the window was - nothing had pressed
  -- one where it shows until Settings was reached through this one (d5).
  --
  win:open_menu((win.origin_x or 0) + hb.menu.x, (win.origin_y or 0) + TABS + HEAD - 2, {
    { text = "New tab", on_choose = function() new_tab() end },
    { text = "Close tab", on_choose = function() close_tab(current) end },
    { separator = true },
    { text = (here and kept[here]) and "Remove from favorites" or "Add to favorites",
      on_choose = function() toggle_favorite() end },
    { text = side_open and "Hide favorites and history" or "Favorites and history",
      on_choose = function() toggle_side() end },
    { separator = true },
    { text = ("Zoom, %d%%"):format(setting.zoom), submenu = (function()
        local items = {}

        for _, c in ipairs(prefs.CHOICES.zoom) do
          items[#items + 1] = { text = c[2], mark = c[1] == setting.zoom,
                                on_choose = function() set_zoom(c[1]) end }
        end

        return items
      end)() },
    { text = "Settings", on_choose = function() open_settings() end },
    { separator = true },
    { text = "Home", on_choose = function() go_home() end },
    { text = "Reload", on_choose = function() reload() end },
    { separator = true },
    { text = "Empty the cache", on_choose = function()
        CACHE:empty()
        say("the cache is empty")
      end },
  })
end

win:add(strip)
win:add(header)
win:add(favbar)
win:add(side)
win:add(settings_page)

--
-- **This window's own paint** (`on_paint`): the kit calls it whenever an
-- event changed something - a press on a header button, a key - since a
-- window that draws its own pixels has nothing for the kit to draw.
--
win.on_paint = function() frame() end

--
-- **Where a link goes, said as the pointer passes over it** (`docs/
-- browser.html`): the window asks to be told where the pointer is while it
-- has the focus (`track`), and the status line's left says the address of
-- the link under it.
--
use("/Kosmos/Libraries/wmproto.lua").track(win.handle, true)

win.on_hover = function(_, x, y)
  local now = nil

  if ns_doc and doc and y >= VIEW_Y and y < VIEW_Y + VIEW_H
     and x >= VIEW_X and x < W - SBAR then
    local href = doc:ns_link_at(x - VIEW_X - PAD, y - VIEW_Y + top)

    now = href and from_ns(href) or nil
  end

  if now == pointing then return false end

  pointing = now
  return true
end

--
-- **Resized**, by the grip or by the window manager's maximise (6zz e). The
-- kit has already made a region the new size and handed it over; this is
-- the browser's half: its geometry from the new size, the toolbar laid out
-- along the new width, the status line cut to it, and the page reflowed.
--
--
-- **The page's room changed** - the window resized, the favorites bar come
-- or gone, the sidebar opened or closed - so everything placed from the
-- size is placed again and the page laid out at its new width.
--
local function room_changed(w, h)
  geometry(w or W, h or H)
  sink.w, sink.h = W, H
  lay_out_header()
  lay_out_bar()
  lay_out_side()
  status_for = nil
  reflow()

  if here == prefs.PAGE then fill_settings() end
end

win.on_resize = function(_, w, h)
  room_changed(w, h)
end

--------------------------------------------------------------------------
-- **The tabs, kept and shown.**
--
-- `stow` writes the shown tab's page out of the locals into its table and
-- lets its band go; `unstow` reads one back. Every name either touches is
-- one the page above is drawn, scrolled and clicked from, which is why
-- they are listed here and nowhere else: a new piece of a page's state is
-- a line in each.
--------------------------------------------------------------------------

local function stow(t)
  if paper then paper:free() end

  paper, paper_h = nil, 0

  t.band_top, t.content_h, t.top = band_top, content_h, top
  t.pictures, t.kept_bytes = pictures, kept_bytes
  t.ns_doc, t.ns_tried, t.ns_svgs, t.ns_raster = ns_doc, ns_tried, ns_svgs, ns_raster
  t.doc, t.said, t.timing, t.came, t.anyway = doc, said, timing, came, anyway
  t.address, t.here, t.back, t.forward = address, here, back, forward
  t.laid_for, t.laid_ms, t.painted_ms, t.pictures_ms = laid_for, laid_ms,
                                                       painted_ms, pictures_ms
end

local function unstow(t)
  band_top, content_h, top = t.band_top, t.content_h, t.top
  pictures, kept_bytes = t.pictures, t.kept_bytes
  ns_doc, ns_tried, ns_svgs, ns_raster = t.ns_doc, t.ns_tried, t.ns_svgs, t.ns_raster
  doc, said, timing, came, anyway = t.doc, t.said, t.timing, t.came, t.anyway
  address, here, back, forward = t.address, t.here, t.back, t.forward
  laid_for, laid_ms, painted_ms, pictures_ms = t.laid_for, t.laid_ms,
                                               t.painted_ms, t.pictures_ms

  -- What belongs to the moment rather than the page.
  pointing, band_ms, dragging, status_for = nil, nil, nil, nil
end

-- A tab with nothing in it yet.
local function blank_tab()
  local t = { band_top = 0, content_h = 0, top = 0, pictures = {}, kept_bytes = 0,
              ns_doc = false, ns_tried = {}, ns_svgs = {}, ns_raster = {},
              said = "", timing = "", anyway = {},
              address = { text = "", caret = 0, from = 0 },
              back = {}, forward = {}, laid_ms = 0, painted_ms = 0,
              pictures_ms = 0 }

  t.view = tab_view(t)
  return t
end

local function say_tab(how)
  print(("browser: tab %d of %d, %s, showing %s"):format(index_of(current), #tabs,
        how, tostring(here)))
end

show_tab = function(t)
  if t == current then return end

  if current then stow(current) end

  current = t
  unstow(t)
  win:retitle(t.title and ("Browser - " .. t.title) or "Browser")

  -- Its band painted again from what it kept - laid out again first if
  -- the window is not the size it was laid out at.
  if doc then
    if laid_for == PAGE_W * 65536 + VIEW_H + setting.zoom * 4294967296 then
      make_band()
    else
      reflow()
    end
  end

  if here == prefs.PAGE then fill_settings() end

  if here == NEWTAB then field:take_keys() else win:focus_on(sink) end

  say_tab("shown")

  -- One the browser opened on and has not shown yet: its page now.
  if t.pending then
    local a = t.pending

    t.pending = nil
    visit(a)
  end
end

--
-- A new tab, beside the shown one, as every browser opens one: on a new
-- tab's page with the address field waiting, or on `where`.
--
new_tab = function(where)
  local t = blank_tab()
  local at = current and index_of(current) or 0

  table.insert(tabs, at + 1, t)
  strip:add(t.view)

  if current then stow(current) end

  current = t
  unstow(t)
  say_tab("new")
  visit(where or NEWTAB)

  if not where then field:take_keys() end
end

--
-- A tab closed: its document and its pictures let go, its page kept among
-- what was open lately, and the one after it shown - or the one before, at
-- the end. The last one closes the window, as in every browser.
--
local function forget_tab(t)
  if t.doc then t.doc:close() end

  for _, p in ipairs(t.pictures or {}) do
    if p.kept then p.kept:free() end
  end

  t.doc, t.pictures, t.ns_raster, t.ns_svgs = nil, nil, nil, nil
end

close_tab = function(t)
  local i = t and index_of(t)

  if not i then return end

  if #tabs == 1 then
    print("browser: the last tab closed, and the window with it")
    win:close()
    return
  end

  if t == current then
    stow(t)
    current = nil
  end

  table.remove(tabs, i)
  strip:remove(t.view)
  forget_tab(t)

  if current == nil then show_tab(tabs[math.min(i, #tabs)]) end

  keep_tabs()
  print(("browser: a tab closed, %d left"):format(#tabs))
end

-- The next tab or the one before, round the ends; and one by its place,
-- 9 being the last whatever there are, as other browsers have it.
local function step_tab(by)
  show_tab(tabs[(index_of(current) - 1 + by) % #tabs + 1])
end

local function pick_tab(n)
  local t = (n == 9) and tabs[#tabs] or tabs[n]

  if t then show_tab(t) end
end

new_b.on_click = function() new_tab() end

--------------------------------------------------------------------------
-- **Favorites, read and kept** (`roadmap.md` 6zz d3).
--
-- Read when the window opens, after the star makes or unmakes one, when
-- the sidebar opens, and every few seconds while the window runs - Tracker
-- renames, moves and deletes them, and nothing tells a window that a
-- folder changed. What was read is compared with what was shown, so a
-- read that finds nothing new draws nothing.
--------------------------------------------------------------------------

local read_favorites

do
  local fav_read_at, fav_said = 0, nil

  function read_favorites()
    fav_read_at = sys.ticks()

    local all, list = favorites.all(), favorites.read()
    local parts, addresses = {}, {}

    for _, e in ipairs(list) do
      parts[#parts + 1] = e.name .. "\1" .. (e.address or "/")
    end

    for a in pairs(all) do addresses[#addresses + 1] = a end

    table.sort(addresses)

    local said = table.concat(parts, "\2") .. "\3" .. table.concat(addresses, "\2")

    if said == fav_said then return false end

    local had_bar = #bar_list > 0

    fav_said, kept, bar_list = said, all, list

    -- The bar coming or going moves the page; otherwise only the bar changes.
    if had_bar ~= (#bar_list > 0) then room_changed() else lay_out_bar() end

    if side_open and side_seg.on == 1 then fill_side() end

    return true
  end

  --
  -- And a refresh that is due, on the page that asked for it (`load`).
  --
  win.on_frame = function()
    local r = current and current.refresh

    if r and sys.ticks() >= r.at then
      current.refresh = nil

      if here == r.from then
        print(("browser: refreshed to %s"):format(r.to))
        visit(r.to)
        return true
      end
    end

    if sys.ticks() - fav_read_at < HZ * 3 then return false end

    return read_favorites()
  end
end

--
-- **The star, pressed** - or Super D, or the menu: the page on screen a
-- favorite, at the end of the folder, or not one any more, wherever it was
-- kept.
--
toggle_favorite = function()
  if here == nil or here == NEWTAB then return end

  if kept[here] then
    local n = favorites.remove(here)

    say("not a favorite any more")
    print(("browser: %s is not a favorite, %d removed"):format(here, n))
  else
    local path, why = favorites.add(here, current.title)

    if path then
      say("a favorite, in " .. path)
      print(("browser: a favorite, %s, of %s"):format(path, here))
    else
      say("it could not be kept: " .. tostring(why))
    end
  end

  read_favorites()
end

toggle_side = function()
  side_open = not side_open

  if side_open then
    read_favorites()
    fill_side()
  end

  room_changed()

  local row = ui.theme.metrics.row

  local fav_w = gfx.measure("Favorites") + 22

  print(("browser: the sidebar %s, its first row at %d,%d, History at %d,%d")
        :format(side_open and "open" or "closed", side_tree.x + 40,
                VIEW_Y + side_tree.y + 2 + row // 2,
                side_seg.x + 1 + fav_w + (gfx.measure("History") + 22) // 2,
                VIEW_Y + side_seg.y + side_seg.h // 2))
end

open_address = function(address)
  win:focus_on(sink)
  visit(address)
end

do
  -- A folder's favorites as a menu, a folder in it as a menu of its own.
  local function menu_of(dir)
    local items = {}

    for _, e in ipairs(favorites.read(dir)) do
      if e.folder then
        items[#items + 1] = { text = e.name, submenu = menu_of(e.path) }
      else
        items[#items + 1] = { text = e.name,
                              on_choose = function() open_address(e.address) end }
      end
    end

    if #items == 0 then items[1] = { text = "Nothing in it" } end

    return items
  end

  open_entry = function(e, v)
    if e.folder then
      win:open_menu((win.origin_x or 0) + v.x,
                    (win.origin_y or 0) + TABS + HEAD + FAVS - 2, menu_of(e.path))
    else
      open_address(e.address)
    end
  end

  bar_more.on_click = function()
    local items = {}

    for _, e in ipairs(bar_rest) do
      if e.folder then
        items[#items + 1] = { text = e.name, submenu = menu_of(e.path) }
      else
        items[#items + 1] = { text = e.name,
                              on_choose = function() open_address(e.address) end }
      end
    end

    win:open_menu((win.origin_x or 0) + math.max(0, W - 220),
                  (win.origin_y or 0) + TABS + HEAD + FAVS - 2, items)
  end
end

star_b.on_click = then_page(function() toggle_favorite() end)
hb.side.on_click = then_page(function() toggle_side() end)

--------------------------------------------------------------------------
-- **Settings' rows, and what each one does** (`roadmap.md` 6zz d5).
--
-- Built again whenever the page is shown or its room changes - the rows are
-- what the browser knows, so the page is made from them rather than edited.
-- Each change is written to `/Home/Preferences/browser/settings` as it is
-- made, and takes effect at once where it can: the favorites bar comes or
-- goes, the costs leave the status line, history is let go. Text size is
-- the size pages opened after it start from.
--------------------------------------------------------------------------

-- The days history is kept, as Settings says, and the older let go.
local function prune_history()
  local now = clock.now()

  if now then
    return history.prune(history.day_of(clock.at(now.epoch
                         - (setting.history_days - 1) * 86400)))
  end

  return 0
end

do
  -- Where a view is in the window, walking up through what holds it.
  local function in_window(v)
    local x, y, at = v.x, v.y, v.parent

    while at do x, y, at = x + (at.x or 0), y + (at.y or 0), at.parent end

    return x, y
  end

  -- A menu under a button, on the screen (`open_menu`'s coordinates).
  local function menu_under(v, items)
    local x, y = in_window(v)

    win:open_menu((win.origin_x or 0) + x, (win.origin_y or 0) + y + v.h, items)
  end

  local function save(key, value, after)
    setting[key] = value
    prefs.write(setting)
    print(("browser: set %s %s"):format(key, tostring(value)))

    if after then after(value) end
  end

  local function switch(key, after)
    return ui.switch{ on = setting[key],
                      on_change = function(_, on) save(key, on, after) end }
  end

  local function choice(key, after)
    return ui.dropdown{ choices = prefs.CHOICES[key], value = setting[key],
                        on_change = function(_, v) save(key, v, after) end }
  end

  local function button(text, act)
    local b = ui.button{ text = text }

    if b.fit then b:fit() end

    b.on_click = act
    return b
  end

  function fill_settings()
    local page = settings_page

    page.x, page.y, page.w, page.h = VIEW_X, VIEW_Y, W - VIEW_X, VIEW_H
    page.children = {}

    local home = ui.field{ text = setting.home, w = 220 }

    home.on_change = function(_, text)
      if text ~= "" then
        setting.home = text
        prefs.write(setting)
      end
    end

    local clear = button("Clear...", function(b)
      menu_under(b, { { text = "Clear every page visited, from every tab",
                        on_choose = function()
                          local days = #history.days()

                          history.clear()
                          say("history cleared")
                          print(("browser: history cleared, %d days"):format(days))

                          if side_open and side_seg.on == 2 then fill_side() end
                        end } })
    end)

    -- Mozilla's roots, as the build read their names, and the date.
    local roots = sys.asset("ca/roots.txt") or ""
    local count, as_of = 0, nil

    for line in roots:gmatch("[^\n]+") do
      if not as_of and line:match("^as of ") then as_of = line:sub(7) else count = count + 1 end
    end

    -- "Fri Sep 25 03:12:01 2026 GMT", as curl writes it, as a person would.
    local mon, day, year = tostring(as_of):match("^%a+ (%a+) +(%d+) [%d:]+ (%d+)")

    for i, m in ipairs(clock.MONTHS) do
      if m == mon then as_of = ("%d %s %s"):format(tonumber(day), clock.FULL_MONTHS[i], year) end
    end

    local own = fs.list(http.AUTHORITIES) or {}
    local kept = fs.list(CACHE.dir) or {}

    local open_own = button("Open", function()
      if not fs.getattr(http.AUTHORITIES) then
        fs.send(http.AUTHORITIES, { type = "mkdir" })
      end

      fs.send("/Running/wm", { type = "launch", program = "tracker",
                               args = http.AUTHORITIES })
    end)

    local empty = button("Empty", function()
      CACHE:empty()
      say("the cache is empty")
      fill_settings()
    end)

    local zoom = choice("zoom", function(v)
      if web and web.zoom then web.zoom(v) end
    end)

    local costs = switch("costs", function() status_for = nil end)
    local images = switch("images")
    local opens = choice("opens")

    -- What it tells sites it is, chosen, and the words themselves - which
    -- may be edited, making them its own (`browserprefs.agent`).
    local version = (sys.build and sys.build() or {}).version
    local agent = choice("agent", function()
      AGENT = prefs.agent(setting, version)
      fill_settings()
    end)
    local words = ui.field{ text = AGENT }

    words.fill = true
    words.on_change = function(_, text)
      if text ~= "" and not text:find("%c") and text ~= AGENT then
        setting.agent, setting.agent_words = "own", text
        prefs.write(setting)
        AGENT = prefs.agent(setting, version)
        agent.value = "own"
      end
    end

    local groups = {
      { name = "Starting", rows = {
          { label = "Home page", control = home },
          { label = "When the browser opens", control = opens },
          { label = "Show the favorites bar",
            control = switch("bar", function() room_changed() end) } } },
      { name = "Pages", rows = {
          { label = "Zoom", note = "All of the page: its words, pictures and boxes",
            control = zoom },
          { label = "Load images", control = images },
          { label = "What each page cost",
            note = "Fetch, parse, layout, paint, in the status line",
            control = costs },
          { label = "Tell sites it is",
            note = "Some send a plainer browser a simpler page", control = agent },
          { label = "In words", control = words } } },
      { name = "Searching", rows = {
          { label = "Search with",
            note = "Words typed that are not an address",
            control = choice("search") } } },
      { name = "History", rows = {
          { label = "Keep history for",
            control = choice("history_days", function() prune_history() end) },
          { label = "Clear history", note = "Every page visited, from every tab",
            control = clear } } },
      { name = "Security", rows = {
          { label = "Trusted authorities",
            note = ("Mozilla's %d, as of %s"):format(count, as_of or "the build"),
            control = button("Show", function() new_tab(prefs.ROOTS) end) },
          { label = "Added here",
            note = #own > 0 and table.concat(own, ", ")
                   or "None - a certificate in DER put in /Home/Preferences/Authorities is trusted too",
            control = open_own },
          { label = "A page with a bad certificate",
            note = "Refused, with the reason and an Open anyway for that tab" } } },
      { name = "Cache", rows = {
          { label = "Kept to be used again",
            note = ("%d replies, %d KB, in /Home/Cache/Browser")
                   :format(#kept, (CACHE:count() + 1023) // 1024),
            control = empty } } },
    }

    -- Two columns when there is room for two, as the drawing has them.
    local two = page.w >= 860
    local columns = two and { { groups[1], groups[2] }, { groups[3], groups[4],
                                groups[5], groups[6] } } or { groups }
    local each = page.w // #columns

    page.tall = 0

    for i, list in ipairs(columns) do
      local c = ui.cards{ x = (i - 1) * each, y = 0, w = each, h = 4000,
                          groups = list, width = 440 }

      c.h = c.content_h + 16
      c.top = 0
      page:add(c)
      page.tall = math.max(page.tall, c.h)
    end

    page.scroll = math.max(0, math.min(page.scroll, page.tall - page.h))

    for _, c in ipairs(page.children) do c.y = c.top - page.scroll end

    -- Said, for whoever drives the page from outside: where its controls
    -- are in the window.
    local function at(v)
      local x, y = in_window(v)

      return ("%d,%d"):format(x + v.w // 2, y + v.h // 2)
    end

    print(("browser: settings, %d columns, %d tall, costs %s, zoom %s, opens %s, "
           .. "clear %s, empty %s, images %s, agent %s")
          :format(#columns, page.tall, at(costs), at(zoom), at(opens), at(clear),
                  at(empty), at(images), at(agent)))
  end
end

-- Settings, in the tab that shows it or a new one.
--
-- **Zoom**, from the menu, Settings or the keys: written, handed to the
-- kit, and the page on screen laid out again at it - the others when they
-- are shown (`laid_for`).
--
set_zoom = function(pct)
  if pct == setting.zoom then return end

  setting.zoom = pct
  prefs.write(setting)

  if web and web.zoom then web.zoom(pct) end

  reflow()
  say(("Zoom %d%%"):format(pct))
  print(("browser: zoom %d"):format(pct))
end

open_settings = function()
  for _, t in ipairs(tabs) do
    if tab_at(t) == prefs.PAGE then
      show_tab(t)
      return
    end
  end

  new_tab(prefs.PAGE)
end

--
-- **The browser's keys, with Super** (`docs/browser.html`): the window
-- manager keeps the ones it has a binding for - Super Tab goes round the
-- windows, which is why the tabs go round on Super Shift ] and [, as
-- Safari's and Chrome's do on a Mac - and hands this window the rest,
-- whichever of its parts has the keyboard.
--
win.on_key = function(_, c)
  local k, mods = ui.keyparts(c)

  -- Control-L, whichever part has the keyboard - Settings' switches as
  -- well as the page - since the address is the window's, not the page's.
  if c == 12 then
    page_blur()
    field:take_keys()
    return true
  end

  if mods ~= ui.SUPER then return false end

  if k == 116 or k == 84 then new_tab()                         -- T
  elseif k == 119 or k == 87 then close_tab(current)            -- W
  elseif k == 125 then step_tab(1)                              -- Shift ]
  elseif k == 123 then step_tab(-1)                             -- Shift [
  elseif k >= 49 and k <= 57 then pick_tab(k - 48)              -- 1 to 9
  elseif k == 108 or k == 76 then                               -- L
    page_blur()
    field:take_keys()
  elseif k == 114 or k == 82 then reload()                      -- R
  elseif k == 91 then go_back()                                 -- [
  elseif k == 93 then go_forward()                              -- ]
  elseif k == 100 or k == 68 then toggle_favorite()             -- D
  elseif k == 121 or k == 89 then toggle_side()                 -- Y
  elseif k == 44 then open_settings()                           -- ,
  elseif k == 61 or k == 43 or k == 45 then                     -- = + -
    local list, at = prefs.CHOICES.zoom, 1

    for i, c in ipairs(list) do
      if c[1] == setting.zoom then at = i end
    end

    set_zoom(list[math.max(1, math.min(#list, at + (k == 45 and -1 or 1)))][1])
  elseif k == 48 then set_zoom(100)                             -- 0
  else return false end

  return true
end

--------------------------------------------------------------------------

read_favorites()


geometry(W, H)
lay_out_header()
lay_out_bar()
lay_out_side()

do
  --
  -- An address on the command line, which `wm browser:10.0.2.2:8000/` passes
  -- through, in the first tab.
  --
  -- With no address, the page inside the image. A browser that opened on
  -- nothing could not be tried without a server running somewhere, which is
  -- a thing an operating system has no business asking for.
  --
  local start = tostring(args or ""):match("^%s*(.-)%s*$")

  -- The days of history older than Settings keeps, let go as the window opens.
  prune_history()

  --
  -- **Or the tabs it had**, when Settings says it opens on them and there
  -- are any (d5): every one made, and only the one that was shown loaded -
  -- the others load when they are shown, so opening is one page's wait
  -- rather than all of theirs.
  --
  local had, had_shown = prefs.tabs()

  if start == "" and setting.opens == "tabs" and #had > 0 then
    for _, a in ipairs(had) do
      local t = blank_tab()

      t.pending = a
      tabs[#tabs + 1] = t
      strip:add(t.view)
    end

    print(("browser: opened on the tabs it had, %d"):format(#had))
    show_tab(tabs[had_shown])
  else
    new_tab(start ~= "" and start or setting.home)
  end
end

win:run()
