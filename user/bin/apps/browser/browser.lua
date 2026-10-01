-- kosmos: application
-- kosmos: icon App_NetSurf
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- kosmos: opens html
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
local theme = ui.theme

--------------------------------------------------------------------------
-- The window, and what is where in it.
--------------------------------------------------------------------------

local W, H = 900, 640

local TOOL = 34                      -- the row of buttons and the address
local STAT = 22                      -- the status line along the bottom
local SBAR = 16                      -- the scrollbar down the right
local PAD  = 8                       -- white margin either side of the page

local VIEW_Y = TOOL
local VIEW_H = H - TOOL - STAT
local VIEW_W = W - SBAR
local PAGE_W = VIEW_W - PAD * 2

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
local have, web = pcall(use, "/Kosmos/Kits/web")

if not have or type(web) ~= "table" then web = nil end

--
-- **NetSurf's layout** (`roadmap.md` 6zz j): the page laid out and drawn by
-- the engine NetSurf wrote for exactly these libraries, set up once with its
-- own default stylesheets. Without them - an image built before they were
-- carried - the pages are laid out by `web_paint.c`, as they were.
--
local UA_SHEET = sys.asset("netsurf/default.css")
local NS = web ~= nil and web.setup ~= nil and UA_SHEET ~= nil
           and web.setup(UA_SHEET, sys.asset("netsurf/quirks.css")) == true

local win, err = ui.window{
  title = "Browser", w = W, h = H, x = 80, y = 60, direct = true,
}

if not win then
  print("browser: " .. tostring(err))
  return
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

-- The other clock. Every timeout in this system is in these.
local TICK_HZ = (sys.info() or {}).tick_hz or 250

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
local frame_ms, frame_worst = 0, 0
local blit_ms, commit_ms = 0, 0

--
-- And what the key itself cost when it took the view out of the band: the
-- next band painted, and its pictures fetched if it is the first time. Not
-- part of a frame - it happens before one - and the only work a scroll
-- does that is more than a blit.
--
local band_ms

--
-- And what a repaint *allocates*, in hundredths of a kilobyte.
--
-- `CLAUDE.md` records why this is measured next to the time rather than
-- instead of it: the desktop's frame profile found the language question
-- worth about a ninth of a pass and the allocation question worth four
-- times the worst-case collector pause. A process on a deadline is judged
-- by `gc_pause_max`, and the collector runs when it chooses.
--
local frame_kb = 0

local address = { text = HOME, caret = #HOME, from = 0,
                  focus = false }
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
local BAR_W = 180

