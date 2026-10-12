-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Packages
-- kosmos: section system
-- kosmos: needs network
-- Every package the server offers: what is installed, what has another
-- version, what is not installed yet - and Install, Update and Remove.
--
--   wm packages
--
-- Diego, 12 October 2026: "where we can see all installed packages, remove
-- them, install new ones, update packages to new versions", "and packages
-- might have dependencies?", "like an APT package system?". Drawn in
-- `docs/packages.html` and agreed; `docs/packages.md` is what it is made of,
-- and this is step P3.
--
-- **It shows and asks; `packages.lua` does the rest.** The index is read in
-- a coroutine stepped between the window's passes, so the window draws while
-- the server answers. Install, Update and Remove are `pkg` in a process of
-- its own, started through the window manager and not waited for: it writes
-- what it says into a report file (`--report`), which this reads on its own
-- clock - nothing on the desktop waits on a server, and a package that takes
-- seconds to unpack and check never holds the window still.

local ui = use("/Kosmos/Libraries/ui.lua")
local http = use("/Kosmos/Libraries/http.lua")
local files = use("/Kosmos/Libraries/files.lua")
local packages = use("/Kosmos/Libraries/packages.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil
local FROM = (tostring(args or "")):match("%-%-from%s+(%S+)")

local W, H = 1080, 640
local SIDE_W, DETAIL_W = 200, 340
local ROW_H = 74
local TILE = 44

local server = FROM or packages.server()
local index, by_name, looked_at, problem = nil, {}, nil, nil
local filter, chosen, top_row = "all", nil, 0
local running = nil                 -- { what, name, report, started }
local last_said = nil
local asking = nil                  -- a question in the details: { title, lines, verb, names, go }

--------------------------------------------------------------------------
-- The index, fetched without holding the window.
--------------------------------------------------------------------------

local fetching = nil
local state

local function fetch_index()
  if not server or fetching then return end

  fetching = coroutine.create(function()
    local reply, why = http.get("http://" .. server .. "/index.txt",
                                { pause = function(conn) coroutine.yield(conn) end })

    if not reply then return nil, tostring(why) end

    local status, _, body = http.parse(reply)

    if status ~= 200 then return nil, ("the server said %d"):format(status) end

    return body
  end)
end

local function step_fetch()
  if not fetching then return false end

  local ok, a, b = coroutine.resume(fetching)

  if coroutine.status(fetching) ~= "dead" then return false end

  fetching = nil

  if not ok or not a then
    problem = tostring(ok and b or a)
    print("packages: the server did not answer: " .. problem)
    return true
  end

  index, by_name, problem = packages.parse(a), {}, nil
  looked_at = sys.ticks()

  for _, p in ipairs(index) do by_name[p.name] = p end

  if not chosen or not by_name[chosen] then chosen = index[1] and index[1].name end

  if TELL then
    local states = {}

    for _, p in ipairs(index) do
      local s, m = state(p)

      states[#states + 1] = p.name .. "=" .. s .. (m and (" " .. m.version) or "")
    end

    print(("packages: listed %d from %s"):format(#index, server))
    print("packages: states " .. table.concat(states, ", "))
  end

  return true
end

--------------------------------------------------------------------------
-- What each package is here.
--------------------------------------------------------------------------

function state(p)
  local m = packages.installed(p.name)

  if not m then return "absent", nil end
  if packages.current(p) then return "installed", m end

  return "update", m
end

local function counts()
  local c = { all = 0, installed = 0, update = 0, absent = 0, kinds = {} }

  for _, p in ipairs(index or {}) do
    local s = state(p)

    c.all = c.all + 1
    c[s] = c[s] + 1
    if s == "update" then c.installed = c.installed + 1 end
    c.kinds[p.kind or "other"] = (c.kinds[p.kind or "other"] or 0) + 1
  end

  return c
end

local function shown()
  local out = {}

  for _, p in ipairs(index or {}) do
    local s = state(p)
    local keep = filter == "all" or (filter == "installed" and s ~= "absent")
                 or filter == s or filter == "kind:" .. (p.kind or "other")

    if keep then out[#out + 1] = p end
  end

  return out
end

--------------------------------------------------------------------------
-- An action: `pkg` in a process of its own, followed by its report.
--------------------------------------------------------------------------

local function run(verb, names)
  if running then return end

  local report = ("/Temporary/packages-%d.txt"):format(sys.ticks() % 1000000007)
  local words = verb .. (names and #names > 0 and (" " .. table.concat(names, " ")) or "")
              .. (FROM and (" --from " .. FROM) or "") .. " --report " .. report

  fs.write(report, "")
  local reply, why = fs.send("/Running/wm", { type = "launch", program = "pkg", args = words, wait = false })

  if not reply or not reply.ok then
    last_said = "pkg would not start: " .. tostring(why or (reply and reply.error))
    return
  end

  running = { verb = verb, names = names or {}, report = report, started = sys.ticks() }
  last_said = nil
  print(("packages: running pkg %s"):format(words:gsub(" %-%-report .*$", "")))
end

local function follow()
  if not running then return false end

  local text = fs.read(running.report)

  if type(text) ~= "string" or text == "" then return false end

  local lines = {}

  for l in text:gmatch("[^\n]+") do lines[#lines + 1] = l end

  local final = lines[#lines]

  if final == "done ok" or final == "done failed" then
    last_said = lines[#lines - 1] or final
    print(("packages: %s - %s"):format(final, tostring(last_said)))
    files.remove(running.report)
    running = nil
    return true
  end

  last_said = final
  return true
end

-- Install, with what it depends on asked about first.
local function ask_install(p)
  local plan, why = packages.plan({ p.name }, by_name)

  if not plan then last_said = why return end

  if #plan > 1 then
    local lines, total = {}, 0

    for _, q in ipairs(plan) do
      lines[#lines + 1] = ("%s - %s"):format(q.title, files.size(q.zip))
      total = total + q.zip
    end

    asking = { title = "Install " .. p.title .. "?",
               note = ("It depends on what is not installed, which is installed first - %s in all."):format(files.size(total)),
               lines = lines, go = "Install " .. #plan, verb = "install", names = { p.name } }
  else
    run("install", { p.name })
  end
end

local function ask_remove(p)
  local needs = packages.needed_by(p.name)

  if #needs > 0 then
    local names = { p.name }

    for _, n in ipairs(needs) do names[#names + 1] = n end

    asking = { title = "Remove " .. p.title .. "?",
               note = ("%s needs it. Removing it alone would leave them unable to start, so they go together."):format(table.concat(needs, ", ")),
               lines = names, go = "Remove " .. #names, verb = "remove", names = names }
  else
    asking = { title = "Remove " .. p.title .. "?",
               note = "Only the files it installed go; anything you put beside them stays.",
               lines = { p.title .. " - " .. tostring(p.into) }, go = "Remove", verb = "remove", names = { p.name } }
  end
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local win, err = ui.window{ title = "Packages", w = W, h = H, x = 120, y = 80, header = true }

if not win then
  print("packages: " .. tostring(err))
  return
end

local update_all = ui.button{ text = "Update all", go = true }

update_all.on_click = function()
  if not running and counts().update > 0 then run("update") end
end

local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  local m = win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "Look again", on_choose = function() fetch_index() end },
  })

  if m and TELL then print("packages: more menu") end
end

local head = ui.header{ x = 0, y = 0, w = W, title = "Packages", right = { update_all, more }, title_bar = true }

win:add(head)

local BODY_H = H - L.head
local LIST_X, LIST_W = SIDE_W, W - SIDE_W - DETAIL_W
local DETAIL_X = W - DETAIL_W

--
-- **The sidebar**: what to look at.
--
local FILTERS = { { "all", "All" }, { "installed", "Installed" }, { "update", "Updates" }, { "absent", "Not installed" } }

local side = ui.view{ x = 0, y = L.head, w = SIDE_W, h = BODY_H }

local function side_rows()
  local rows, c = {}, counts()

  for _, f in ipairs(FILTERS) do rows[#rows + 1] = { key = f[1], text = f[2], n = c[f[1]] } end

  rows[#rows + 1] = { group = "Kinds" }

  local kinds = {}

  for k in pairs(c.kinds) do kinds[#kinds + 1] = k end
  table.sort(kinds)

  for _, k in ipairs(kinds) do
    rows[#rows + 1] = { key = "kind:" .. k, text = k:sub(1, 1):upper() .. k:sub(2), n = c.kinds[k] }
  end

  return rows
end

function side:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)

  local y = 12

  for _, r in ipairs(side_rows()) do
    if r.group then
      y = y + 10
      g:text(18, y, r.group:upper(), theme.text_dim, theme.sunken, "label")
      y = y + 24
    else
      if r.key == filter then g:fill_round(8, y, self.w - 16, 30, theme.raised, 7) end

      g:text(18, y + (30 - gfx.height()) // 2, r.text, theme.text, theme.sunken, "ui")

      local n = tostring(r.n or 0)

      g:text(self.w - 18 - gfx.measure(n), y + (30 - gfx.height()) // 2, n,
             (r.key == "update" and (r.n or 0) > 0) and theme.io_out or theme.text_dim, theme.sunken, "ui")
      r.y = y
      y = y + 32
    end
  end
end

function side:mouse(action, x, y)
  return ui.click(self, action, x, y, function(_, py)
    local at = 12

    for _, r in ipairs(side_rows()) do
      if r.group then
        at = at + 34
      else
        if py >= at and py < at + 30 then
          return r.key, function() filter, top_row = r.key, 0 print("packages: showing " .. r.key) end
        end

        at = at + 32
      end
    end
  end)
end

win:add(side)

--
-- **The list**: each package, its state, and its button.
--
local list = ui.view{ x = LIST_X, y = L.head, w = LIST_W, h = BODY_H - 34 }
local KIND_COLOUR = { game = 0xff7a3b8f, emulator = 0xff2f7a6b, developer = 0xff3b5a8f }

local function tile(g, x, y, size, p)
  g:fill_round(x, y, size, size, KIND_COLOUR[p.kind] or 0xff4a5468, size // 5)

  local letter = (p.title or p.name):sub(1, 1):upper()
  local face = ui.sized("title", size // 2)

  g:text(x + (size - gfx.measure(letter, face)) // 2, y + (size - gfx.height(face)) // 2,
         letter, 0xffffffff, KIND_COLOUR[p.kind] or 0xff4a5468, face)
end

local function button_box(row_y)
  return LIST_W - 16 - 84, row_y + (ROW_H - 28) // 2, 84, 28
end

function list:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  if not index then
    g:text(16, 16, problem and ("The package server did not answer: " .. problem)
                   or ("Asking " .. tostring(server or "no server") .. "..."), theme.text_dim, theme.window, "ui")
    return
  end

  local rows = shown()

  for i, p in ipairs(rows) do
    local y = (i - 1) * ROW_H - top_row

    if y > -ROW_H and y < self.h then
      local s, m = state(p)

      if p.name == chosen then g:fill(0, y, self.w, ROW_H, theme.raised) end

      tile(g, 16, y + (ROW_H - TILE) // 2, TILE, p)

      local tx, room = 16 + TILE + 12, self.w - 16 - TILE - 12 - 110

      g:text(tx, y + 12, ui.fitted(p.title, room, "heading"), theme.text, theme.window, "heading")
      g:text(tx, y + 12 + gfx.height("heading"), ui.fitted(p.about or "", room, "ui"), theme.text_dim, theme.window, "ui")

      local meta = files.size(p.zip) .. " \u{b7} " .. (p.version or "")

      if s == "update" then
        meta = ("%s \u{b7} %s installed, %s here"):format(files.size(p.zip), m.version or "?", p.version or "?")
      end
      if p.depends ~= "" then meta = meta .. " \u{b7} depends on " .. p.depends end

      g:text(tx, y + 12 + gfx.height("heading") + gfx.height("ui"), ui.fitted(meta, room, "label"),
             theme.text_dim, theme.window, "label")

      local bx, by, bw, bh = button_box(y)
      local busy = running and ((running.verb == "update" and s == "update")
                                or (" " .. table.concat(running.names, " ") .. " "):find(" " .. p.name .. " ", 1, true))

      if busy then
        g:text(bx, by + (bh - gfx.height()) // 2, "Working...", theme.text_dim, theme.window, "ui")
      elseif s == "installed" then
        g:text(bx + bw - gfx.measure("\u{2713} Installed"), by + (bh - gfx.height()) // 2,
               "\u{2713} Installed", theme.good, theme.window, "ui")
      else
        local word = s == "update" and "Update" or "Install"

        g:fill_round(bx, by, bw, bh, s == "update" and theme.raised or theme.accent, 7)
        g:frame_round(bx, by, bw, bh, theme.line, 7)
        g:text(bx + (bw - gfx.measure(word)) // 2, by + (bh - gfx.height()) // 2, word,
               s == "update" and theme.text or 0xffffffff, s == "update" and theme.raised or theme.accent, "ui")
      end

      g:fill(0, y + ROW_H - 1, self.w, 1, theme.line_soft)
    end
  end
end

function list:wheel(n)
  local most = math.max(0, #shown() * ROW_H - self.h)

  top_row = math.max(0, math.min(most, top_row + n * ROW_H))
  return true
end

function list:mouse(action, x, y)
  return ui.click(self, action, x, y, function(px, py)
    local rows = shown()
    local i = (py + top_row) // ROW_H + 1
    local p = rows[i]

    if not p then return nil end

    local bx, by, bw, bh = button_box((i - 1) * ROW_H - top_row)

    if px >= bx and px < bx + bw and py >= by and py < by + bh and not running then
      local s = state(p)

      if s ~= "installed" then
        return "button " .. p.name, function()
          chosen = p.name
          print(("packages: pressed %s on %s"):format(s == "update" and "Update" or "Install", p.name))
          if s == "update" then run("install", { p.name }) else ask_install(p) end
        end
      end
    end

    return "row " .. p.name, function()
      chosen, asking = p.name, nil
      print("packages: chose " .. p.name)
    end
  end)
end

win:add(list)

--
-- **The details**: the chosen package's manifest, its actions, and any
-- question being asked about them.
--
local detail = ui.view{ x = DETAIL_X, y = L.head, w = DETAIL_W, h = BODY_H - 34 }
local detail_buttons = {}

function detail:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, 0, 1, self.h, theme.line_soft)
  detail_buttons = {}

  local p = chosen and by_name[chosen]

  if not p then return end

  local s, m = state(p)
  local x, y = 18, 18

  tile(g, x, y, 64, p)
  g:text(x + 78, y + 8, ui.fitted(p.title, self.w - x - 96, "title"), theme.text, theme.window, "title")
  g:text(x + 78, y + 8 + gfx.height("title"), (p.kind or "package"), theme.text_dim, theme.window, "ui")
  y = y + 82

  for _, line in ipairs(ui.wrapped(p.about or "", self.w - 36, "ui", 3)) do
    g:text(x, y, line, theme.text, theme.window, "ui")
    y = y + gfx.height("ui")
  end

  y = y + 10

  local function button(text, go, act)
    local bw = gfx.measure(text) + 28

    g:fill_round(x, y, bw, 30, go and theme.accent or theme.raised, 7)
    g:frame_round(x, y, bw, 30, theme.line, 7)
    g:text(x + 14, y + (30 - gfx.height()) // 2, text, go and 0xffffffff or theme.text,
           go and theme.accent or theme.raised, "ui")
    detail_buttons[#detail_buttons + 1] = { x = x, y = y, w = bw, h = 30, text = text, act = act }
    x = x + bw + 8
  end

  if asking then
    g:text(x, y, asking.title, theme.text, theme.window, "heading")
    y = y + gfx.height("heading") + 4

    for _, line in ipairs(ui.wrapped(asking.note, self.w - 36, "ui", 4)) do
      g:text(x, y, line, theme.text_dim, theme.window, "ui")
      y = y + gfx.height("ui")
    end

    for _, line in ipairs(asking.lines) do
      g:text(x + 8, y + 2, "\u{2022} " .. line, theme.text, theme.window, "ui")
      y = y + gfx.height("ui") + 2
    end

    y = y + 10
    button("Cancel", false, function() asking = nil end)
    button(asking.go, true, function()
      local a = asking

      asking = nil
      run(a.verb, a.names)
    end)
    return
  end

  if not running then
    if s == "absent" then
      button("Install", true, function() ask_install(p) end)
    elseif s == "update" then
      button("Update to " .. p.version, true, function() run("install", { p.name }) end)
    end

    if s ~= "absent" then button("Remove", false, function() ask_remove(p) end) end
  else
    g:text(x, y + 6, "Working...", theme.text_dim, theme.window, "ui")
  end

  x, y = 18, y + 46

  local facts = {
    { "Installed", m and (m.version .. (m.into and (", in " .. m.into) or "")) or "no" },
    { "Here", (p.version or "") .. " - " .. ((sys.protostamp and p.protocols == sys.protostamp)
                                                and "for this system's protocols" or "for other protocols") },
    { "Size", files.size(p.bytes) .. (m and m.files and #m.files > 0 and (", " .. #m.files .. " files") or "") },
    { "Needs", p.needs ~= "" and p.needs or "nothing asked for" },
    { "Depends on", p.depends ~= "" and p.depends or "nothing else" },
  }
  local needed = packages.needed_by(p.name)

  facts[#facts + 1] = { "Needed by", #needed > 0 and table.concat(needed, ", ") or "nothing installed" }

  for _, f in ipairs(facts) do
    g:text(x, y, f[1], theme.text_dim, theme.window, "label")
    g:text(x + 100, y, ui.fitted(f[2], self.w - x - 118, "ui"), theme.text, theme.window, "ui")
    y = y + gfx.height("ui") + 6
  end
end

-- For its suite: where the details' buttons are, each time they change.
local told_buttons = nil

local function tell_buttons()
  local said = {}

  for _, b in ipairs(detail_buttons) do
    said[#said + 1] = ("%s@%d,%d"):format(b.text, DETAIL_X + b.x + b.w // 2, L.head + b.y + b.h // 2)
  end

  local line = tostring(chosen) .. ": " .. table.concat(said, " | ")

  if line ~= told_buttons then
    told_buttons = line
    print("packages: buttons " .. line)
  end
end

function detail:mouse(action, x, y)
  return ui.click(self, action, x, y, function(px, py)
    for _, b in ipairs(detail_buttons) do
      if px >= b.x and px < b.x + b.w and py >= b.y and py < b.y + b.h then
        return "button " .. b.text, function()
          print("packages: pressed " .. b.text)
          b.act()
        end
      end
    end
  end)
end

local draw_detail = detail.draw

function detail:draw(g)
  draw_detail(self, g)
  if TELL then tell_buttons() end
end

win:add(detail)

--
-- **The foot**: how many, and what the last action said.
--
local foot = ui.view{ x = SIDE_W, y = L.head + BODY_H - 34, w = W - SIDE_W, h = 34 }

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local c = counts()
  local left = index and ("%d packages \u{b7} %d installed, %d with updates"):format(c.all, c.installed, c.update)
               or "no packages yet"

  if looked_at then
    left = left .. (" \u{b7} from %s, looked %d s ago"):format(server, (sys.ticks() - looked_at) // counter_hz)
  end

  g:text(16, (self.h - gfx.height()) // 2, left, theme.text_dim, theme.window, "ui")

  local right = last_said and ui.fitted(last_said, self.w // 2, "ui") or "pkg, at the prompt, does all of this too"

  g:text(self.w - 16 - gfx.measure(right), (self.h - gfx.height()) // 2, right, theme.text_dim, theme.window, "ui")

  if TELL and not self.told and more.x then
    print(("packages: window at %d,%d; list at %d,%d, %d wide, rows of %d")
          :format(win.origin_x or 0, win.origin_y or 0, LIST_X, L.head, LIST_W, ROW_H))
    self.told = true
  end
end

win:add(foot)

--------------------------------------------------------------------------
-- The clock: the index, the action being followed, and the button's count.
--------------------------------------------------------------------------

local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }
local was_running = false

function clock:tick()
  step_fetch()
  follow()

  -- An action finished: the index again, since what is installed changed.
  if was_running and not running then fetch_index() end
  was_running = running ~= nil

  local n = counts().update

  update_all.text = n > 0 and ("Update all (%d)"):format(n) or "Update all"
end

win:add(clock)

if not server then
  problem = "the boot names no package server (opt/kosmos/packages)"
end

fetch_index()
win.tick_every = counter_hz // 4
win:run()
