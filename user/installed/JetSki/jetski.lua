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
--   Control-T    a new tab           Super-1..9  that tab
--
-- A tab closes by its close; Control-W is the desktop's own prefix
-- (`wm.lua`, Control-W then Q), so it is never a browser's here.
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
local keys  = use("/Kosmos/Libraries/keys.lua")
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
local TABS = 38                      -- the tabs, the window's top row
local BAR = ui.layout.head           -- back, forward, reload and the address
local HEAD = TABS + BAR
local STAT = 26                      -- what the page is doing, along the bottom
local VIEW_Y = HEAD
local VIEW_H = H - HEAD - STAT

local X, Y = 70, 50
--
-- **The tabs are the window's top row** (`docs/webkit.html`, agreed 11
-- November 2026): a window with a header of its own, so the kit draws no
-- band above it, and the window manager's three sit at the strip's right
-- end - where the look has no title bars; where it has, they stay above.
--
local win, why = ui.window{ title = "JetSki", w = W, h = H, x = X, y = Y, direct = true,
                            header = true, resizable = true }

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

local header = ui.view{ x = 0, y = TABS, w = W, h = BAR }

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
    b.x, b.y = x, (BAR - 1 - b.h) // 2
    x = x + b.w + gap
    header:add(b)
  end

  field.x = x + 4
  field.y = (BAR - 1 - field.h) // 2
  header:add(field)
end

win:add(header)

--------------------------------------------------------------------------
-- **The strip**: a tab for each page, the shown one joined to the bar
-- below it, a close on each, and + after the last. Its empty band moves
-- the window, so the tabs and the + are views of their own on it.
--

local strip = ui.view{ x = 0, y = 0, w = W, h = TABS, moves_window = true }
local new_tab, select_tab, close_tab     -- below, once the page's state is
local tabs = {}                      -- `web.tabs` fills it: id, shown, loading, title
local TAB_W, TAB_MIN, CLOSE = 210, 90, 22

-- The strip's ground, between the window and its rules as the mockup's
-- `#eef0f4` is, so it is right in every look (`theme.mix`).
local function strip_ground() return theme.mix(theme.window, theme.line_soft, 500) end
local LIGHTS_ROOM = 84               -- the three, where they sit on the strip

