-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki.
-- kosmos: application
-- kosmos: icon App_NetSurf
-- kosmos: name JetSki
-- kosmos: section applications
-- kosmos: needs network
-- kosmos: about WebKit on Kosmos: the web as Safari draws it
-- kosmos: image jetski.elf
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

-- Web Inspector's pages, in the folder beside this file (`tools/inspector.py`).
local HERE = tostring(sys.program or ""):match("^(.*)/[^/]+$") or "/Home/Apps/JetSki"
web.inspector_files(HERE .. "/Inspector")
web.developer(DEVELOPER)

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
local gone                           -- a tab whose page stopped, below

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
  if not inspect.hidden then
    inspect.x = right - inspect.w
    right = inspect.x - ui.layout.head_gap
  end
  field.w = right - field.x
  SPLIT_W = inspecting and math.min(INSPECTOR_W, W // 2) or 0
  PAGE_W = W - SPLIT_W
  view.w, view.h = W, VIEW_H
  if gone then gone.w, gone.h = PAGE_W, VIEW_H end
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
-- and Super with a digit.
local function inspector_key(c)
  local key, mods = keys.parts(c)
  return DEVELOPER and mods & keys.CTRL ~= 0 and mods & keys.ALT ~= 0
         and (key == 73 or key == 105)
end

local function windows_key(c)
  if c == 12 or c == 15 or c == 20 then return true end
  if inspector_key(c) then return true end
  local key, mods = keys.parts(c)
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
local IMAGE = HERE .. "/jetski.elf"
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
    rows[#rows + 1] = { "at", hex(c.pc) .. " in jetski.elf" }
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

-- An address as WebKit wants one: a scheme always, `https` when none was
-- typed; words are a search, as the NetSurf browser's field has them.
local function visit(text)
  -- A path is a file of this machine's: `file:` and the path, which
  -- WebKit's network process reads from /Home with the files beside it.
  if text:match("^%s*/") then text = "file://" .. text:match("^%s*(.-)%s*$") end
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

-- **Web Inspector shown or put away**, for the tab shown. WebKit makes its
-- page and says so (`st.inspector`), and the split follows that rather
-- than this: what is docked is what WebKit says is.
local function toggle_inspector()
  if not DEVELOPER then return end
  print("jetski: " .. (inspecting and "hiding" or "showing") .. " Web Inspector")
  web.inspect(not inspecting)
end
inspect.on_click = toggle_inspector

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

  if crash then
    ui.paint_view(gone, s, 0, VIEW_Y)
  else
    web.paint(s, 0, VIEW_Y, PAGE_W, VIEW_H)
  end

  if inspecting then
    s:fill(PAGE_W, VIEW_Y, 1, VIEW_H, theme.line)
    web.paint_inspector(s, PAGE_W + 1, VIEW_Y, SPLIT_W - 1, VIEW_H)
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
    gone.hidden = crash == nil
    view.hidden = crash ~= nil
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

-- Control-L: the address, to type over.
win.on_key = function(_, c)
  if c == 12 then
    web.focus(false)
    win:focus_on(field)
    return true
  end

  if c == 20 then new_tab() return true end

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

tabs_changed()
win:focus_on(view)
visit(files.words(args)[1] or HOME)
win:run()