--
-- **What this browser says it is**, which is what a site decides what to
-- send on. Diego, 30 September: "to browse websites we need to send user
-- agent that aligns to what our browser is capable of". The engine is
-- NetSurf's - hubbub, libcss and libdom as NetSurf 3.11 released them - so
-- its token is the true one, and the one sites already know means HTML and
-- CSS without JavaScript: the same shape as NetSurf's own `NetSurf/3.11
-- (Linux)`, with Kosmos where the system goes.
--
local AGENT = ("NetSurf/3.11 (Kosmos %s)"):format((sys.build and sys.build() or {}).version or "0")

local dragging               -- the scrollbar thumb, while it is held

--------------------------------------------------------------------------
-- The chrome, drawn.
--
-- `theme` rather than colours of its own: the shape is NetSurf's and the
-- palette is the desktop's, so a browser window does not become the one
-- thing on screen that ignores the appearance setting. The *page* is white
-- whatever the desktop is, because that is what the document asked for.
--------------------------------------------------------------------------

local function bevel(s, x, y, w, h, sunken)
  local hi = sunken and theme.edge_dark or theme.edge_light
  local lo = sunken and theme.edge_light or theme.edge_dark

  s:fill(x, y, w, 1, hi)
  s:fill(x, y, 1, h, hi)
  s:fill(x, y + h - 1, w, 1, lo)
  s:fill(x + w - 1, y, 1, h, lo)
end

--
-- A triangle, out of one-pixel rectangles.
--
-- There is no line and no polygon on a surface - `fill`, `blit` and `text`
-- is the whole of it - and an arrow drawn as text would need a glyph the
-- interface font may not have. Eleven fills is cheaper than that argument.
--
local function arrow(s, cx, cy, size, dir, colour)
  for i = 0, size do
    if dir == "left" then
      s:fill(cx + i, cy - i, 1, 2 * i + 1, colour)
    elseif dir == "right" then
      s:fill(cx - i, cy - i, 1, 2 * i + 1, colour)
    elseif dir == "up" then
      s:fill(cx - i, cy + i, 2 * i + 1, 1, colour)
    else
      s:fill(cx - i, cy - i, 2 * i + 1, 1, colour)
    end
  end
end

--------------------------------------------------------------------------
-- The toolbar's buttons.
--
-- Laid out from their labels rather than from numbers typed in, so the row
-- still fits when the desktop is set to a font that is not the one this was
-- written against. `enabled` is asked at every repaint, because whether you
-- can go back is not a fact about the button.
--------------------------------------------------------------------------

local go_back, go_forward, go_home, reload      -- filled in further down

local BUTTONS = {
  { name = "back",    arrow = "left",  wide = 30,
    enabled = function() return #back > 0 end },
  { name = "forward", arrow = "right", wide = 30,
    enabled = function() return #forward > 0 end },
  { name = "reload",  text = "Reload" },
  { name = "home",    text = "Home" },
}

local URL = {}
local GO  = { text = "Go" }

local function lay_out_toolbar()
  local x = 6

  for _, b in ipairs(BUTTONS) do
    b.x = x
    b.w = b.wide or (gfx.measure(b.text) + 20)
    b.y = 4
    b.h = TOOL - 9
    x = x + b.w + 4
  end

  GO.w = gfx.measure(GO.text) + 20
  GO.h = TOOL - 9
  GO.y = 4
  GO.x = W - 6 - GO.w

  URL.x = x + 6
  URL.y = 4
  URL.h = TOOL - 9
  URL.w = GO.x - 6 - URL.x
end

local function inside(b, x, y)
  return b.x and x >= b.x and x < b.x + b.w
         and y >= b.y and y < b.y + b.h
end

--
-- What of the address is visible, and from which byte.
--
-- A well is a fixed width and a URL is not, and `s:text` clips against the
-- *surface* rather than against anything smaller - so a long address drawn
-- whole would run straight over the Go button and out of the toolbar. The
-- field scrolls instead: enough is dropped from the front to keep the caret
-- in view, and enough from the back to stay inside the well.
--
-- Quadratic in the length of a URL and run once per repaint, which is once
-- per event rather than once per frame. Sixty characters is a few thousand
-- glyph advances and this window does not animate.
--
local function field_view()
  local room = URL.w - 10
  local from = address.from or 0

  if from > address.caret then from = address.caret end

  -- Back, when there is room again. A short address loaded after a long one
  -- would otherwise keep the old offset and hide its first characters in a
  -- well with space to spare.
  while from > 0
        and gfx.measure(address.text:sub(from, #address.text)) <= room do
    from = from - 1
  end

  while from < address.caret
        and gfx.measure(address.text:sub(from + 1, address.caret)) > room do
    from = from + 1
  end

  address.from = from

  local upto = #address.text

  while upto > from
        and gfx.measure(address.text:sub(from + 1, upto)) > room do
    upto = upto - 1
  end

  return address.text:sub(from + 1, upto), from
end

--
-- What the status line says, cut to end before the timings begin.
--
-- It ran underneath them once a page's line began with how it came -
-- "Refused: the certificate is signed by nobody this machine trusts" and
-- then the counts. Cut a character at a time, never inside one, and worked
-- out again only when the words or the timings change rather than every
-- frame of a scroll.
--
local status_for, status_cut = nil, ""

local function shorter(text)
  local n = #text

  while n > 0 and (text:byte(n) & 0xC0) == 0x80 do n = n - 1 end

  return text:sub(1, n - 1)
end

local function status_text()
  local key = said .. "\0" .. timing .. "\0" .. (loading and "bar" or "")

  if key == status_for then return status_cut end

  local right = loading and BAR_W + 16
                or (timing ~= "" and gfx.measure(timing) + 16 or 0)
  local room = W - 16 - right
  local text = said

  if gfx.measure(text) > room then
    while text ~= "" and gfx.measure(text .. "...") > room do
      text = shorter(text)
    end

    text = text .. "..."
  end

  status_for, status_cut = key, text
  return text
end

local function draw_button(s, b, label_colour)
  local ink = label_colour or theme.text

  s:fill(b.x, b.y, b.w, b.h, theme.raised)
  bevel(s, b.x, b.y, b.w, b.h, b.down)

  local shift = b.down and 1 or 0

  if b.arrow then
    arrow(s, b.x + b.w // 2 + (b.arrow == "left" and -3 or 3) + shift,
          b.y + b.h // 2 + shift, 5, b.arrow, ink)
  else
    s:text(b.x + (b.w - gfx.measure(b.text)) // 2 + shift,
           b.y + (b.h - gfx.height()) // 2 + shift, b.text, ink)
  end
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
  -- The toolbar.
  --
  s:fill(0, 0, W, TOOL, theme.window)
  s:fill(0, TOOL - 1, W, 1, theme.line)

  for _, b in ipairs(BUTTONS) do
    local on = (b.enabled == nil) or b.enabled()

    draw_button(s, b, on and theme.text or theme.text_dim)
  end

  draw_button(s, GO)

  --
  -- The address, in a well.
  --
  s:fill(URL.x, URL.y, URL.w, URL.h, theme.sunken)
  bevel(s, URL.x, URL.y, URL.w, URL.h, true)

  local ty = URL.y + (URL.h - gfx.height()) // 2
  local shown, from = field_view()

  --
  -- **All of it selected**, after Control-L: drawn in the caret's colours,
  -- as `ui.field` draws its select-all, so it is plain that what is typed
  -- next replaces it.
  --
  if address.focus and address.all and shown ~= "" then
    s:fill(URL.x + 5, ty, gfx.measure(shown), gfx.height(), theme.ring)
    s:text(URL.x + 5, ty, shown, theme.sunken)
  else
    s:text(URL.x + 5, ty, shown, theme.text)
  end

  --
  -- The caret. Drawn only when the field has the keys, because a caret in a
  -- field that is not listening is the thing that makes a person type into
  -- the wrong place.
  --
  if address.focus and not address.all then
    local at = URL.x + 5
               + gfx.measure(address.text:sub(from + 1, address.caret))

    s:fill(at, ty, 1, gfx.height(), theme.text)
  end

  --
  -- The page.
  --
  -- The chrome is measured out of the split rather than into it: it came
  -- back at 0.1 ms against a 2.8 ms blit, which is a number that answers
  -- nothing and takes room on the line from the two that do.
  local drew_chrome = sys.ticks()

  s:fill(0, VIEW_Y, VIEW_W, VIEW_H, paper and PAPER or theme.window)

  if paper then
    local rows = math.min(VIEW_H, band_top + paper_h - top)

    if rows > 0 then
      s:blit(paper, 0, top - band_top, PAGE_W, rows, PAD, VIEW_Y)
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
      s:text(PAD + 4, VIEW_Y + 16 + (i - 1) * (gfx.height() + 4), line,
             theme.text_dim)
    end
  end

  draw_scrollbar(s)

  --
  -- The status line.
  --
  s:fill(0, H - STAT, W, STAT, theme.window)
  s:fill(0, H - STAT, W, 1, theme.line)

  local sy = H - STAT + (STAT - gfx.height()) // 2

  s:text(8, sy, status_text(), theme.text_dim)

  --
  -- The bar, while a page arrives, where the last page's costs were: they
  -- belong to the page before. Filled against the length the server gave;
  -- with none, the words say how much so far and the bar stays empty.
  --
  if loading then
    local bx, by = W - 8 - BAR_W, H - STAT + (STAT - 6) // 2

    s:fill(bx, by, BAR_W, 6, theme.sunken)

    if loading.total and loading.total > 0 then
      local done = math.min(BAR_W, BAR_W * loading.got // loading.total)

      s:fill(bx, by, done, 6, theme.accent)
    end
  elseif timing ~= "" then
    s:text(W - 8 - gfx.measure(timing), sy, timing, theme.text_dim)
  end

  local drew_all = sys.ticks()

  blit_ms = ((drew_all - drew_chrome) * 10000) // HZ

  win:commit()

  commit_ms = since(drew_all)
  frame_ms = since(began)

  -- A negative delta means the collector ran inside this frame, which says
  -- nothing about what the frame allocated. Kept rather than clamped,
  -- because seeing one is itself the answer to a question.
  frame_kb = math.floor((collectgarbage("count") - held) * 100)

  if frame_ms > frame_worst then frame_worst = frame_ms end
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

--
-- An address as NetSurf takes one, and one it gives back as this browser
-- writes it. NetSurf joins links by the rules for URLs, so a file of this
-- machine's goes to it as a `file:` URL; what comes back loses `http://`,
-- which the address bar has never shown, and `file://` becomes the path.
--
local KNOWN = { http = true, https = true, asset = true, about = true,
                file = true, kosmos = true }

