-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki.
-- kosmos: application
-- kosmos: icon App_NetSurf
-- kosmos: name JetSki
-- kosmos: section applications
-- kosmos: needs network
-- kosmos: about WebKit on Kosmos: the web as Safari draws it
-- kosmos: image jetski.elf
--
--   wm jetski                              the start page
--   wm jetski:https://en.wikipedia.org/    that page
--
--   Control-L    type an address
--   Escape       the address as it was
--
-- The page takes the pointer, the wheel and keys once it is clicked, and
-- after an address is typed.
--
-- **The browser, W7c's first slice** (`docs/w7.md`): WebKit's three
-- processes - this image as the UI process, and again as each page's web
-- process and as the network process - and this file, the window around
-- them. An installed application (`docs/elf.md` step 5): this file and
-- `jetski.elf` in `/Home/Apps/JetSki`; the system's image carries none of
-- it.
--
-- **A window that draws its own pixels** (`gfx.md` 19.4), as the NetSurf
-- browser's is: the kit's widgets - back, forward, reload and the address
-- field - painted into the surface with `ui.paint_view`, and the page
-- below them painted by WebKit (`jetski.paint`) straight into the same
-- surface, so a page's pixels never pass through Lua.
--
-- **The loop is the window's**, and WebKit's runs inside it a slice at a
-- time (`jetski.pump`): a few milliseconds on each pass, ended early when
-- the page has a picture, which is painted then. So the window still
-- answers the desktop between slices - and waits a slice at most for it,
-- which is the first thing to take away (`roadmap.md`, W7c): one wait on
-- both the window manager and WebKit's processes.

local ui    = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local prefs = use("/Kosmos/Libraries/browserprefs.lua")
local theme = ui.theme

local have, web = pcall(use, "jetski.elf")

if not have or type(web) ~= "table" then
  print("jetski: " .. tostring(web))
  return
end

local HOME = "https://en.wikipedia.org/wiki/BeOS"

-- The window's size, which a person changes with its grip: every other
-- size here is worked out from these two in `layout`.
local W, H = 1200, 800
local HEAD = ui.layout.head          -- back, forward, reload and the address
local STAT = 26                      -- what the page is doing, along the bottom
local VIEW_Y = HEAD
local VIEW_H = H - HEAD - STAT

local X, Y = 70, 50
local win, why = ui.window{ title = "JetSki", w = W, h = H, x = X, y = Y, direct = true,
                            resizable = true }

if not win then
  print("jetski: " .. tostring(why))
  return
end

if not win:surface() then
  print("jetski: this window did not get a shared surface")
  return
end

local made, refused = web.open(W, VIEW_H)

if not made then
  print("jetski: " .. tostring(refused))
  win:close()
  return
end

-- What a page's `window.screenX` and `outerWidth` answer: the window, frame
-- and all. Left at nought, a site's robot check reads it as no window.
web.frame(X, Y, W, H)

--------------------------------------------------------------------------
-- The header: the kit's widgets, in this window's own pixels.
--

local header = ui.view{ x = 0, y = 0, w = W, h = HEAD }

function header:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, self.h - 1, self.w, 1, theme.line)
end

local back    = ui.iconbutton{ icon = "back" }
local forward = ui.iconbutton{ icon = "forward" }
local reload  = ui.iconbutton{ icon = "reload" }
local field   = ui.field{ text = HOME, hint = "Search, or type an address" }

do
  local gap, edge = ui.layout.head_gap, ui.layout.head_edge
  local x = edge

  for _, b in ipairs({ back, forward, reload }) do
    b.x, b.y = x, (HEAD - 1 - b.h) // 2
    x = x + b.w + gap
    header:add(b)
  end

  field.x = x + 4
  field.y = (HEAD - 1 - field.h) // 2
  header:add(field)
end

win:add(header)

--------------------------------------------------------------------------
-- **The page's place**, a view the kit routes the pointer, the wheel and
-- keys to like any other (W7c) - but drawn by WebKit, so it draws nothing
-- itself. Positions are handed on in the page's own pixels.
--

local view = ui.view{ x = 0, y = VIEW_Y, w = W, h = VIEW_H }

-- **Every size from the window's**: the header and the address across it,
-- the page below it, and the status line along the bottom.
local function layout()
  VIEW_H = math.max(1, H - HEAD - STAT)
  header.w = W
  field.w = W - ui.layout.head_edge - field.x
  view.w, view.h = W, VIEW_H
end

layout()

view.focusable = true
view.takes_tab = true                -- Tab is the page's, between its fields

function view:draw() end

