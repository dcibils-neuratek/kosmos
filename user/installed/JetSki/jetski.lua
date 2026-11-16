-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki.
-- kosmos: application
-- kosmos: icon App_NetSurf
-- kosmos: name JetSki
-- kosmos: section applications
-- kosmos: needs network
-- kosmos: about WebKit on Kosmos: the web as Safari draws it
-- kosmos: image webkit
-- kosmos: depends webkit
-- kosmos: opens html htm
--
--   wm jetski                              the start page
--   wm jetski:https://en.wikipedia.org/    that page
--   wm jetski:/Home/page.html              a page from the disk
--
--   Control-L    type an address
--   Escape       the address as it was
--   Control-T    a new tab           Super-1..9  that tab
--   Control-O    an HTML file from the disk, by the Open window;
--                one dropped on the window opens as well
--   Control-Alt-I  Web Inspector, docked to the right, and away again
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
-- them. Kosmos's own application, kept on the disk because it is too big
-- for the image (Diego, 15 November 2026: "/Home/System, seen as /Kosmos"):
-- this file in `/Home/System/Apps/JetSki`, which every program sees, read
-- only, as `/Kosmos/Apps/JetSki`; the system's image carries none of it.
-- **The image it runs in is Kosmos's WebKit** (Diego, 15 November 2026:
-- "One WebKit image", in "/Kosmos/WebKit"), which Mail runs in too:
-- `webkit.elf` in `/Kosmos/WebKit`, named by `-- kosmos: image webkit`
-- (`docs/mailweb.md`).
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
local clock = use("/Kosmos/Libraries/clock.lua")
local theme = ui.theme

local have, webkit = pcall(use, "/Kosmos/Kits/webkit")

if not have or type(webkit) ~= "table" then
  print("jetski: " .. tostring(webkit))
  return
end

--
-- **JetSki's tabs, over the WebKit Kit's views** (`docs/mailweb.md` W2).
-- The kit makes views - a page each, painted into this window's surface -
-- and knows nothing of tabs; a tab is a view, and which one is shown is
-- JetSki's to say. `web` is the tab shown, and the tabs, in the words the
-- window below was written in when the kit was JetSki's own door: one
-- page, with tabs inside it. (The pointer's, the wheel's and the keys' are
-- written `web.key = function`, so `x86-jetski`'s deaf controls change only
-- the calls.)
--
local web = {}
local views = {}                     -- a view's id -> the view
local shown                          -- the view in the window
local targeting = false              -- input to Web Inspector's page
local size_w, size_h = 1, 1          -- every view's size: the page's
local events = {}                    -- `webkit.pump` fills it

local function show(v)
  if shown == v then return end
  if shown then shown:show(false) end
  shown = v
  v:show(true)
  v:target(targeting)
end

function web.open(w, h)
  size_w, size_h = w, h
  local v, why = webkit.view(w, h)
  if not v then return nil, why end
  views[v:id()] = v
  show(v)
  v:focus(true)
  return true
end

-- A tab: its id, or nil. Dark until `tab_select`.
function web.tab_new()
  local v = webkit.view(size_w, size_h)
  if not v then return nil end
  views[v:id()] = v
  return v:id()
end

function web.tab_select(id)
  if not views[id] then return false end
  show(views[id])
  return true
end

-- A tab closed - never the last, which is the window's to close - and the
-- one after it shown in its place, or the one before when it was last.
local order = {}
function web.tab_close(id)
  local v = views[id]
  local n = webkit.views(order)
  if not v or n < 2 then return false end
  local after
  for i = 1, n do
    if order[i].id == id then after = order[i + 1] or order[i - 1] break end
  end
  v:close()
  views[id] = nil
  if v == shown then
    shown = nil
    show(views[after.id])
  end
  return true
end

function web.tabs(list) return webkit.views(list) end

function web.resize(w, h)
  size_w, size_h = w, h
  for _, v in pairs(views) do v:resize(w, h) end
end

-- WebKit for a while, then the tab shown's news into `st`: `tab` is which
-- one, `opened` a tab a page opened - shown, as a browser shows it -
-- `closing` one whose page asked to close, and `tabs` that a tab not shown
-- began, finished or ended.
function web.pump(ms, st)
  webkit.pump(ms, events)
  -- What the kit wrote - the trace's lines - into the log, a line a print.
  local said = webkit.said()
  if said ~= "" then
    for line in said:gmatch("[^\n]+") do print(line) end
  end
  if events.opened ~= 0 then
    local v = webkit.adopt(events.opened)
    views[events.opened] = v
    show(v)
  end
  shown:state(st)
  st.tab, st.opened, st.closing, st.tabs = shown:id(), events.opened, events.closing,
                                           events.others
  return st
end

function web.crash(id) return views[id] and views[id]:crash() end
function web.target(on) targeting = on shown:target(on) end
function web.shown_id() return shown and shown:id() end
function web.load(address) shown:load(address) end
function web.back() shown:back() end
function web.forward() shown:forward() end
function web.reload() shown:reload() end
function web.stop() shown:stop() end
function web.focus(on) shown:focus(on) end
web.mouse = function(...) shown:mouse(...) end
web.wheel = function(...) shown:wheel(...) end
web.key = function(c) return shown:key(c) end
function web.paint(...) shown:paint(...) end
function web.paint_inspector(...) shown:paint_inspector(...) end
function web.inspect(on) shown:inspect(on) end
function web.inspecting() return shown:inspecting() end
function web.security() return shown:security() end
function web.anyway() return shown:anyway() end
function web.kept(n) return shown:kept(n) end
web.frame, web.developer = webkit.frame, webkit.developer
web.inspector_files, web.inspector_size = webkit.inspector_files, webkit.inspector_size

--
-- **JetSki's Settings** (`docs/webkit.html`, *Settings*; `roadmap.md`,
-- S1): its own file beside the cookies, read for what this JetSki knows
-- (`browserprefs.lua`), its own page in place of the old browser's
-- `about:start`. The tabs it had are read before any tab is made, which
-- would write over them.
--
local SETTINGS = "/Home/Settings/JetSki/settings"
local TABS_FILE = "/Home/Settings/JetSki/tabs"
local OWN = { home = "https://en.wikipedia.org/wiki/BeOS" }
local setting = prefs.read(SETTINGS, OWN)
local had, had_shown = prefs.tabs(TABS_FILE)
local HOME = setting.home

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

-- **Developer features** (`docs/webkit.html`, *Developer mode*): Web
-- Inspector, its toolbar button and its right-click item. On by default
-- in a development build and off in a stable one, by Settings' *Show
-- developer features* - and JetSki has no Settings yet, and every build
-- is a development one until Diego renames it, so on.
local DEVELOPER = true
-- Web Inspector's width, docked against the page's right edge as the
-- drawing has it; never more than half the window.
local INSPECTOR_W = 548
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

-- Web Inspector's pages, WebKit's own, in its folder (`tools/inspector.py`).
local WEBKIT = "/Kosmos/WebKit"
web.inspector_files(WEBKIT .. "/Inspector")
web.developer(DEVELOPER)

-- What Settings says of every page, from the first one.
webkit.settings{ scripts = setting.javascript, images = setting.images }
webkit.zoom(setting.zoom)