local function ns_address(text)
  if text == nil then return nil end
  if text:sub(1, 1) == "/" then return "file://" .. text end

  local scheme = text:match("^(%a[%w+.%-]*):")

  -- An address the bar keeps without its scheme is http's - and a host
  -- with a port looks like a scheme to the pattern, so only the ones this
  -- browser speaks count as one.
  if scheme and KNOWN[scheme:lower()] then return text end

  return "http://" .. text
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
  ns_doc, ns_tried, ns_svgs = false, {}, {}

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
<p>Open anyway loads it for this window only; it says Not secure for as
long as it is open, and nothing is remembered.</p>
</body></html>]]):format(escaped(why), escaped(text))
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

  if #wanted > 0 then
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
-- has given it one (`svgs_to_boxes`); `k` is which picture it is.
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
-- page said otherwise. Its natural size goes back unchanged, so the layout
-- does not move.
--
local function svgs_to_boxes()
  if not next(ns_svgs) then return end

  for k, o in ipairs(doc:ns_objects()) do
    local s = ns_svgs[k]

    if s and o.w > 0 and o.h > 0 and (o.w ~= s.w or o.h ~= s.h) then
      local pic = svg_surface(s.svg, o.w, o.h)

      if pic then
        local sw, sh = s.svg:size()

        doc:ns_picture(k, pic, sw, sh)
        s.w, s.h = o.w, o.h
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

    if #wanted == 0 then break end

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
      end
    end

    said = before

    if arrived == 0 then break end

    local tall = doc:ns_layout(PAGE_W, VIEW_H, ns_address(here))

    if tall then content_h = tall end

    band_top = math.max(0, math.min(band_top, content_h + 16 - paper_h))
    top = math.max(0, math.min(top, math.max(0, content_h - VIEW_H)))
    svgs_to_boxes()
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
    return ns_band_pictures()
  end

  doc:render(paper, PAGE_W, paper_h, band_top)

  return band_pictures()
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

