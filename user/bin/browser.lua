-- kosmos: application
-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
--
-- A web browser.
--
--   wm browser
--   wm browser:10.0.2.2:8000/
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
-- So this process owns its pixels. `web_paint.c` lays the document out and
-- paints it into a surface as tall as the whole page, once; a frame is one
-- blit of the visible band out of it. Scrolling re-runs nothing - no
-- layout, no line breaking, no glyph rasterised twice - which is the same
-- shape `pdfview` has and for the same reason.
--
-- What it costs is that there are no widgets. A direct window owns every
-- pixel, so a button would have nothing to draw into: the chrome here is
-- rectangles this file knows the position of, and a click is a comparison
-- against them. That is the trade the mode makes, and it is the reason the
-- chrome is deliberately small - back, forward, reload, home, an address
-- and a status line, which is NetSurf's own row and nothing more.
--
-- **No DNS**, so an address is four numbers. `ping` and `fetch` say the
-- same thing for the same reason: nothing on this machine resolves a name.

local ui    = use("/lib/ui.lua")
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
-- How tall a page may be laid out, in bytes rather than in pixels.
--
-- A process may map 48 MB and this one is already holding two window
-- buffers, a Lua heap and a DOM. Sixteen megabytes of paper is about seven
-- screenfuls at this width, and a document taller than that is cut off and
-- *said to be* - which is the honest end of a fixed budget, and better than
-- a page that renders for a while and then fails to exist.
--
local PAPER_BYTES = 16 * 1024 * 1024

local PAPER = 0xffffffff             -- what `web_paint.c` fills a page with

--
-- The kit is only in an image built with `WEB=1`, so its absence is an
-- ordinary state rather than a failure: the window opens and says which
-- build it is running on. An application that raised here would be a broken
-- entry in the Deskbar of every ordinary image.
--
local have, web = pcall(use, "/kits/web")

if not have or type(web) ~= "table" then web = nil end

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
-- An address, which is four numbers and a path.
--------------------------------------------------------------------------

local function split(text)
  local rest = tostring(text or ""):gsub("^%s+", ""):gsub("%s+$", "")
  local hostport, path = rest:match("^([^/]+)(/.*)$")

  hostport = hostport or rest
  path = path or "/"

  local host, port = hostport:match("^([^:]+):(%d+)$")

  host = host or hostport
  port = tonumber(port) or 80

  local a, b, c, d = tostring(host):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d), port, path
end

--
-- A link's address, against the page it was found on.
--
-- Enough of RFC 3986 to follow a link and no more: a scheme this browser
-- does not speak is refused by name rather than attempted, an absolute path
-- keeps the host, and a relative one is taken from the directory the page
-- came from. `..` is not collapsed, which a real resolver does and which no
-- page here has needed yet.
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
    if scheme:lower() ~= "http" then
      return nil, ("this browser speaks http, not %s"):format(scheme)
    end

    href = href:gsub("^%a[%w+.%-]*:", "")
  end

  if href:sub(1, 2) == "//" then return href:sub(3) end

  local host = base:match("^([^/]+)") or base

  if href:sub(1, 1) == "/" then return host .. href end

  return (base:match("^(.*/)") or (host .. "/")) .. href
end

--------------------------------------------------------------------------
-- State.
--------------------------------------------------------------------------

local paper                  -- the page, laid out once and scrolled by blit
local paper_h  = 0           -- how tall that surface is
local content_h = 0          -- how tall the document turned out to be
local top      = 0           -- the pixel of it at the top of the view

--
-- The document, kept rather than closed the moment it is painted.
--
-- The boxes live on it - `link_at` asks the layout which word is under a
-- point - so closing it would leave a page you can read and cannot click.
-- One document at a time: the previous one is closed when the next arrives,
-- which is what bounds this rather than hoping nobody opens many pages.
--
local doc

local said = "an address is four numbers - there is no resolver yet"

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