function strip:draw(g)
  g:fill(0, 0, self.w, self.h, strip_ground())
  g:fill(0, self.h - 1, self.w, 1, theme.line)
  if win.headed and win.lights then
    win:place_lights(self, self.w - ui.layout.lights_in - win.lights.w,
                     (self.h - win.lights.h) // 2)
  end
end

local function tab_view(i)
  local v = ui.view{ h = TABS - 6, y = 6 }

  function v:draw(g)
    local t = tabs[i]
    if not t then return end
    local fill = t.shown and theme.window or strip_ground()
    g:fill_round(0, 0, self.w, self.h + 8, fill, 8)
    if not t.shown then g:fill(self.w - 1, 7, 1, self.h - 14, theme.line) end
    local dot = t.loading and theme.accent or theme.text_dim
    g:fill_round(10, (self.h - 8) // 2, 8, 8, dot, 4)
    local title = t.title ~= "" and t.title or "New tab"
    g:text(26, (self.h - gfx.height()) // 2,
           ui.fitted(title, self.w - 26 - CLOSE - 6),
           t.shown and theme.text or theme.text_dim, fill)
    g:line_icon(self.w - CLOSE - 2, (self.h - 15) // 2, "close", theme.text_dim)
  end

  function v:mouse(action, x)
    local t = tabs[i]
    if action ~= "press" or not t then return false end
    if x >= self.w - CLOSE - 6 then
      close_tab(t.id)
    else
      select_tab(t.id)
    end
    return true
  end

  return v
end

local plus = ui.view{ w = 30, h = TABS - 8, y = 4 }

function plus:draw(g)
  g:line_icon((self.w - 15) // 2, (self.h - 15) // 2, "plus", theme.text_dim)
end

function plus:mouse(action)
  if action == "press" then new_tab() end
  return true
end

local tab_views = {}

-- The tabs laid along the strip: as wide as `TAB_W`, narrower as they
-- crowd, and never under `TAB_MIN`.
local function lay_tabs()
  local room = W - 12 - plus.w - LIGHTS_ROOM
  local each = math.max(TAB_MIN, math.min(TAB_W, room // math.max(1, #tabs)))

  for i = #tab_views + 1, #tabs do
    tab_views[i] = tab_view(i)
    strip:add(tab_views[i])
  end
  for i = #tabs + 1, #tab_views do
    strip:remove(tab_views[i])
    tab_views[i] = nil
  end

  local x = 8
  for i = 1, #tabs do
    tab_views[i].x, tab_views[i].w = x, each
    x = x + each
  end
  plus.x = x + 4
end

strip:add(plus)
win:add(strip)

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
  strip.w, header.w = W, W
  lay_tabs()
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

-- Keys that are the window's, never the page's: Control-L, Control-T,
-- and Super with a digit.
local function windows_key(c)
  if c == 12 or c == 20 then return true end
  local key, mods = keys.parts(c)
  return mods & keys.SUPER ~= 0 and key >= 49 and key <= 57
end

function view:key(c)
  if windows_key(c) then return false end
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
local page = { title = "", address = HOME, said = "", loading = false, going = "",
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

--------------------------------------------------------------------------
-- **Tabs**: WebKit's pages, one shown. Which tab is shown is WebKit's to
-- say (`st.tab`); the strip is drawn from `web.tabs`, asked again whenever
-- a tab may have changed.
--

local function tabs_changed()
  web.tabs(tabs)
  lay_tabs()
end

-- The window's state forgotten, so the next pass takes the tab shown.
local function shown_changed()
  page.title, page.address = nil, ""
  tabs_changed()
  for i, t in ipairs(tabs) do
    if t.shown then print(("jetski: tab %d of %d"):format(i, #tabs)) end
  end
end

function select_tab(id)
  if web.tab_select(id) then shown_changed() end
end

-- A new tab, shown, its address field waiting for something typed.
function new_tab()
  local id = web.tab_new()
  if not id then return end
  web.tab_select(id)
  shown_changed()
  field.text = ""
  web.focus(false)
  win:focus_on(field)
end

-- The last tab closed is the window closed, as Safari's is.
function close_tab(id)
  if #tabs <= 1 then
    win:close()                      -- `win:run` ends with it
    return
  end
  if web.tab_close(id) then shown_changed() end
end

back.on_click = function() web.back() end
forward.on_click = function() web.forward() end
-- Reload is Stop while a page is on its way, as in every browser.
reload.on_click = function()
  if page.loading then web.stop() else web.reload() end
end

local function status_text()
  -- Followed and not begun: where to, by its host, from the first frame.
  if page.loading and page.going ~= "" then
    return "Opening " .. (page.going:match("^%a[%w+.%-]*://([^/?#]+)") or page.going) .. "..."
  end

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
  reload.icon = page.loading and "close" or "reload"

  ui.paint_view(strip, s, 0, 0)
  ui.paint_view(header, s, 0, TABS)

  -- A page on its way, along the header's rule - a sliver of it from the
  -- moment a link is followed, before WebKit has a figure to give.
  if page.loading then
    s:fill(0, HEAD - 3, math.max(math.floor(W * page.progress), W // 16), 3, theme.accent)
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

  -- A page that opened a tab, `target=_blank`: WebKit shows it already.
  if st.opened ~= 0 then
    print("jetski: a page opened a tab")
    shown_changed()
    changed = true
  end
  if st.closing ~= 0 then
    close_tab(st.closing)
    changed = true
  end
  if st.tabs or st.started or st.finished or st.failed or st.crashed or st.asked then
    tabs_changed()
    changed = true
  end

  if st.started then page.loading = true end
  if st.asked then
    print("jetski: following " .. st.going)
    changed = true
  end
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

  if st.loading ~= page.loading or st.progress ~= page.progress or st.going ~= page.going
     or st.can_back ~= page.can_back or st.can_forward ~= page.can_forward then
    changed = true
  end

  page.loading, page.progress, page.going = st.loading, st.progress, st.going
  page.can_back, page.can_forward = st.can_back, st.can_forward

  if st.title ~= page.title then
    page.title = st.title
    tabs_changed()
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

  if c == 20 then new_tab() return true end

  -- Super with a digit: that tab, and 9 the last, as every browser has it.
  local key, mods = keys.parts(c)
  if mods & keys.SUPER ~= 0 and key >= 49 and key <= 57 then
    local t = (key == 57) and tabs[#tabs] or tabs[key - 48]
    if t then select_tab(t.id) end
    return true
  end

  return false
end

tabs_changed()
win:focus_on(view)
visit(files.words(args)[1] or HOME)
win:run()