-- **A page's load, traced in the log** (`webkit.cpp`, *A page's load,
-- traced*): every request as it ends, and what a page still waits on, for
-- measuring JetSki's speed. On while `/Home/Settings/JetSki/trace` exists -
-- `touch` it, and open JetSki again; `log webkit` reads what it said.
local TRACE = "/Home/Settings/JetSki/trace"
webkit.trace(fs.getattr(TRACE) ~= nil)

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
-- Web Inspector's, at the bar's right end, while the developer features are on.
local inspect = ui.iconbutton{ icon = "debug", hidden = not DEVELOPER }
-- The menu, at the end as the drawing's three dots are: Settings in it.
local more    = ui.iconbutton{ icon = "more" }

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
  inspect.y = (BAR - 1 - inspect.h) // 2
  header:add(inspect)
  more.y = (BAR - 1 - more.h) // 2
  header:add(more)
end

-- **The shield** (`docs/padlock.html`, `adblock.md` B4): what Privacy and
-- Ad Blocking kept out of this page, its count beside it, and pressed, its
-- panel (below). Off for this site, it is the shield struck through.
local shield = ui.view{ w = 30, h = 26 }
shield.count, shield.off = 0, false
function shield:draw(g)
  local icon = self.off and "unshielded" or "shield"
  local ink = self.off and theme.text_dim or (self.count > 0 and theme.accent or theme.text_dim)
  local words = self.count > 0 and tostring(self.count) or ""
  local x = (self.w - 15 - (words ~= "" and gfx.measure(words, "label") + 3 or 0)) // 2
  g:line_icon(x, (self.h - 15) // 2, icon, ink)
  if words ~= "" then
    g:text(x + 18, (self.h - gfx.height("label")) // 2, words, ink, nil, "label")
  end
end
shield.y = (BAR - 1 - shield.h) // 2
header:add(shield)

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
local gone                           -- a tab whose page stopped, below
local refused_page                   -- a tab whose certificate was refused, below
local lock_close                     -- the padlock's panel put away, below
local shield_close                   -- the shield's, below
local settings_page                  -- Settings, in a tab of its own, below
local open_settings, fill_settings, cover
local settings_tab                   -- the tab Settings is in, or nil
local pending = {}                   -- a tab's address, loaded when it is first shown

-- **The page and its inspector, side by side**: the page `PAGE_W` wide, a
-- rule, and the inspector after it. `inspecting` is WebKit's word - the
-- shown tab's inspector is docked - and `to_inspector` where the pointer
-- was pressed last, which is where keys go.
local PAGE_W, SPLIT_W = W, 0
local inspecting, to_inspector = false, false

-- **Every size from the window's**: the header and the address across it,
-- the page below it, and the status line along the bottom.
local function layout()
  VIEW_H = math.max(1, H - HEAD - STAT)
  strip.w, header.w = W, W
  lay_tabs()
  local right = W - ui.layout.head_edge
  more.x = right - more.w
  right = more.x - ui.layout.head_gap
  if not inspect.hidden then
    inspect.x = right - inspect.w
    right = inspect.x - ui.layout.head_gap
  end
  if shield then
    shield.x = right - shield.w
    right = shield.x - ui.layout.head_gap
  end
  field.w = right - field.x
  SPLIT_W = inspecting and math.min(INSPECTOR_W, W // 2) or 0
  PAGE_W = W - SPLIT_W
  view.w, view.h = W, VIEW_H
  if gone then gone.w, gone.h = PAGE_W, VIEW_H end
  if refused_page then refused_page.w, refused_page.h = PAGE_W, VIEW_H end
  if settings_page then settings_page.w, settings_page.h = W, VIEW_H end
end

layout()

view.focusable = true
view.takes_tab = true                -- Tab is the page's, between its fields

function view:draw() end

-- Which of the two a point is over, and the point in its coordinates: the
-- page left of the rule, the inspector right of it.
local function over(x, y)
  if inspecting and x >= PAGE_W then
    return true, math.max(0, x - PAGE_W - 1), y
  end
  return false, x, y
end

function view:mouse(action, x, y)
  if action == "press" then
    lock_close()
    shield_close()
    local there, px, py = over(x, y)
    to_inspector = there
    web.target(there)
    web.focus(true)
    web.mouse("press", "left", px, py)
  else
    -- A drag stays with what was pressed, even across the rule.
    local px = to_inspector and math.max(0, x - PAGE_W - 1) or x
    if action == "release" then
      web.mouse("release", "left", px, y)
    else
      web.mouse("move", nil, px, y)
    end
  end

  return false                       -- WebKit says when there is a picture
end

function view:wheel(n, x, y)
  local there, px, py = over(x, y)
  web.target(there)
  web.wheel(px, py, n)
  web.target(to_inspector)
  return true
end

-- Keys that are the window's, never the page's: Control-L, Control-T,
-- Control or Super with a comma, and Super with a digit.
local function inspector_key(c)
  local key, mods = keys.parts(c)
  return DEVELOPER and mods & keys.CTRL ~= 0 and mods & keys.ALT ~= 0
         and (key == 73 or key == 105)
end

local function windows_key(c)
  if c == 12 or c == 15 or c == 20 then return true end
  if inspector_key(c) then return true end
  local key, mods = keys.parts(c)
  if key == 44 and mods & (keys.CTRL | keys.SUPER) ~= 0 then return true end
  return mods & keys.SUPER ~= 0 and key >= 49 and key <= 57
end

function view:key(c)
  if windows_key(c) then return false end
  return web.key(c)
end

win:add(view)

--------------------------------------------------------------------------
-- **A tab whose page stopped** (`docs/webkit.html`, drawn 14 November
-- 2026): its process ended, so there is no picture, and this is drawn in
-- the page's place instead - with **What happened** under it, how Nebula
-- recorded the death, because a development build should say (Diego: "it
-- should help us work in bugs and edge cases easily", "right now we are
-- blind on whats going in"). `crash` is `web.crash`'s table for the tab
-- shown, or nil while its page is alive.
--

local crash = nil
gone = ui.view{ x = 0, y = VIEW_Y, w = W, h = VIEW_H, hidden = true }
local MONO_LH = gfx.height("mono") + 3
local LABEL_W = 112

local function hex(n) return n and ("0x%x"):format(n) or "?" end

local function host_of(address)
  return address:match("^%a[%w+.%-]*://([^/?#]+)") or address
end

-- The fault in words: which kind of access, and whether the page was there.
local function fault_words(c)
  if c.fault == "page fault" then              -- x86: the error code's bits
    local d = c.detail
    local what = d & 16 ~= 0 and "an instruction fetch"
                 or (d & 2 ~= 0 and "a write" or "a read")
    return ("page fault: %s, %s"):format(what, d & 1 ~= 0 and "not allowed" or "not present")
  elseif c.fault and c.fault:match("abort") then -- ARM: the syndrome's ISS
    local iss = c.detail & 0x1ffffff
    local status = iss & 0x3f
    local what = c.fault:match("instruction") and "an instruction fetch"
                 or (iss & 64 ~= 0 and "a write" or "a read")
    local why = (status >= 4 and status <= 7) and "not present"
                or ((status >= 12 and status <= 15) and "not allowed" or ("status 0x%x"):format(status))
    return ("%s: %s, %s"):format(c.fault, what, why)
  end
  return c.fault or "?"
end

-- What an address is, as far as can be told from the record alone.
local function address_words(c)
  local a = c.addr
  if a < 4096 then return hex(a) .. " - a null pointer" end
  if c.sp and math.abs(a - c.sp) < 64 * 1024 then
    return ("%s - %d bytes from thread %d's stack pointer"):format(hex(a), a - c.sp, c.thread)
  end
  return hex(a)
end

-- **What happened**, as label and value: what is drawn, and what Copy
-- details puts on the clipboard.
-- **Which function** (`docs/debug.md`): the Debug Kit reads this image's
-- symbols - the web process runs this same image, so a fault's instruction
-- is an address in the file beside this one. Looked up once per address,
-- the pass after the stopped page is first drawn, so it never waits for
-- the 7 MB of symbols to be read; the kit's table given back after.
local have_debug, debugkit = pcall(use, "/Kosmos/Kits/debug")
if not have_debug or type(debugkit) ~= "table" then debugkit = nil end
local IMAGE = WEBKIT .. "/webkit.elf"
local named = {}                     -- an address -> its function's name, or false

local function look_up(pc)
  if named[pc] == nil and debugkit then
    local name, offset = debugkit.name(IMAGE, pc)
    named[pc] = name and ("%s + 0x%x"):format(name, offset) or false
    if not name then print("jetski: no function's name for " .. hex(pc) .. ": " .. tostring(offset)) end
    debugkit.forget()
  end
  return named[pc]
end

local function function_of(pc)
  local n = named[pc]
  if n == nil then return debugkit and "looking it up" or "no Debug Kit here" end
  return n or "no symbol holds it"
end

local function crash_rows(c)
  local rows = { { "process", ("web, %d"):format(c.pid) } }
  if not c.known then
    rows[#rows + 1] = { "fault", "not collected yet" }
  elseif c.fault then
    rows[#rows + 1] = { "fault", fault_words(c) }
    rows[#rows + 1] = { "address", address_words(c) }
    rows[#rows + 1] = { "at", hex(c.pc) .. " in webkit.elf" }
    rows[#rows + 1] = { "in", function_of(c.pc) }
    rows[#rows + 1] = { "thread", ("%d, stack pointer %s"):format(c.thread, hex(c.sp)) }
  else
    rows[#rows + 1] = { "ended", ("with code %d, not by a fault"):format(c.code or 0) }
  end
  if c.address ~= "" then rows[#rows + 1] = { "loading", c.address } end
  return rows
end

local function crash_details(c)
  local b = sys.build and sys.build() or {}
  local out = { ("JetSki, Kosmos %s - a page stopped"):format(b.version or "?") }
  for _, r in ipairs(crash_rows(c)) do
    out[#out + 1] = ("%-9s %s"):format(r[1], r[2])
  end
  return table.concat(out, "\n") .. "\n"
end

local gone_reload = ui.button{ text = "Reload", go = true, on_click = function()
  web.reload()
end }
local gone_close = ui.button{ text = "Close tab", on_click = function()
  for _, t in ipairs(tabs) do
    if t.shown then close_tab(t.id) return end
  end
end }
local gone_copy = ui.button{ text = "Copy details", on_click = function()
  if crash then
    use("/Kosmos/Libraries/wmproto.lua").copy(crash_details(crash))
    print("jetski: the page's details copied")
  end
end }

gone:add(gone_reload)
gone:add(gone_close)
gone:add(gone_copy)

-- Where everything on it goes, from its size: one column, in the middle.
local GONE_COL = 600

local function gone_top()
  local rows = crash and #crash_rows(crash) or 0
  local tall = 40 + 32 + 28 + 26 + 30 + 18 + 30 + rows * MONO_LH + 12
  return math.max(20, (gone.h - tall) // 2)
end

local function lay_gone()
  local top = gone_top() + 40 + 32 + 28 + 26
  local widths = gone_reload.w + gone_close.w + gone_copy.w + 16
  local x = (gone.w - widths) // 2
  gone_reload.x, gone_reload.y = x, top
  gone_close.x, gone_close.y = x + gone_reload.w + 8, top
  gone_copy.x, gone_copy.y = gone_close.x + gone_close.w + 8, top
end

function gone:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  if not crash then return end
  lay_gone()

  local y = gone_top()
  local mid = self.w // 2
  local function centred(text, color, role, px, at)
    local tw = gfx.measure(text, role and px and ui.sized(role, px) or nil)
    g:text(mid - tw // 2, at, text, color, theme.window, role, px)
  end

  g:fill_round(mid - 16, y, 32, 32, theme.line_soft, 16)
  g:fill_round(mid - 12, y + 4, 24, 24, theme.window, 12)
  y = y + 40
  centred("This page stopped", theme.text, "ui", 18, y)
  y = y + 32
  centred(("The page at %s ended unexpectedly. Your other tabs are fine."):format(
          host_of(crash.address)), theme.text_dim, nil, nil, y)

  -- What happened, in a box under the buttons.
  local rows = crash_rows(crash)
  local bw = math.min(GONE_COL, self.w - 32)
  local bx = mid - bw // 2
  local by = gone_reload.y + gone_reload.h + 18
  local bh = 30 + #rows * MONO_LH + 12
  g:fill_round(bx, by, bw, bh, theme.line, 8)
  g:fill_round(bx + 1, by + 1, bw - 2, bh - 2, theme.field or theme.window, 7)
  g:text(bx + 12, by + 8, "What happened", theme.text, nil)
  local b = sys.build and sys.build() or {}
  local tag = ("Kosmos %s - development"):format(b.version or "?")
  g:text(bx + bw - 12 - gfx.measure(tag), by + 8, tag, theme.text_dim, nil)
  g:fill(bx + 1, by + 28, bw - 2, 1, theme.line)

  local ry = by + 36
  for _, r in ipairs(rows) do
    g:text(bx + 12, ry, r[1], theme.text_dim, nil, "mono")
    g:text(bx + 12 + LABEL_W, ry, ui.fitted(r[2], bw - 24 - LABEL_W, "mono"),
           theme.text, nil, "mono")
    ry = ry + MONO_LH
  end
end

win:add(gone)

--------------------------------------------------------------------------
-- **Settings** (`docs/webkit.html`, *Settings*; `roadmap.md`, S1): a tab
-- of JetSki's own at `about:settings` - a view of the kit's that loads
-- nothing, which this page is drawn over - in the kit's cards, as the old
-- browser's (`browser.lua`, d5), two columns when there is room. Every
-- change is kept the moment it is made, and reaches every tab at once.
-- History, cookies, and Privacy and Ad Blocking are S2 to S4.
--

settings_page = ui.view{ x = 0, y = VIEW_Y, w = W, h = VIEW_H, hidden = true }
settings_page.scroll, settings_page.tall = 0, 0

function settings_page:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
end

function settings_page:wheel(n)
  local most = math.max(0, self.tall - self.h)
  self.scroll = math.max(0, math.min(most, self.scroll - n * 48))
  for _, c in ipairs(self.children) do c.y = c.top - self.scroll end
  return true
end

win:add(settings_page)

-- What Settings says of pages, handed to the kit: every view's.
local function apply_pages()
  webkit.settings{ scripts = setting.javascript, images = setting.images }
end

function fill_settings()
  local page = settings_page
  page.children = {}

  local function save(key, value, after)
    setting[key] = value
    prefs.write(setting, SETTINGS, OWN)
    print(("jetski: set %s %s"):format(key, tostring(value)))
    if after then after(value) end
  end

  local function switch(key, after)
    return ui.switch{ on = setting[key], on_change = function(_, on) save(key, on, after) end }
  end

  local function choice(key, after)
    return ui.dropdown{ choices = prefs.CHOICES[key], value = setting[key],
                        on_change = function(_, v) save(key, v, after) end }
  end

  local home = ui.field{ text = setting.home, w = 260 }
  home.on_change = function(_, text)
    if text ~= "" and not text:find("%c") and text ~= setting.home then save("home", text) end
  end

  local zoom = choice("zoom", function(v) webkit.zoom(v) end)
  local images = switch("images", apply_pages)
  local javascript = switch("javascript", apply_pages)
  local search = choice("search")

  local groups = {
    { name = "Starting", rows = {
        { label = "Home page", control = home },
        { label = "When JetSki opens", control = choice("opens") } } },
    { name = "Pages", rows = {
        { label = "Zoom", note = "All of the page: its words, pictures and boxes", control = zoom },
        { label = "Load images", control = images },
        { label = "JavaScript",
          note = "Off makes most sites plain or blank; for reading, or a page that misbehaves",
          control = javascript } } },
    { name = "Searching", rows = {
        { label = "Search with", note = "Words in the address field that are not an address",
          control = search } } },
  }

  local two = page.w >= 860
  local columns = two and { { groups[1], groups[2] }, { groups[3] } } or { groups }
  local each = page.w // #columns

  page.tall = 0
  for i, list in ipairs(columns) do
    local c = ui.cards{ x = (i - 1) * each, y = 0, w = each, h = 4000, groups = list, width = 440 }
    c.h = c.content_h + 16
    c.top = 0
    page:add(c)
    page.tall = math.max(page.tall, c.h)
  end
  page.scroll = math.max(0, math.min(page.scroll, page.tall - page.h))
  for _, c in ipairs(page.children) do c.y = c.top - page.scroll end

  -- Where its controls are in the window, for whoever drives it from
  -- outside (`x86-jetski`).
  local function at(v)
    local x, y = v:in_window()
    return ("%d,%d"):format(x + v.w // 2, y + v.h // 2)
  end
  print(("jetski: settings, %d columns, javascript %s, images %s, zoom %s, search %s")
        :format(#columns, at(javascript), at(images), at(zoom), at(search)))
end

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
               progress = 0, can_back = false, can_forward = false, crash_asks = 0,
               naming = false }

local function say(text)
  page.said = text
  print("jetski: " .. text)
end

--------------------------------------------------------------------------
-- **The padlock: how the page came** (`docs/padlock.html`, agreed 16
-- November 2026). At the head of the address, in the old browser's words
-- (`browser.lua`, `how_came`): a green padlock alone for a page whose
-- certificate checked out, as Safari and Chrome show it; Not encrypted for
-- plain HTTP; Not secure for one opened anyway, or one with something
-- plain in it; This machine for a file; JetSki for its own pages; and
-- Refused for a page JetSki would not open. `page.came` is the kit's
-- `security()` for the tab shown, asked again when its page changes.
--

local refusal = nil                  -- the tab shown's refused page, from `st`

local function how_came()
  if settings_tab and web.shown_id() == settings_tab then return "settings", "JetSki" end
  if refusal then return "refused", "Refused" end
  local c = page.came and page.came.came
  if c == "secure" then return "secure", nil end
  if c == "plain" then return "plain", "Not encrypted" end
  if c == "mixed" or c == "anyway" then return "refused", "Not secure" end
  if c == "file" then return "drive", "This machine" end
  return nil
end

local function came_ink(kind)
  if kind == "secure" then return theme.good end
  if kind == "refused" then return theme.bad end
  return theme.text_dim
end

-- The address in two: the host, drawn dark, and the rest, dim. The scheme
-- goes, since the padlock has just said how the page came.
local function address_parts(text)
  local bare = tostring(text or ""):gsub("^%a[%w+.-]*://", "")
  local host = bare:match("^[^/]*") or ""
  return host, bare:sub(#host + 1)
end

-- The padlock's width at the head of the field, 0 when there is none.
local function badge_w(kind, words)
  if not kind then return 0 end
  if not words then return 4 + 15 + 6 end
  return 4 + 15 + 5 + gfx.measure(words, "label") + 7
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
      local ink = came_ink(kind)
      local bw = badge_w(kind, words)
      if words then
        g:fill_round(4, 3, bw, h - 6, theme.mix(theme.sunken, ink, 140), 6)
        g:text(28, (h - gfx.height("label")) // 2, words, ink, nil, "label")
      end
      g:line_icon(8, (h - 15) // 2, kind, ink)
      x = 4 + bw + 6
    end

    local host, rest = address_parts(self.text)
    local room = self.w - x - 8
    local hw = gfx.measure(host, "label")
    if hw > room then
      host = ui.fitted(host, room, "label")
      hw, rest = gfx.measure(host, "label"), ""
    end
    g:text(x, (h - gfx.height("label")) // 2, host, theme.text, nil, "label")
    g:text(x + hw, (h - gfx.height()) // 2, ui.fitted(rest, room - hw), theme.text_dim)
  end
end

-- **Pressed: the connection and the certificate**, under the padlock. What
-- the certificate says is what WebKit's network process saw.
local lockpanel = ui.view{ w = 480, h = 10, hidden = true }
local lock = ui.view{ w = 1, h = 1 }  -- the padlock's place over the field

local function when(epoch)
  if not epoch or epoch <= 0 then return "not said" end
  local t = clock.at(epoch, 0)
  return ("%d %s %d"):format(t.day, clock.FULL_MONTHS[t.month], t.year)
end

-- The panel's heading, its line under it, and its rows.
local function lock_words()
  local kind = how_came()
  local host = host_of(page.address or "")
  local c = page.came or {}
  local head, under
  if kind == "secure" then
    head = ("The connection to %s is secure"):format(host)
    under = "Encrypted, and its certificate checked out"
  elseif c.came == "anyway" then
    head = ("The connection to %s is not secure"):format(host)
    under = "Opened anyway: its certificate did not check out, so this may not be " .. host
  elseif c.came == "mixed" then
    head = ("The connection to %s is not secure"):format(host)
    under = "Something on this page came over plain HTTP, and could have been changed on the way"
  elseif kind == "plain" then
    head = ("The connection to %s is not encrypted"):format(host)
    under = "Anyone on the way can read what is sent, and change it"
  elseif kind == "drive" then
    head, under = "This page is a file of this machine's", "No network had a part in it"
  elseif kind == "refused" then
    head, under = ("JetSki did not open %s"):format(host_of(refusal.address)), refusal.why
  else
    head, under = "One of JetSki's own pages", "Nothing of it came from a network"
  end
  local rows = {}
  if c.chain and c.chain > 0 then
    rows[#rows + 1] = { "Certificate for", c.names ~= "" and c.names or c.subject }
    rows[#rows + 1] = { "Issued by", c.issuer ~= "" and c.issuer or "not said" }
    rows[#rows + 1] = { "Valid until", when(c.valid_until) }
  end
  return head, under, rows
end

local PANEL_LH = 22

function lockpanel:draw(g)
  local head, under, rows = lock_words()
  local kind = how_came()
  local ink = came_ink(kind)
  g:fill_round(0, 0, self.w, self.h, theme.line, 9)
  g:fill_round(1, 1, self.w - 2, self.h - 2, theme.window, 8)
  if kind then g:line_icon(14, 16, kind, ink) end
  g:text(38, 14, ui.fitted(head, self.w - 52, "ui"), theme.text, nil, "ui")
  g:text(38, 36, ui.fitted(under, self.w - 52), theme.text_dim)
  local y = 64
  if #rows > 0 then
    g:fill(1, y - 6, self.w - 2, 1, theme.line_soft)
    for _, r in ipairs(rows) do
      g:text(14, y + 2, r[1], theme.text_dim)
      g:text(140, y + 2, ui.fitted(r[2], self.w - 154), theme.text)
      y = y + PANEL_LH
    end
  end
end

function lockpanel:mouse() return true end

function lock_close()
  if lockpanel.hidden then return false end
  lockpanel.hidden = true
  return true
end

local function lock_open()
  local _, _, rows = lock_words()
  lockpanel.h = 64 + #rows * PANEL_LH + (#rows > 0 and 10 or 0)
  lockpanel.x = field.x
  lockpanel.y = HEAD + 2
  lockpanel.hidden = false
  print(("jetski: the padlock pressed: %s"):format((lock_words())))
end

function lock:draw() end
header:add(lock)
function lock:mouse(action)
  if action ~= "press" then return true end
  if shield_close then shield_close() end
  if not lock_close() then lock_open() end
  return true
end

-- Laid over the badge's place, as wide as the badge is now.
local function lay_lock()
  local kind, words = how_came()
  lock.hidden = kind == nil or field.focused
  lock.x, lock.y = field.x + 4, field.y + 3
  lock.w, lock.h = math.max(1, badge_w(kind, words)), math.max(1, field.h - 6)
  -- Said when it changes, with where it is, for whoever drives the window
  -- from outside (`x86-padlock`).
  local now = tostring(kind) .. tostring(words)
  if now ~= lock.said then
    lock.said = now
    local x, y = lock:in_window()
    print(("jetski: padlock %s%s at %d,%d"):format(tostring(kind),
          words and (", " .. words) or "", x + lock.w // 2, y + lock.h // 2))
  end
end

--------------------------------------------------------------------------
-- **What was kept out** (`adblock.md` B4, `docs/padlock.html`; Diego, 16
-- November: "we need to show how many ads are blocked, and cookies, and
-- else somewhere in jetski", and "Let's do totals by month"). This page's
-- counts are the kit's; the month's are JetSki's, every tab's added as
-- they grow and kept in `/Home/Settings/Filters/kept`, a month a row, so
-- they outlive a restart and earlier months stay. The sites a person
-- allowed past blocking are `/Home/Settings/Filters/allowed`, handed to
-- the kit, which loads their pages with WebKit's content blockers off.
--

local KEPT_FILE = "/Home/Settings/Filters/kept"
local ALLOWED_FILE = "/Home/Settings/Filters/allowed"
local kept = fs.read(KEPT_FILE)
if type(kept) ~= "table" then kept = {} end
kept.months = type(kept.months) == "table" and kept.months or {}
local allowed = fs.read(ALLOWED_FILE)
if type(allowed) ~= "table" then allowed = {} end
local counted = { tab = nil, blocked = 0, cookies = 0 }
local kept_dirty = false

local function month_now()
  local t = clock.now()
  return t and ("%04d-%02d"):format(t.year, t.month) or "unknown"
end

local function month_of(key)
  local m = kept.months[key]
  if type(m) ~= "table" then
    m = { requests = 0, cookies = 0 }
    kept.months[key] = m
  end
  return m
end

-- Written when a page finishes or another tab is shown, and as JetSki
-- closes: a page's later blocks join the next write.
local function save_kept()
  if not kept_dirty then return end
  kept_dirty = false
  files.make_folder("/Home/Settings/Filters")
  fs.write(KEPT_FILE, kept)
  local m = month_of(month_now())
  print(("jetski: kept this month %d requests, %d cookies"):format(m.requests, m.cookies))
end

-- The site a switch is for: the page's host, its port and a leading www.
-- left off, so the switch holds for the site and everything under it.
local function site_of(address)
  local host = host_of(address or ""):gsub(":%d+$", ""):lower()
  return (host:gsub("^www%.", ""))
end

local function site_allowed(site)
  for _, s in ipairs(allowed) do
    if s == site then return true end
  end
  return false
end

local function hand_allowed()
  if have then webkit.allow(table.concat(allowed, "\n")) end
end
hand_allowed()

-- The lists in use, by the names people know them by.
local LIST_NAMES = { easylist = "EasyList", easyprivacy = "EasyPrivacy",
                     ["easylist-cookie"] = "EasyList Cookie", cookie = "EasyList Cookie" }
local function list_names()
  local out = {}
  for _, folder in ipairs({ "/Kosmos/Filters", "/Home/Settings/Filters" }) do
    for _, name in ipairs(fs.list(folder) or {}) do
      local stem = type(name) == "string" and name:match("^(.-)%.json") or nil
      if stem then out[#out + 1] = LIST_NAMES[stem:lower()] or stem end
    end
  end
  return #out > 0 and table.concat(out, ", ") or "no lists"
end

local function plural(n, one, many) return ("%d %s"):format(n, n == 1 and one or many) end

local function thousands(n)
  local s = tostring(n)
  while true do
    local done
    s, done = s:gsub("^(%d+)(%d%d%d)", "%1,%2")
    if done == 0 then return s end
  end
end

local shieldpanel = ui.view{ w = 420, h = 10, hidden = true }
local blocking_switch = ui.switch{ on = true }
shieldpanel:add(blocking_switch)
local SHIELD_LH = 22

local function shield_words()
  local site = site_of(page.address)
  local here = { requests = page.blocked or 0, cookies = page.cookies_blocked or 0 }
  local list, all = web.kept(2)
  local month = month_of(month_now())
  return site, here, list or {}, all or 0, month
end

-- How tall the panel is for what it says: the rows `draw` lays.
local function shield_height()
  local _, _, list, all = shield_words()
  local y = 66
  if shield.off then
    y = y + SHIELD_LH
  else
    y = y + SHIELD_LH + 6
    if #list > 0 then y = y + SHIELD_LH * (1 + #list + (all > #list and 1 or 0)) end
  end
  return y + 6 + 2 * SHIELD_LH + 8 + blocking_switch.h + 12
end

function shieldpanel:draw(g)
  local site, here, list, all, month = shield_words()
  g:fill_round(0, 0, self.w, self.h, theme.line, 9)
  g:fill_round(1, 1, self.w - 2, self.h - 2, theme.window, 8)
  g:line_icon(14, 16, shield.off and "unshielded" or "shield", shield.off and theme.text_dim or theme.accent)
  g:text(38, 14, "Privacy and Ad Blocking", theme.text, nil, "ui")
  g:text(38, 36, ui.fitted(list_names(), self.w - 52), theme.text_dim)
  local y = 66
  g:fill(1, y - 8, self.w - 2, 1, theme.line_soft)
  if shield.off then
    g:text(14, y, ui.fitted(("Blocking is off on %s"):format(site), self.w - 28), theme.text)
    y = y + SHIELD_LH
  else
    g:text(14, y, ("%s kept out    %s refused"):format(plural(here.requests, "request", "requests"),
           plural(here.cookies, "cookie", "cookies")), theme.text)
    y = y + SHIELD_LH + 6
    if #list > 0 then
      g:text(14, y, "Most blocked on this page", theme.text_dim)
      y = y + SHIELD_LH
      local shown_n = 0
      for _, k in ipairs(list) do
        g:text(14, y, ui.fitted(k.host, self.w - 90), theme.text)
        local n = tostring(k.count)
        g:text(self.w - 14 - gfx.measure(n), y, n, theme.text)
        shown_n = shown_n + k.count
        y = y + SHIELD_LH
      end
      if all > #list then
        local rest = ("and %s"):format(plural(all - #list, "other site", "other sites"))
        g:text(14, y, rest, theme.text_dim)
        local n = tostring(here.requests - shown_n)
        g:text(self.w - 14 - gfx.measure(n), y, n, theme.text_dim)
        y = y + SHIELD_LH
      end
    end
  end
  y = y + 6
  g:fill(1, y - 6, self.w - 2, 1, theme.line_soft)
  g:text(14, y, "This month, every site", theme.text_dim)
  y = y + SHIELD_LH
  g:text(14, y, ("%s requests kept out    %s cookies refused"):format(thousands(month.requests),
         thousands(month.cookies)), theme.text)
  y = y + SHIELD_LH + 8
  g:fill(1, y - 6, self.w - 2, 1, theme.line_soft)
  g:text(14, y + (blocking_switch.h - gfx.height()) // 2,
         ui.fitted(("Block adverts and trackers on %s"):format(site), self.w - 40 - blocking_switch.w),
         theme.text)
  blocking_switch.x, blocking_switch.y = self.w - 14 - blocking_switch.w, y
  if not self.said then
    self.said = true
    local x, sy = blocking_switch:in_window()
    print(("jetski: blocking switch at %d,%d"):format(x + blocking_switch.w // 2,
                                                       sy + blocking_switch.h // 2))
  end
end

function shieldpanel:mouse() return true end

function shield_close()
  if shieldpanel.hidden then return false end
  shieldpanel.hidden = true
  return true
end

local function shield_open()
  local site, here, list, all, month = shield_words()
  blocking_switch.on = not site_allowed(site)
  shieldpanel.said = false
  shieldpanel.x = math.max(8, shield.x + shield.w - shieldpanel.w)
  shieldpanel.y = HEAD + 2
  shieldpanel.hidden = false
  print(("jetski: the shield pressed: %d kept out, %d cookies refused; most blocked %s; this month %d, %d")
        :format(here.requests, here.cookies,
                list[1] and (list[1].host .. " " .. list[1].count) or "none",
                month.requests, month.cookies))
end

blocking_switch.on_change = function(_, on)
  local site = site_of(page.address)
  if site == "" then return end
  local list = {}
  for _, s in ipairs(allowed) do
    if s ~= site then list[#list + 1] = s end
  end
  if not on then list[#list + 1] = site end
  allowed = list
  files.make_folder("/Home/Settings/Filters")
  fs.write(ALLOWED_FILE, allowed)
  hand_allowed()
  print(("jetski: blocking %s for %s"):format(on and "on" or "off", site))
  web.reload()
end

function shield:mouse(action)
  if action ~= "press" then return true end
  lock_close()
  if not shield_close() then shield_open() end
  return true
end

-- Where the shield is and what it says, said when that changes, for
-- whoever drives the window from outside (`x86-padlock`).
local function shield_said()
  local now = ("%d %s"):format(shield.count, tostring(shield.off))
  if now == shield.said_was then return end
  shield.said_was = now
  local x, y = shield:in_window()
  print(("jetski: shield %d%s at %d,%d"):format(shield.count, shield.off and ", off" or "",
        x + shield.w // 2, y + shield.h // 2))
end

-- The page's counts into the month's, as they grow: the tab shown's, from
-- the moment it is shown or begins a page.
local function count_kept()
  if st.tab ~= counted.tab or st.started then
    counted.tab, counted.blocked, counted.cookies = st.tab, st.blocked or 0, st.cookies_blocked or 0
    return
  end
  local more = (st.blocked or 0) - counted.blocked
  local cookies = (st.cookies_blocked or 0) - counted.cookies
  if more > 0 or cookies > 0 then
    local m = month_of(month_now())
    m.requests = m.requests + math.max(0, more)
    m.cookies = m.cookies + math.max(0, cookies)
    counted.blocked, counted.cookies = st.blocked or 0, st.cookies_blocked or 0
    kept_dirty = true
  end
end

-- **A certificate refused** (`docs/padlock.html`): a page of JetSki's own
-- in the site's place, the reason in this machine's words, and *Open
-- anyway* for this tab alone, remembered nowhere.
local REASONS = {
  [2]  = "its certificate is signed by nobody this machine trusts",
  [9]  = "its certificate is not valid yet",
  [10] = "its certificate has expired",
  [18] = "its certificate signed itself, and nobody this machine trusts vouches for it",
  [19] = "its certificate's chain ends in an authority this machine does not trust",
  [20] = "its certificate is signed by nobody this machine trusts",
  [21] = "its certificate is signed by nobody this machine trusts",
  [23] = "its certificate was revoked",
  [62] = "its certificate is for another name",
}

local function refusal_why(code, words)
  return REASONS[code] or (words ~= "" and words) or "its certificate did not check out"
end

refused_page = ui.view{ x = 0, y = VIEW_Y, w = W, h = VIEW_H, hidden = true }

local refused_back = ui.button{ text = "Go back", go = true, on_click = function()
  web.back()
end }
local refused_anyway = ui.button{ text = "Open anyway, in this tab", on_click = function()
  if refusal then
    print("jetski: opened anyway: " .. refusal.address)
    web.anyway()
  end
end }
refused_page:add(refused_back)
refused_page:add(refused_anyway)

function refused_page:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  if not refusal then return end
  local mid = self.w // 2
  local y = math.max(20, (self.h - 200) // 2)
  local function centred(text, color, role, px, at)
    local tw = gfx.measure(text, role and px and ui.sized(role, px) or nil)
    g:text(mid - tw // 2, at, text, color, theme.window, role, px)
  end
  g:line_icon(mid - 7, y, "refused", theme.bad)
  y = y + 30
  centred(("JetSki did not open %s"):format(host_of(refusal.address)), theme.text, "ui", 18, y)
  y = y + 32
  centred("The server answered with a certificate this machine cannot check, so JetSki",
          theme.text_dim, nil, nil, y)
  y = y + 20
  centred(("cannot tell whether it is talking to %s or to someone pretending to be it.")
          :format(host_of(refusal.address)), theme.text_dim, nil, nil, y)
  y = y + 30
  centred(refusal.why, theme.bad, nil, nil, y)
  y = y + 34
  local widths = refused_back.w + refused_anyway.w + 8
  refused_back.x, refused_back.y = mid - widths // 2, y
  refused_anyway.x, refused_anyway.y = refused_back.x + refused_back.w + 8, y
  if refusal.placed ~= refused_anyway.x * 65536 + y then
    refusal.placed = refused_anyway.x * 65536 + y
    local ax, ay = refused_anyway:in_window()
    print(("jetski: open anyway at %d,%d"):format(ax + refused_anyway.w // 2,
                                                  ay + refused_anyway.h // 2))
  end
end

win:add(refused_page)

-- An address as WebKit wants one: a scheme always, `https` when none was
-- typed; words are a search, as the NetSurf browser's field has them.
local function visit(text)
  if text:match("^%s*" .. prefs.PAGE .. "%s*$") then
    open_settings()
    return
  end

  -- A path is a file of this machine's: `file:` and the path, which
  -- WebKit's network process reads from /Home with the files beside it.
  if text:match("^%s*/") then text = "file://" .. text:match("^%s*(.-)%s*$") end
  local where = prefs.destination(text, setting.search)

  if not where then return end

  if not where:match("^%a[%w+.%-]*:") then where = "https://" .. where end

  -- An address typed in Settings' tab: the tab a page's again.
  if settings_tab and web.shown_id() == settings_tab then
    settings_tab = nil
    cover()
  end

  page.address = where
  field.text = where
  say("loading " .. where)
  web.load(where)
end

field.on_enter = function(_, text)
  -- The page takes the keys; while a page of JetSki's own covers it - a
  -- refused or a stopped one - it cannot, and takes them when it is shown
  -- again (`cover`). Left in the field, the field stayed focused and drew
  -- no padlock (`x86-padlock`'s control found it).
  page.wants_keys = not win:focus_on(view)
  web.focus(true)
  visit(text)
end

--------------------------------------------------------------------------
-- **Tabs**: views, one shown (`web`, above). The strip is drawn from
-- `web.tabs`, asked again whenever a tab may have changed.
--

-- **The tabs kept as they change** (*When JetSki opens: The tabs it had*):
-- their addresses and which is shown, written only when that changed.
local kept_tabs
local function keep_tabs()
  local list, at = {}, 1
  for _, t in ipairs(tabs) do
    if t.address ~= "" and t.address ~= "about:blank" then
      list[#list + 1] = t.address
      if t.shown then at = #list end
    end
  end
  local now = table.concat(list, "\n") .. "\n" .. at
  if now == kept_tabs then return end
  kept_tabs = now
  prefs.save_tabs(list, at, TABS_FILE)
end

local function tabs_changed()
  web.tabs(tabs)
  for _, t in ipairs(tabs) do
    if t.id == settings_tab then
      t.title, t.address, t.loading = "Settings", prefs.PAGE, false
    elseif pending[t.id] then
      t.title, t.address = pending[t.id], pending[t.id]
    end
  end
  lay_tabs()
  keep_tabs()
end

-- Which of the three is over the page's place: Settings, a stopped page's
-- account, or the page.
function cover()
  local here = settings_tab ~= nil and web.shown_id() == settings_tab
  settings_page.hidden = not here
  gone.hidden = here or crash == nil
  refused_page.hidden = here or crash ~= nil or refusal == nil
  view.hidden = here or crash ~= nil or refusal ~= nil
  if here then fill_settings() end
  if page.wants_keys and not view.hidden then
    page.wants_keys = not win:focus_on(view)
  end
end

-- A tab made for an address and not shown yet - the tabs JetSki had - takes
-- it the first time it is shown, so opening loads one page, not all.
local function take_pending(id)
  local a = pending[id]
  if not a then return end
  pending[id] = nil
  if a == prefs.PAGE then settings_tab = id else visit(a) end
end

-- The window's state forgotten, so the next pass takes the tab shown.
local function shown_changed()
  page.title, page.address = nil, ""
  cover()
  tabs_changed()

  for i, t in ipairs(tabs) do
    if t.shown then print(("jetski: tab %d of %d"):format(i, #tabs)) end
  end
end

function select_tab(id)
  if web.tab_select(id) then
    take_pending(id)
    shown_changed()
  end
end

-- Settings, in its tab if it has one, or a new one.
function open_settings()
  if settings_tab then
    select_tab(settings_tab)
    return
  end
  local id = web.tab_new()
  if not id then return end
  settings_tab = id
  web.tab_select(id)
  shown_changed()
  print("jetski: settings opened")
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
  if web.tab_close(id) then
    if id == settings_tab then settings_tab = nil end
    pending[id] = nil
    shown_changed()
  end
end

back.on_click = function() web.back() end

-- **Web Inspector shown or put away**, for the tab shown. WebKit makes its
-- page and says so (`st.inspector`), and the split follows that rather
-- than this: what is docked is what WebKit says is.
local function toggle_inspector()
  if not DEVELOPER then return end
  print("jetski: " .. (inspecting and "hiding" or "showing") .. " Web Inspector")
  web.inspect(not inspecting)
end
inspect.on_click = toggle_inspector

more.on_click = function()
  local x, y = more:in_window()
  win:open_menu((win.origin_x or 0) + x, (win.origin_y or 0) + y + more.h, {
    { text = "New tab", on_choose = function() new_tab() end },
    { text = "Settings", on_choose = function() open_settings() end },
  })
end

-- The page's right-click: the toolbar's three, and Web Inspector while the
-- developer features are on. *Inspect Element* - Elements opened on what
-- was under the pointer - waits for WebKit's context menus, which this
-- port builds without (`OptionsKosmos.cmake`).
function view:on_context(x, y)
  local items = {
    { text = "Back", disabled = not page.can_back, on_choose = function() web.back() end },
    { text = "Forward", disabled = not page.can_forward, on_choose = function() web.forward() end },
    { text = "Reload", on_choose = function() web.reload() end },
  }
  if DEVELOPER then
    items[#items + 1] = { text = inspecting and "Hide Web Inspector" or "Show Web Inspector",
                          on_choose = toggle_inspector }
  end
  win:open_menu(win.origin_x + self.x + x, win.origin_y + self.y + y, items)
  return true
end
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

  lay_lock()
  shield_said()
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

  if not settings_page.hidden then
    ui.paint_view(settings_page, s, 0, VIEW_Y)
  elseif crash then
    ui.paint_view(gone, s, 0, VIEW_Y)
  elseif refusal then
    ui.paint_view(refused_page, s, 0, VIEW_Y)
  else
    web.paint(s, 0, VIEW_Y, PAGE_W, VIEW_H)
  end

  if inspecting then
    s:fill(PAGE_W, VIEW_Y, 1, VIEW_H, theme.line)
    web.paint_inspector(s, PAGE_W + 1, VIEW_Y, SPLIT_W - 1, VIEW_H)
  end

  if not lockpanel.hidden then ui.paint_view(lockpanel, s, lockpanel.x, lockpanel.y) end
  if not shieldpanel.hidden then
    shieldpanel.h = shield_height()
    ui.paint_view(shieldpanel, s, shieldpanel.x, shieldpanel.y)
  end

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
  web.resize(PAGE_W, VIEW_H)
  if inspecting then web.inspector_size(SPLIT_W - 1, VIEW_H) end
  web.frame(X, Y, W, H)
  if not settings_page.hidden then fill_settings() end
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

  -- Settings' tab is JetSki's: its name and address, whatever the view
  -- under it says.
  if settings_tab and st.tab == settings_tab then
    st.title, st.address = "Settings", prefs.PAGE
  end

  local changed = st.picture

  -- Web Inspector docked or gone, for the tab shown - by a key, its
  -- button, its own close, or another tab shown: the page narrower or
  -- whole again, and laid out for it.
  if st.inspector or web.inspecting() ~= inspecting then
    local now = web.inspecting()
    if now ~= inspecting then
      inspecting = now
      if not now then to_inspector = false web.target(false) end
      layout()
      web.resize(PAGE_W, VIEW_H)
      if now then web.inspector_size(SPLIT_W - 1, VIEW_H) end
      print(("jetski: Web Inspector %s, the page %d wide"):format(now and "docked" or "away", PAGE_W))
    end
    changed = true
  end

  -- A page that opened a tab, `target=_blank`: shown already, by `web.pump`.
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

  -- A page refused for its certificate, or no longer: JetSki's own page
  -- in its place, or the page again.
  local was_refused = refusal and refusal.address
  if st.refused then
    if was_refused ~= st.refused_address then
      refusal = { address = st.refused_address, code = st.certificate_error,
                  why = refusal_why(st.certificate_error, st.refused_reason) }
      say(("refused %s: %s (curl's %d, OpenSSL's %d: %s)"):format(host_of(refusal.address),
          refusal.why, st.error, st.certificate_error, st.refused_reason))
    end
  else
    refusal = nil
  end
  if (refusal and refusal.address) ~= was_refused then
    lock_close()
    cover()
    changed = true
  end

  -- What blocking kept out: the shield's count, the month's, kept.
  if st.tab ~= counted.tab then save_kept() end
  count_kept()
  if st.finished then save_kept() end
  page.blocked, page.cookies_blocked = st.blocked or 0, st.cookies_blocked or 0
  if page.blocked ~= shield.count or (st.unblocked or false) ~= shield.off then
    shield.count, shield.off = page.blocked, st.unblocked or false
    changed = true
  end
  if not shieldpanel.hidden and st.picture then changed = true end

  -- How the page came, asked again whenever it may have changed.
  if st.committed or st.finished or st.started or st.failed or st.tab ~= page.came_tab then
    local was = page.came and page.came.came
    page.came, page.came_tab = web.security(), st.tab
    local now = page.came and page.came.came
    if now ~= was then
      print(("jetski: came %s%s"):format(tostring(now),
            page.came and page.came.chain > 0 and (", certificate for " .. tostring(page.came.names)
                                                   .. " by " .. tostring(page.came.issuer)) or ""))
      changed = true
    end
  end

  -- The tab shown: its page stopped, or stopped and now collected, or
  -- loading again - whichever it is, the stopped page follows.
  local was = crash
  -- A process not yet collected is asked about again, for a second or two
  -- of passes, and then left as it is.
  if st.crashed or st.started or st.tab ~= page.crash_tab then
    page.crash_asks = 120
  end
  if page.crash_asks > 0 and (st.crashed or st.started or st.tab ~= page.crash_tab
                              or (crash and not crash.known)) then
    page.crash_asks = page.crash_asks - 1
    crash = web.crash(st.tab)
    page.crash_tab = st.tab
  end
  if crash ~= was then
    cover()
    if crash and (not was or crash.known ~= was.known) then
      say("the page's process ended")
      for _, r in ipairs(crash_rows(crash)) do
        print(("jetski: %-9s %s"):format(r[1], r[2]))
      end
    end
    changed = true
  end
  -- The function, the pass after the stopped page first showed.
  if crash and crash.fault and named[crash.pc] == nil then
    if page.naming == crash.pc then
      print("jetski: in        " .. tostring(look_up(crash.pc) or "no symbol holds it"))
      changed = true
    else
      page.naming = crash.pc
    end
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

-- Control-L: the address, all of it chosen, to type over.
win.on_key = function(_, c)
  if c == 12 then
    web.focus(false)
    win:focus_on(field)
    field:set_text(nil, true)
    return true
  end

  if c == 20 then new_tab() return true end

  -- Control or Super with a comma: Settings, as Safari and Chrome have it.
  do
    local key, mods = keys.parts(c)
    if key == 44 and mods & (keys.CTRL | keys.SUPER) ~= 0 then
      open_settings()
      return true
    end
  end

  if inspector_key(c) then toggle_inspector() return true end

  -- Control-O: an HTML file, chosen in the system's Open window.
  if c == 15 then
    local chooser = use("/Kosmos/Libraries/panel.lua").open{
      -- Where the page shown is, when it is a file; /Home otherwise.
      start = (page.address or ""):match("^file://(/.*)/[^/]*$") or "/Home",
      title = "Open a web page",
      filter = function(name) return name:lower():match("%.html?$") ~= nil end,
      on_choose = function(path) win:focus_on(view) web.focus(true) visit(path) end,
    }
    if chooser then chooser:run() end
    return true
  end

  -- Super with a digit: that tab, and 9 the last, as every browser has it.
  local key, mods = keys.parts(c)
  if mods & keys.SUPER ~= 0 and key >= 49 and key <= 57 then
    local t = (key == 57) and tabs[#tabs] or tabs[key - 48]
    if t then select_tab(t.id) end
    return true
  end

  return false
end

-- **A file dropped on the window** - from Tracker, a path a line - opens
-- in the tab shown, the first of them; a web page is one page.
function win:on_drop(kind, payload)
  if kind ~= "files" then return false end
  local first = payload:match("^[^\n]+")
  if not first or not first:lower():match("%.html?$") then
    say("only a web page opens here - an .html file")
    return first ~= nil
  end
  print("jetski: dropped " .. first)
  win:focus_on(view)
  web.focus(true)
  visit(first)
  return true
end

win:add(lockpanel)                   -- over everything, while it is open
win:add(shieldpanel)
tabs_changed()
win:focus_on(view)

do
  local start = files.words(args)[1]
  if not start and setting.opens == "tabs" and #had > 0 then
    -- The tab `web.open` made is the first; the rest wait to be shown.
    local ids = { web.shown_id() }
    for i = 2, #had do ids[i] = web.tab_new() end
    for i, a in ipairs(had) do
      if ids[i] then pending[ids[i]] = a end
    end
    local first = ids[had_shown] or ids[1]
    web.tab_select(first)
    take_pending(first)
    shown_changed()
    print(("jetski: opened on the tabs it had, %d"):format(#had))
  else
    visit(start or HOME)
  end
end
win:run()
save_kept()