function view:mouse(action, x, y)
  if action == "press" then
    web.focus(true)
    web.mouse("press", "left", x, y)
  elseif action == "release" then
    web.mouse("release", "left", x, y)
  else
    web.mouse("move", nil, x, y)
  end

  return false                       -- WebKit says when there is a picture
end

function view:wheel(n, x, y)
  web.wheel(x, y, n)
  return true
end

function view:key(c)
  if c == 12 then return false end   -- Control-L is the window's
  return web.key(c)
end

win:add(view)

-- The pointer passing over, nothing held: links light up, and say so.
use("/Kosmos/Libraries/wmproto.lua").track(win.handle, true)

win.on_hover = function(_, x, y)
  if y >= VIEW_Y and y < VIEW_Y + VIEW_H then
    web.mouse("move", nil, x, y - VIEW_Y)
  end

  return false
end

--------------------------------------------------------------------------
-- The page, as WebKit says it is.
--

local st = {}                        -- `jetski.pump` fills it, every pass
local page = { title = "", address = HOME, said = "", loading = false,
               progress = 0, can_back = false, can_forward = false }

local function say(text)
  page.said = text
  print("jetski: " .. text)
end

-- An address as WebKit wants one: a scheme always, `https` when none was
-- typed; words are a search, as the NetSurf browser's field has them.
local function visit(text)
  local where = prefs.destination(text, "duckduckgo")

  if not where then return end

  if not where:match("^%a[%w+.%-]*:") then where = "https://" .. where end

  page.address = where
  field.text = where
  say("loading " .. where)
  web.load(where)
end

field.on_enter = function(_, text)
  win:focus_on(view)
  web.focus(true)
  visit(text)
end

back.on_click = function() web.back() end
forward.on_click = function() web.forward() end
reload.on_click = function() web.reload() end

local function status_text()
  if page.loading then
    return ("Loading %d%%"):format(math.floor(page.progress * 100 + 0.5))
  end

  return page.said
end

--------------------------------------------------------------------------
-- One frame: the header, the page, the status line.
--

local function frame()
  local s = win:surface()

  if not s then return end

  back.disabled = not page.can_back
  forward.disabled = not page.can_forward

  ui.paint_view(header, s, 0, 0)

  -- A page on its way, along the header's rule.
  if page.loading and page.progress > 0 then
    s:fill(0, HEAD - 2, math.floor(W * page.progress), 2, theme.accent)
  end

  web.paint(s, 0, VIEW_Y, W, VIEW_H)

  s:fill(0, H - STAT, W, STAT, theme.window)
  s:fill(0, H - STAT, W, 1, theme.line)
  s:text(10, H - STAT + (STAT - gfx.height()) // 2, status_text(), theme.text_dim)

  win:commit()
end

win.on_paint = function() frame() end

-- Resized by its grip: everything laid out again, and WebKit told the
-- page's new size, which it lays out and paints at.
win.on_resize = function(_, w, h)
  if w == W and h == H then return end

  W, H = w, h
  layout()
  web.resize(W, VIEW_H)
  web.frame(X, Y, W, H)
  print(("jetski: resized to %dx%d"):format(W, H))
  frame()
end

--
-- **WebKit, a slice a pass**: longer while a page is arriving, short when
-- it is still, and what it said taken into the page's state. A pass that
-- changed what is on screen says so, and the kit paints.
--
win.poll_wait_ticks = 1

win.on_frame = function()
  web.pump(page.loading and 12 or 4, st)

  local changed = st.picture

  if st.started then page.loading = true end
  if st.committed then say("") end

  if st.failed then
    say(("could not load %s: error %d"):format(page.address, st.error))
    changed = true
  end

  if st.crashed then
    say("the page's process ended")
    changed = true
  end

  if st.finished then
    say(("loaded %s"):format(st.address))
    changed = true
  end

  if st.loading ~= page.loading or st.progress ~= page.progress
     or st.can_back ~= page.can_back or st.can_forward ~= page.can_forward then
    changed = true
  end

  page.loading, page.progress = st.loading, st.progress
  page.can_back, page.can_forward = st.can_back, st.can_forward

  if st.title ~= page.title then
    page.title = st.title
    win:retitle(page.title ~= "" and page.title or "JetSki")
    print("jetski: title " .. page.title)
  end

  -- The address as WebKit has it - after a redirect, a link, Back - unless
  -- somebody is typing one.
  if st.address ~= "" and st.address ~= page.address and not field.focused then
    page.address = st.address
    field.text = st.address
    changed = true
  end

  return changed
end

-- Control-L: the address, to type over.
win.on_key = function(_, c)
  if c == 12 then
    web.focus(false)
    win:focus_on(field)
    return true
  end

  return false
end

win:focus_on(view)
visit(files.words(args)[1] or HOME)
win:run()