local HZ = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

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
-- And what a repaint *allocates*, in hundredths of a kilobyte.
--
-- `CLAUDE.md` records why this is measured next to the time rather than
-- instead of it: the desktop's frame profile found the language question
-- worth about a ninth of a pass and the allocation question worth four
-- times the worst-case collector pause. A process on a deadline is judged
-- by `gc_pause_max`, and the collector runs when it chooses.
--
local frame_kb = 0

local address = { text = "10.0.2.2:8000/", caret = 14, from = 0,
                  focus = false }
local here                                          -- what is on screen
local back, forward = {}, {}

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
  return math.max(0, math.min(content_h, paper_h) - VIEW_H)
end

local function thumb()
  local track = VIEW_H - SBAR * 2
  local shown = math.min(content_h, paper_h)
  local last  = reach()

  if track < 8 or shown <= VIEW_H then
    return VIEW_Y + SBAR, track           -- nothing to scroll: a full thumb
  end

  local h = math.max(20, (track * VIEW_H) // shown)
  local y = VIEW_Y + SBAR + ((track - h) * top) // last

  return y, h
end

local function draw_scrollbar(s)
  local x = W - SBAR
  local ty, th = thumb()

  s:fill(x, VIEW_Y, SBAR, VIEW_H, theme.sunken)

  -- An arrow button at each end, which is where the eye looks for one.
  for _, a in ipairs { { y = VIEW_Y, dir = "up" },
                       { y = VIEW_Y + VIEW_H - SBAR, dir = "down" } } do
    s:fill(x, a.y, SBAR, SBAR, theme.raised)
    bevel(s, x, a.y, SBAR, SBAR, false)
    arrow(s, x + SBAR // 2, a.y + SBAR // 2 + (a.dir == "up" and -2 or 2),
          4, a.dir, theme.text)
  end

  s:fill(x, ty, SBAR, th, theme.raised)
  bevel(s, x, ty, SBAR, th, false)
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

  s:text(URL.x + 5, ty, shown, theme.text)

  --
  -- The caret. Drawn only when the field has the keys, because a caret in a
  -- field that is not listening is the thing that makes a person type into
  -- the wrong place.
  --
  if address.focus then
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
    local band = math.min(VIEW_H, paper_h - top)

    if band > 0 then
      s:blit(paper, 0, top, PAGE_W, band, PAD, VIEW_Y)
    end
  else
    local lines = web
                  and { "Nothing loaded.",
                        "",
                        "Type an address and press Return, or start with one:",
                        "",
                        "    wm browser:10.0.2.2:8000/" }
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

  s:text(8, sy, said, theme.text_dim)

  if timing ~= "" then
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
-- Measured first, into nothing, and then painted into a surface the right
-- height. The other order - guess, render, discover it did not fit, render
-- again - costs a second painting pass, and a painting pass is thousands of
-- glyphs. A measuring pass breaks the same lines without drawing them.
--------------------------------------------------------------------------

local laid_ms, painted_ms = 0, 0

local function lay_out(doc)
  if paper then
    paper:free()
    paper = nil
  end

  paper_h, content_h, top = 0, 0, 0

  --
  -- Two calls, and the split is the measurement: the first lays the page
  -- out and keeps the boxes, the second paints them. Timing them together
  -- would answer "the page took 90 ms" and leave the only useful question -
  -- *which half* - unanswered.
  --
  local t0 = sys.ticks()
  local wanted = doc:render(nil, PAGE_W) + 16

  laid_ms = since(t0)

  local room = PAPER_BYTES // (PAGE_W * 4)
  local tall = math.max(VIEW_H, math.min(wanted, room))

  local made = pcall(function()
    paper = gfx.surface{ w = PAGE_W, h = tall }
  end)

  if not made or not paper then
    paper = nil
    return nil, ("no memory for a %dx%d page"):format(PAGE_W, tall)
  end

  paper_h = tall

  local t1 = sys.ticks()

  content_h = doc:render(paper, PAGE_W, tall)
  painted_ms = since(t1)

  return content_h
end

--------------------------------------------------------------------------
-- One load: connect, ask, read until the far end hangs up, parse, lay out.
--
-- The read loop is `fetch`'s, including the read *after* the loop: a close
-- and the last bytes can arrive in the same segment, and stopping at
-- `closed` would lose them.
--------------------------------------------------------------------------

local function load(text)
  if web == nil then
    say("this image has no web kit - build it with `make WEB=1`")
    return false
  end

  local where, port, path = split(text)

  if not where then
    say("that is not an address - four numbers, no names, there is no DNS")
    return false
  end

  here = text
  address.text = text
  address.caret = #text

  say("connecting to " .. text .. " ...")

  local fetch_from = sys.ticks()
  local conn, why = fs.connect("/net", where, port)

  if not conn then
    local because = ({ [4] = "no route to it",
                       [7] = "it refused the connection",
                       [9] = "it did not answer",
                       [5] = "too many connections" })[why]

    say(because or ("could not connect: " .. tostring(why)))
    return false
  end

  conn:write(("GET %s HTTP/1.0\r\nHost: kosmos\r\nConnection: close\r\n\r\n")
             :format(path))

  local parts, total = {}, 0

  for _ = 1, 400 do
    local piece = conn:read()

    if piece then
      parts[#parts + 1] = piece
      total = total + #piece
    end

    if conn:closed() then break end

    conn:wait(25)
  end

  local last = conn:read()

  if last then
    parts[#parts + 1] = last
    total = total + #last
  end

  conn:close()

  if total == 0 then
    say("nothing came back")
    return false
  end

  local fetched_ms = since(fetch_from)
  local reply = table.concat(parts)
  local body = reply:match("\r\n\r\n(.*)$") or reply

  say(("parsing %d bytes..."):format(#body))

  local parse_from = sys.ticks()
  local fresh, bad = web.parse(body)
  local parsed_ms = since(parse_from)

  if not fresh then
    say("fetched " .. total .. " bytes, but it did not parse: " .. tostring(bad))
    return false
  end

  -- The one before it, and only once this one exists: a page that fails to
  -- parse should leave what is on screen alone rather than blank it.
  if doc then doc:close() end

  doc = fresh

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

  local drawn, why_not = lay_out(doc)

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

  if content_h > paper_h then
    say(("%s - %d pixels tall, and this shows the first %d")
        :format(counts, content_h, paper_h))
  else
    say(counts)
  end

  return true
end

--------------------------------------------------------------------------
-- Where we have been.
--
-- Two stacks and one current address, which is the whole of it: going back
-- moves the current one onto the forward stack, and following a new address
-- throws that stack away. Nothing here knows about the network.
--------------------------------------------------------------------------

local function visit(text)
  if here then back[#back + 1] = here end

  forward = {}
  load(text)
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
  visit("10.0.2.2:8000/")
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

local sink = ui.view{ x = 0, y = 0, w = W, h = H }
sink.focusable = true

local function url_key(c)
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

function sink:key(c)
  if address.focus then return url_key(c) end

  local screen = VIEW_H - gfx.height() * 2

  if c == ui.UP then scroll_by(-40)
  elseif c == ui.DOWN then scroll_by(40)
  elseif c == 32 then scroll_by(screen)              -- space
  elseif c == 98 then scroll_by(-screen)             -- b
  elseif c == 103 then scroll_to(0)                  -- g
  elseif c == 71 then scroll_to(reach())             -- G
  elseif c == 12 then                                -- Control-L
    address.focus = true
    address.caret = #address.text
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

  if y < VIEW_Y + SBAR then
    scroll_by(-40)
  elseif y >= VIEW_Y + VIEW_H - SBAR then
    scroll_by(40)
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
  local track = VIEW_H - SBAR * 2
  local _, th = thumb()
  local room = track - th

  if room <= 0 then return end

  scroll_to(((y - dragging - VIEW_Y - SBAR) * reach()) // room)
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

  local href = doc:link_at(x - PAD, y - VIEW_Y + top)

  if not href then return end

  local where, why = resolve(here or address.text, href)

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
elseif start ~= "" then
  frame()
  visit(start)
else
  frame()
end

win:run()