local function load(text, post)
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
  address.text = text
  address.caret = #text
  address.from = 0

  local body, fetched_ms, how

  if text == HOME then
    say("the page inside this image")
    body, fetched_ms = START, 0

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
  local fresh, bad = web.parse(body)
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
  -- Four numbers, in the order the bytes go through them. Read as
  -- proportions rather than as speeds: this is QEMU, and `CLAUDE.md` is
  -- clear about what a number from QEMU is worth. What it is worth is
  -- knowing which of the four to work on.
  --
  timing = ("fetch %s  parse %s  layout %s  paint %s ms")
           :format(tenths(fetched_ms), tenths(parsed_ms),
                   tenths(laid_ms), tenths(painted_ms))

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
  return true
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
  load(here or address.text)
end

go_home = function()
  visit(HOME)
end

local ACTIONS = {
  back = function() go_back() end,
  forward = function() go_forward() end,
  reload = function() reload() end,
  home = function() go_home() end,
}

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

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

local function url_key(c)
  --
  -- **What Control-L selected goes when something is typed**, as in every
  -- browser: the whole address was selected, so a character replaces it and
  -- Backspace empties it. It put the caret at the end instead, and a typed
  -- address went on the end of the old one - `run_browser.py` found it,
  -- typing `http://10.0.2.2/second.html` into a bar that already held a page.
  --
  if address.all then
    address.all = false

    if c == 8 or c == 127 then
      address.text, address.caret = "", 0
      frame()
      return true
    elseif c >= 32 and c < 127 then
      address.text, address.caret = "", 0
    end
  end

  if c == 13 or c == 10 then
    address.focus = false
    visit(address.text)
  elseif c == 27 then
    address.focus = false
  elseif c == 8 or c == 127 then
    if address.caret > 0 then
      address.text = address.text:sub(1, address.caret - 1)
                     .. address.text:sub(address.caret + 1)
      address.caret = address.caret - 1
    end
  elseif c == ui.LEFT then
    address.caret = math.max(0, address.caret - 1)
  elseif c == ui.RIGHT then
    address.caret = math.min(#address.text, address.caret + 1)
  elseif c >= 32 and c < 127 then
    address.text = address.text:sub(1, address.caret) .. string.char(c)
                   .. address.text:sub(address.caret + 1)
    address.caret = address.caret + 1
  else
    return false
  end

  frame()

  return true
end

--
-- The wheel: three lines of the page a notch - forty pixels each, as an
-- arrow moves it - away from the person up (`roadmap.md` 5zv).
--
function sink:wheel(n)
  if scroll_by(-n * 3 * 40) then frame() end
  return true
end

function sink:key(c)
  if address.focus then return url_key(c) end

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
    address.focus = true
    address.caret = #address.text
    address.all = true
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
           :format(tenths(frame_ms), tenths(blit_ms), tenths(commit_ms),
                   tenths(frame_worst),
                   ("%d.%02d"):format(frame_kb // 100, frame_kb % 100))
           .. (band_ms and (", band %s ms"):format(tenths(band_ms)) or "")

  frame()

  return true
end

--
-- Where in the address a click landed.
--
-- Measured prefix by prefix rather than divided by a cell width, because
-- the interface font is whatever the desktop was set to and only two of the
-- ones in this image are fixed width. Over what is *shown* rather than over
-- the whole address, and offset by where the field is scrolled to, or a
-- click in a scrolled field would place the caret near the start of a URL
-- whose start is not on screen.
--
local function caret_from(px)
  local shown, from = field_view()
  local best = 0

  for i = 0, #shown do
    if gfx.measure(shown:sub(1, i)) <= px then best = i else break end
  end

  return from + best
end

local function toolbar_press(x, y)
  address.focus = false
  page_blur()

  for _, b in ipairs(BUTTONS) do
    if inside(b, x, y) then
      b.down = true
      return
    end
  end

  if inside(GO, x, y) then
    GO.down = true
    return
  end

  if inside(URL, x, y) then
    address.focus = true
    address.caret = caret_from(x - URL.x - 5)
  end
end

local function toolbar_release(x, y)
  for _, b in ipairs(BUTTONS) do
    if b.down then
      b.down = nil

      if inside(b, x, y) and ((b.enabled == nil) or b.enabled()) then
        ACTIONS[b.name]()
      end
    end
  end

  if GO.down then
    GO.down = nil

    if inside(GO, x, y) then visit(address.text) end
  end
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
  address.focus = false

  if not doc or not paper then return end

  -- A form's field first: a click there is the field's, and a click
  -- anywhere else takes the caret out of whichever had it.
  if ns_doc then
    local did = doc:ns_click(x - PAD, y - VIEW_Y + top)

    form_changed()

    if did then return end
  end

  local href

  if ns_doc then
    href = doc:ns_link_at(x - PAD, y - VIEW_Y + top)
  else
    href = doc:link_at(x - PAD, y - VIEW_Y + top)
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

function sink:mouse(action, x, y)
  if action == "press" then
    if y < TOOL then
      toolbar_press(x, y)
    elseif y < VIEW_Y + VIEW_H and x >= W - SBAR then
      scrollbar_press(y)
    elseif y < VIEW_Y + VIEW_H then
      page_press(x, y)
    else
      address.focus = false
    end
  elseif action == "move" then
    if dragging then scrollbar_drag(y) end
  elseif action == "release" then
    dragging = nil
    toolbar_release(x, y)
  end

  frame()

  return true
end

win:add(sink)

--------------------------------------------------------------------------

lay_out_toolbar()

--
-- An address on the command line, which `wm browser:10.0.2.2:8000/` passes
-- through. Loaded before the first pass rather than after, so the window is
-- drawn once, with the page already in it.
--
local start = tostring(args or ""):match("^%s*(.-)%s*$")

if web == nil then
  said = "no web kit in this image"
  frame()
else
  frame()

  -- With no address, the page inside the image. A browser that opened on
  -- nothing could not be tried without a server running somewhere, which is
  -- a thing an operating system has no business asking for.
  visit(start ~= "" and start or HOME)
end

win:run()
