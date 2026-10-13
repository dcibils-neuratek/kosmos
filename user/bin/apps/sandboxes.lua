-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Sandboxes
-- kosmos: section system
-- Small operating systems - Linux first - started for a task or kept as a
-- service, each given only what is chosen for it.
--
--   wm sandboxes
--
-- `docs/sandboxes.md` step S2, drawn in `docs/sandboxes.html` and agreed on
-- 11 October 2026 ("sandbox mockup is perfect"); Diego, 12 October:
-- "continue with the sandbox app", "as designed in the mockup".
--
-- **It shows and asks; `sandboxes.lua` does the rest**, as it does for the
-- `sandbox` program. What it shows it asks of each sandbox's monitor twice
-- a second (`vmmproto.h`, one message each). Starting one reads its image's
-- kernel into a region, which takes seconds, so the window does not do it:
-- `sandbox start` does, in a process of its own, and the window sees it
-- arrive in `/Running` - nothing on the desktop waits on that.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local sandboxes = use("/Kosmos/Libraries/sandboxes.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil

local W, H = 1180, 760
local SIDE = 220
local CARD_W, CARD_H, GAP = 0, 196, 14          -- CARD_W follows the window
local MB = 1024 * 1024

local GREEN, BLUE, GREY = 0xff2e9e5b, 0xff2f6fdc, 0xff6b7280

--------------------------------------------------------------------------
-- What there is, asked on the window's own clock.
--------------------------------------------------------------------------

local all, images, machine = {}, {}, {}
local before, busy, history = {}, {}, {}
local group, chosen, watched, tab = "all", nil, nil, 1
local console = {}                      -- name -> { at, text }
local said = nil                        -- the foot's line
local pending = {}                      -- name -> "starting" | "restarting"
local last_full = 0
local start                             -- forward: a restart calls it
local folder_words                      -- forward: the cards say folders
local reports = {}                      -- name -> the file its start says in

local function refresh(full)
  local now = sys.ticks()

  if full or now - last_full > counter_hz * 2 then
    images = sandboxes.images()
    last_full = now

    local info = sys.info() or {}

    machine = { cpus = info.cpus or 1, ram = info.ram_size or 0, guests = info.guests == 1,
                virt = info.virt or 0 }
  end

  all = sandboxes.all()

  for _, s in ipairs(all) do
    if s.now then
      local was = before[s.name]

      busy[s.name] = sandboxes.busy(was, s.now)
      before[s.name] = s.now
      history[s.name] = history[s.name] or { cpu = {}, net = {} }
      ui.history.push(history[s.name].cpu, { math.floor(busy[s.name] * 100 + 0.5) }, 60)

      -- Its network, in bytes a second each way, over the time between asks.
      if was and s.now.up_ticks > was.up_ticks then
        local secs = (s.now.up_ticks - was.up_ticks) / counter_hz

        ui.history.push(history[s.name].net, { math.floor((s.now.net_in - was.net_in) / secs),
                                               math.floor((s.now.net_out - was.net_out) / secs) }, 60)
      end
    else
      busy[s.name], before[s.name] = nil, nil
    end

    -- A start that failed says so in its report: no longer "starting".
    if pending[s.name] == "starting" and reports[s.name] then
      local line = fs.read(reports[s.name])

      if type(line) == "string" and line:match("^failed: ") then
        pending[s.name] = nil
        said = ("could not start %s: %s"):format(s.name, line:gsub("^failed: ", ""):gsub("\n$", ""))
        print("sandboxes: " .. said)
      end

      if type(line) == "string" and line ~= "" then
        files.remove(reports[s.name])
        reports[s.name] = nil
      end
    end

    -- A restart: once it has gone, started again.
    if pending[s.name] == "restarting" and not s.now then
      pending[s.name] = nil
      start(s.name)
    elseif pending[s.name] == "starting" and s.now then
      pending[s.name] = nil
      said = s.name .. " started"
    end
  end

  if watched then
    local c = console[watched] or { at = 0, text = "" }
    local text, next_at = sandboxes.read(watched, c.at)

    if text and #text > 0 then
      c.text = (c.text .. text):sub(-24000)
      c.at = next_at

      -- For its suite: each line of what arrived, as it arrives.
      if TELL then
        for line in text:gmatch("[^\n]+") do
          print("sandboxes: console " .. watched .. ": " .. line)
        end
      end
    end

    console[watched] = c
  end
end

local function state_of(s)
  if pending[s.name] then return pending[s.name] end
  if not s.now then return "stopped" end
  if not s.now.running then return "ended" end

  return s.kind == "task" or s.task and "task" or "running"
end

local function shown()
  local out = {}

  for _, s in ipairs(all) do
    local st = state_of(s)
    local keep = group == "all" or (group == "running" and (st == "running" or st == "task"))
                 or (group == "stopped" and (st == "stopped" or st == "ended"))
                 or (group == "tasks" and (s.kind == "task" or s.task))

    if keep then out[#out + 1] = s end
  end

  return out
end

local function counts()
  local c = { all = #all, running = 0, stopped = 0, tasks = 0 }

  for _, s in ipairs(all) do
    local st = state_of(s)

    if st == "running" or st == "task" then c.running = c.running + 1 else c.stopped = c.stopped + 1 end
    if s.kind == "task" or s.task then c.tasks = c.tasks + 1 end
  end

  return c
end

local function given_mb()
  local mb = 0

  for _, s in ipairs(all) do
    if s.now then mb = mb + (s.now.ram or 0) // MB end
  end

  return mb
end

--------------------------------------------------------------------------
-- What a person does.
--------------------------------------------------------------------------

-- `sandbox start`, in a process of its own: it reads the image's kernel,
-- which takes seconds, and the window carries on drawing meanwhile.
function start(name)
  local report = ("/Temporary/sandbox-start-%s.txt"):format(name)

  fs.write(report, "")
  reports[name] = report

  local reply, why = fs.send("/Running/wm", { type = "launch", program = "sandbox",
                                              args = "start " .. name .. " --report " .. report,
                                              wait = false })

  if not reply or not reply.ok then
    said = "could not start " .. name .. ": " .. tostring(why or (reply and reply.error))
    return
  end

  pending[name] = "starting"
  said = "starting " .. name .. "..."
  print("sandboxes: starting " .. name)
end

local function stop(name)
  sandboxes.stop(name)
  said = "stopping " .. name
  print("sandboxes: stopping " .. name)
end

local function restart(name)
  sandboxes.stop(name)
  pending[name] = "restarting"
  said = "restarting " .. name
end

local function delete(name)
  local ok, why = sandboxes.delete(name)

  said = ok and (name .. " deleted") or tostring(why)
  if chosen == name then chosen = nil end
  if watched == name then watched = nil end
  refresh(true)
end

local function open(name, which)
  watched, tab = name, which or 1
  console[name] = console[name] or { at = 0, text = "" }
  print("sandboxes: watching " .. name .. ", tab " .. tab)
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local win, err = ui.window{ title = "Sandboxes", w = W, h = H, x = 100, y = 60, header = true }

if not win then
  print("sandboxes: " .. tostring(err))
  return
end

local new_button = ui.button{ text = "+ New sandbox", go = true }
local head = ui.header{ x = 0, y = 0, w = W, title = "Sandboxes", sub = "", right = { new_button },
                        title_bar = true }

win:add(head)

--
-- **The sidebar**: the groups, the images there are, and the machine.
--
local side = ui.view{ x = 0, y = L.head, w = SIDE, h = H - L.head, follow = { "left", "top", "bottom" } }
local side_rows = {}

local function draw_group(g, y, title)
  g:text(18, y, title:upper(), theme.text_dim, nil, "label")
  return y + gfx.height("label") + 8
end

function side:draw(g)
  local c = counts()
  local y = 14

  side_rows = {}
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)

  y = draw_group(g, y, "Sandboxes")

  for _, it in ipairs({ { "all", "All" }, { "running", "Running" }, { "stopped", "Stopped" }, { "tasks", "Tasks" } }) do
    local on = group == it[1]

    if on then g:fill_round(8, y, self.w - 16, 30, theme.accent, 7) end

    local ink = on and 0xffffffff or theme.text

    g:text(20, y + (30 - gfx.height()) // 2, it[2], ink, nil, "ui")

    local n = tostring(c[it[1]] or 0)

    g:text(self.w - 22 - gfx.measure(n), y + (30 - gfx.height()) // 2, n,
           on and 0xffffffff or theme.text_dim, nil, "ui")
    side_rows[#side_rows + 1] = { y = y, h = 30, group = it[1] }
    y = y + 32
  end

  y = draw_group(g, y + 14, "Images")

  if #images == 0 then
    g:text(20, y, "none yet", theme.text_dim, nil, "ui")
    y = y + gfx.height() + 6
  end

  for _, i in ipairs(images) do
    g:text(20, y, ui.fitted(i.title, self.w - 90, "ui"), theme.text, nil, "ui")

    local size = ("%d MB"):format(i.bytes // MB)

    g:text(self.w - 22 - gfx.measure(size, "label"), y + 2, size, theme.text_dim, nil, "label")
    y = y + gfx.height() + 8
  end

  g:text(20, y, "+ Get an image", theme.accent, nil, "ui")
  side_rows[#side_rows + 1] = { y = y - 4, h = gfx.height() + 8, get_image = true }
  y = y + gfx.height() + 22

  y = draw_group(g, y, "Machine")
  g:text(20, y, ("%d processor%s"):format(machine.cpus or 1, (machine.cpus or 1) == 1 and "" or "s"),
         theme.text_dim, nil, "ui")
  y = y + gfx.height() + 4
  g:text(20, y, ("%d MB of %d MB given"):format(given_mb(), (machine.ram or 0) // MB),
         theme.text_dim, nil, "ui")
  y = y + gfx.height() + 4

  -- What the processor and its firmware allow (`sysinfo.virt`, probed at
  -- boot): on a PC, whether VT-x is there and what its setup decided.
  local virt = machine.guests and "Virtualization: EL2, a guest a core"
               or ({ [1] = "VT-x is off in this machine's setup: turn on Intel Virtualization Technology",
                     [2] = "VT-x is on; Kosmos does not drive it yet",
                     [3] = "VT-x is here and not locked; Kosmos does not drive it yet" })[machine.virt]
               or "This machine cannot run sandboxes yet"

  for _, line in ipairs(ui.wrapped(virt, self.w - 36, "ui", 3)) do
    g:text(20, y, line, machine.guests and theme.text_dim or theme.io_out, nil, "ui")
    y = y + gfx.height() + 2
  end
end

function side:mouse(action, x, y)
  return ui.click(self, action, x, y, function(_, py)
    for _, r in ipairs(side_rows) do
      if py >= r.y and py < r.y + r.h then
        if r.get_image then
          return "get", function()
            fs.send("/Running/wm", { type = "launch", program = "packages", wait = false })
            print("sandboxes: get an image")
          end
        end

        return r.group, function()
          group, watched = r.group, nil
          print("sandboxes: showing " .. r.group)
        end
      end
    end
  end)
end

win:add(side)

--
-- **The cards**: every sandbox in the group, two to a row.
--
local grid = ui.view{ x = SIDE, y = L.head, w = W - SIDE, h = H - L.head - 34,
                      follow = { "left", "right", "top", "bottom" } }
local buttons = {}                      -- placed by the last draw
local top = 0

local function pill(g, x, y, text, ink)
  local w = gfx.measure(text, "label") + 16

  g:fill_round(x - w, y, w, 22, theme.mix(theme.window, ink, 200), 11)
  g:text(x - w + 8, y + (22 - gfx.height("label")) // 2, text, ink, nil, "label")
end

local function bar(g, x, y, w, share, ink)
  g:fill_round(x, y, w, 4, theme.line_soft, 2)

  if share > 0 then g:fill_round(x, y, math.max(4, math.floor(w * math.min(share, 1))), 4, ink, 2) end
end

local function button(g, x, y, text, act, primary)
  local w = gfx.measure(text) + 24

  g:fill_round(x, y, w, 28, primary and theme.accent or theme.raised, 7)
  g:frame_round(x, y, w, 28, theme.line, 7)
  g:text(x + 12, y + (28 - gfx.height()) // 2, text, primary and 0xffffffff or theme.text, nil, "ui")
  buttons[#buttons + 1] = { x = x, y = y, w = w, h = 28, text = text, act = act }
  return x + w + 8
end

local function chip(g, x, y, text)
  local w = gfx.measure(text, "label") + 18

  g:fill_round(x, y, w, 24, theme.raised, 6)
  g:text(x + 9, y + (24 - gfx.height("label")) // 2, text, theme.text, nil, "label")
  return x + w + 8
end

-- "/Home/www /srv ro" as the drawing says it: "/Home/www -> /srv, read only".
function folder_words(text)
  local f = sandboxes.folder(text)

  if not f then return tostring(text) end

  return ("%s \u{2192} %s%s"):format(f.host, f.at, f.read_only and ", read only" or "")
end

-- "8081:80" as the drawing says it: "8081 -> 80", Kosmos's port and the guest's.
local function port_words(text)
  local from, to = sandboxes.port(text)

  return from and ("%d \u{2192} %d"):format(from, to) or tostring(text)
end

-- Its network in a few words: what it may reach, and what reaches it.
local function network_words(s)
  local ports = #(s.ports or {}) > 0

  if s.network == "outgoing" then return ports and "outgoing, and ports in" or "outgoing only" end

  return ports and "ports in only" or "none"
end

local function up_for(s)
  if not s.now then return nil end

  local secs = s.now.up_ticks // counter_hz

  if secs < 60 then return secs .. " s" end
  if secs < 3600 then return (secs // 60) .. " min" end

  return ("%d h %d min"):format(secs // 3600, secs % 3600 // 60)
end

local function subtitle(s)
  local image = s.task and "a task" or s.image
  local what = s.command ~= "" and s.command or (s.task and "" or "a shell")
  local up = up_for(s)
  local parts = { image }

  if what ~= "" then parts[#parts + 1] = what end
  if up then parts[#parts + 1] = "up " .. up end

  return table.concat(parts, " \u{b7} ")
end

local function draw_card(g, s, x, y, w)
  local st = state_of(s)
  local on = s.name == chosen

  g:fill_round(x, y, w, CARD_H, theme.raised, 12)
  g:frame_round(x, y, w, CARD_H, on and theme.accent or theme.line_soft, 12)

  if on then g:frame_round(x + 1, y + 1, w - 2, CARD_H - 2, theme.accent, 11) end

  local ix = x + 18

  g:fill_round(ix, y + 18, 36, 36, 0xff3b5a8f, 8)
  g:text(ix + (36 - gfx.measure("S", "title")) // 2, y + 18 + (36 - gfx.height("title")) // 2,
         "S", 0xffffffff, nil, "title")

  local tx, room = ix + 50, w - 50 - 18 - 110

  g:text(tx, y + 16, ui.fitted(s.name, room, "heading"), theme.text, nil, "heading")
  g:text(tx, y + 16 + gfx.height("heading") + 2, ui.fitted(subtitle(s), room, "label"),
         theme.text_dim, nil, "label")

  local pills = { running = { "Running", GREEN }, task = { "Task", BLUE }, stopped = { "Stopped", GREY },
                  ended = { "Ended", GREY }, starting = { "Starting", BLUE }, restarting = { "Restarting", BLUE } }
  local p = pills[st] or pills.stopped

  pill(g, x + w - 18, y + 22, p[1], p[2])

  -- Processors and memory, as used now: the guest's share of the time its
  -- monitor has run, and the memory it was given (what it *uses* needs the
  -- guest to say, which is later).
  local my, half = y + 70, (w - 36 - 20) // 2
  local cpu = busy[s.name]

  g:text(ix, my, "Processors", theme.text_dim, nil, "label")
  local cpu_text = cpu and ("%d%%"):format(math.floor(cpu * 100 + 0.5)) or "\u{2013}"
  g:text(ix + half - gfx.measure(cpu_text, "label"), my, cpu_text, theme.text, nil, "label")
  bar(g, ix, my + 20, half, cpu or 0, BLUE)

  local mx = ix + half + 20
  local mem_text = s.now and ("%d MB given"):format(s.now.ram // MB) or ("%d MB"):format(s.memory)

  g:text(mx, my, "Memory", theme.text_dim, nil, "label")
  g:text(mx + half - gfx.measure(mem_text, "label"), my, mem_text, theme.text, nil, "label")
  bar(g, mx, my + 20, half, s.now and machine.ram > 0 and s.now.ram / machine.ram or 0, BLUE)

  -- What it was given.
  local cx = ix

  for _, f in ipairs(s.folders or {}) do cx = chip(g, cx, y + 104, folder_words(f)) end
  for _, p2 in ipairs(s.ports or {}) do cx = chip(g, cx, y + 104, port_words(p2)) end

  if s.network == "outgoing" then
    chip(g, cx, y + 104, #(s.ports or {}) > 0 and "outgoing" or "outgoing only")
  elseif #(s.ports or {}) == 0 then
    chip(g, cx, y + 104, "no network")
  end

  local bx, by = ix, y + CARD_H - 46

  if st == "running" or st == "task" then
    bx = button(g, bx, by, "Console", function() open(s.name, 2) end)
    bx = button(g, bx, by, "Stop", function() stop(s.name) end)

    if not s.task then button(g, bx, by, "Restart", function() restart(s.name) end) end
  elseif st == "stopped" or st == "ended" then
    if not s.task then
      bx = button(g, bx, by, "Start", function() start(s.name) end, true)
      button(g, bx, by, "Delete", function() delete(s.name) end)
    end
  end
end

function grid:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  buttons = {}

  if watched then return end

  local rows = shown()

  if #rows == 0 then
    local line = #all == 0 and "No sandboxes yet - + New sandbox makes one, or sandbox run alpine at the prompt"
                 or "None here"

    g:text(24, 24, line, theme.text_dim, nil, "ui")
    return
  end

  CARD_W = (self.w - 3 * GAP) // 2

  for i, s in ipairs(rows) do
    local col, row = (i - 1) % 2, (i - 1) // 2
    local x, y = GAP + col * (CARD_W + GAP), GAP + row * (CARD_H + GAP) - top

    if y > -CARD_H and y < self.h then
      draw_card(g, s, x, y, CARD_W)
    end

    s.card_at = { x = x, y = y }
  end
end

function grid:mouse(action, x, y)
  if watched then return false end

  return ui.click(self, action, x, y, function(px, py)
    for _, b in ipairs(buttons) do
      if px >= b.x and px < b.x + b.w and py >= b.y and py < b.y + b.h then
        return "button " .. b.text .. b.x .. b.y, function()
          print("sandboxes: pressed " .. b.text)
          b.act()
        end
      end
    end

    for _, s in ipairs(shown()) do
      local at = s.card_at

      if at and px >= at.x and px < at.x + CARD_W and py >= at.y and py < at.y + CARD_H then
        return "card " .. s.name, function()
          local again = chosen == s.name and (sys.ticks() - (grid.clicked or 0)) <= ui.double_click_ticks()

          chosen, grid.clicked = s.name, sys.ticks()
          print("sandboxes: chose " .. s.name)

          if again then open(s.name, 1) end
        end
      end
    end
  end)
end

function grid:wheel(n)
  local rows = (#shown() + 1) // 2

  top = math.max(0, math.min(top + n * 60, math.max(0, rows * (CARD_H + GAP) + GAP - self.h)))
  return true
end

win:add(grid)

--
-- **One sandbox, watched**: its graphs, what it was given and its console;
-- the tabs - its whole console typed into, its folders and ports, its
-- log, its settings.
--
local TABS = { "Overview", "Console", "Folders and ports", "Log", "Settings" }
local detail = ui.view{ x = SIDE, y = L.head, w = W - SIDE, h = H - L.head - 34,
                        follow = { "left", "right", "top", "bottom" }, hidden = true }
local detail_buttons = {}

detail.focusable = true

local function find(name)
  for _, s in ipairs(all) do
    if s.name == name then return s end
  end
end

-- The console's lines, from its text: the last `n`, and the line being
-- written - a prompt - as the last.
local function last_lines(text, n)
  local lines = {}

  for line in (text .. "\n"):gmatch("([^\n]*)\n") do lines[#lines + 1] = line end

  if lines[#lines] == "" and text:sub(-1) == "\n" then table.remove(lines) end

  local from = math.max(1, #lines - n + 1)

  return { table.unpack(lines, from) }
end

local function panel(g, x, y, w, h, title)
  g:fill_round(x, y, w, h, theme.raised, 10)
  g:frame_round(x, y, w, h, theme.line_soft, 10)
  g:text(x + 14, y + 12, title, theme.text_dim, nil, "label")
end

local function terminal(g, x, y, w, h, text, cursor)
  local ground = 0xff0f141c
  local mono = "mono"
  local lh = gfx.height(mono) + 2

  g:fill_round(x, y, w, h, ground, 8)

  local lines = last_lines(text, math.max(1, (h - 16) // lh))
  local ty = y + 8

  for i, line in ipairs(lines) do
    g:text(x + 10, ty, ui.fitted(line, w - 20, mono), 0xffd7dde8, ground, mono)

    if cursor and i == #lines then
      g:fill(x + 10 + gfx.measure(line, mono), ty, gfx.measure("M", mono), lh - 2, 0xffd7dde8)
    end

    ty = ty + lh
  end
end

function detail:draw(g)
  local s = find(watched)
  local x, y = 20, 14

  detail_buttons = {}
  g:fill(0, 0, self.w, self.h, theme.window)

  if not s then
    g:text(20, 20, tostring(watched) .. " is not there any more", theme.text_dim, nil, "ui")
    return
  end

  local function act(text, fn, primary, bx, by)
    local w = gfx.measure(text) + 24

    g:fill_round(bx, by, w, 28, primary and theme.accent or theme.raised, 7)
    g:frame_round(bx, by, w, 28, theme.line, 7)
    g:text(bx + 12, by + (28 - gfx.height()) // 2, text, primary and 0xffffffff or theme.text, nil, "ui")
    detail_buttons[#detail_buttons + 1] = { x = bx, y = by, w = w, h = 28, text = text, act = fn }
    return w
  end

  -- Back to every sandbox; its name, what it runs; its buttons.
  act("\u{2039} All", function() watched = nil print("sandboxes: back") end, false, x, y)
  g:text(x + 80, y - 2, s.name, theme.text, nil, "title")
  g:text(x + 80, y + gfx.height("title"), ui.fitted(subtitle(s), self.w - 460, "label"),
         theme.text_dim, nil, "label")

  local st = state_of(s)
  local rx = self.w - 20

  local function right(text, fn, primary)
    local w = gfx.measure(text) + 24

    rx = rx - w
    act(text, fn, primary, rx, y)
    rx = rx - 8
  end

  if st == "running" or st == "task" then
    if not s.task then right("Restart", function() restart(s.name) end) end
    right("Stop", function() stop(s.name) end)
  elseif not s.task then
    right("Delete", function() delete(s.name) end)
    right("Start", function() start(s.name) end, true)
  end

  -- The tabs.
  local ty = y + 46
  local tx = x

  for i, name in ipairs(TABS) do
    local w = gfx.measure(name) + 24
    local on = i == tab

    g:text(tx + 12, ty + 6, name, on and theme.accent or theme.text, nil, "ui")

    if on then g:fill(tx, ty + 6 + gfx.height() + 6, w, 2, theme.accent) end

    detail_buttons[#detail_buttons + 1] = { x = tx, y = ty, w = w, h = 34, text = name,
                                            act = function() tab = i print("sandboxes: tab " .. name) end }
    tx = tx + w
  end

  g:fill(0, ty + 36, self.w, 1, theme.line_soft)

  -- For its suite: where the tabs and buttons are, once a sandbox.
  if TELL and detail.told ~= s.name .. st then
    local said_at = {}

    for _, b in ipairs(detail_buttons) do
      said_at[#said_at + 1] = ("%s@%d,%d"):format(b.text, SIDE + b.x + b.w // 2, L.head + b.y + b.h // 2)
    end

    detail.told = s.name .. st
    print("sandboxes: detail " .. table.concat(said_at, " | "))
  end

  local top_y = ty + 50
  local body_h = self.h - top_y - 16
  local c = console[s.name] or { text = "" }

  if tab == 1 then
    local lw = (self.w - 60) * 45 // 100
    local rw = self.w - 60 - lw
    local gh = (body_h - 24) // 3

    panel(g, 20, top_y, lw, gh, ("Processors \u{b7} 1 of 1 \u{b7} %s"):format(
          busy[s.name] and ("%d%%"):format(math.floor(busy[s.name] * 100 + 0.5)) or "\u{2013}"))
    ui.history.draw(g, 34, top_y + 34, lw - 28, gh - 46, (history[s.name] or {}).cpu or {},
                    { colours = { BLUE } })

    panel(g, 20, top_y + gh + 12, lw, gh, ("Memory \u{b7} %d MB given"):format(
          s.now and s.now.ram // MB or s.memory))
    g:text(34, top_y + gh + 46, "What the guest uses needs the guest to say - later.",
           theme.text_dim, nil, "label")

    local net = (history[s.name] or {}).net or {}
    local last = net[#net]

    panel(g, 20, top_y + 2 * (gh + 12), lw, gh, last and network_words(s) ~= "none"
          and ("Network \u{b7} %s/s in \u{b7} %s/s out"):format(files.size(last[1]), files.size(last[2]))
          or "Network")

    if last and network_words(s) ~= "none" then
      ui.history.draw(g, 34, top_y + 2 * (gh + 12) + 34, lw - 28, gh - 46, net, { mode = "mirrored" })
    else
      g:text(34, top_y + 2 * (gh + 12) + 34, ({
             ["outgoing only"] = "Outgoing only, through Kosmos's - a gateway at 10.0.3.2.",
             ["outgoing, and ports in"] = "Outgoing through Kosmos's, and its ports carried in.",
             ["ports in only"] = "Its ports carried in, and nothing out.",
           })[network_words(s)] or "None - it reaches nothing.", theme.text_dim, nil, "label")
    end

    local gx = 40 + lw
    local given = {
      { "Image", s.task and "-" or s.image },
      { "Memory", ("%d MB"):format(s.now and s.now.ram // MB or s.memory) },
      { "Folders", #(s.folders or {}) > 0 and folder_words(s.folders[1]) .. (#s.folders > 1 and (" and " .. (#s.folders - 1) .. " more") or "") or "none" },
      { "Network", network_words(s) },
      { "Command", s.command ~= "" and s.command or "a shell" },
      { "Starts", s.starts or "by hand" },
      { "Stops", (s.command or "") ~= "" and "when its command ends" or "when its shell ends" },
    }
    local gh2 = 34 + #given * (gfx.height() + 6) + 8

    panel(g, gx, top_y, rw, gh2, "Given")

    for i, row in ipairs(given) do
      local ry = top_y + 34 + (i - 1) * (gfx.height() + 6)

      g:text(gx + 14, ry, row[1], theme.text_dim, nil, "ui")
      g:text(gx + 110, ry, ui.fitted(row[2], rw - 124, "ui"), theme.text, nil, "ui")
    end

    local cy = top_y + gh2 + 12

    panel(g, gx, cy, rw, body_h - gh2 - 12, "Console, the last lines")
    terminal(g, gx + 12, cy + 32, rw - 24, body_h - gh2 - 56, c.text, s.now and s.now.running)
  elseif tab == 2 then
    terminal(g, 20, top_y, self.w - 40, body_h - 24, c.text, s.now and s.now.running)
    g:text(20, top_y + body_h - 18,
           s.now and s.now.running and "Typing here goes to the sandbox - click in it first"
           or "Not running", theme.text_dim, nil, "label")
  elseif tab == 3 then
    local fy = top_y

    g:text(20, fy, "FOLDERS", theme.text_dim, nil, "label")
    fy = fy + gfx.height("label") + 8

    if #(s.folders or {}) == 0 then
      g:text(20, fy, "none - nothing of /Home reaches it", theme.text_dim, nil, "ui")
      fy = fy + gfx.height() + 6
    end

    for _, f in ipairs(s.folders or {}) do
      chip(g, 20, fy, folder_words(f))
      fy = fy + 32
    end

    fy = fy + 16
    g:text(20, fy, "PORTS AND THE NETWORK", theme.text_dim, nil, "label")
    fy = fy + gfx.height("label") + 8

    for _, p2 in ipairs(s.ports or {}) do
      chip(g, 20, fy, port_words(p2))
      fy = fy + 32
    end

    g:text(20, fy, ({
             ["outgoing only"] = "outgoing only - it reaches out through Kosmos's connections, and nothing reaches in",
             ["outgoing, and ports in"] = "Kosmos's ports above carried in, and it reaches out through Kosmos's connections",
             ["ports in only"] = "Kosmos's ports above carried in, and it reaches nothing itself",
           })[network_words(s)] or "none - nothing reaches it, and it reaches nothing", theme.text_dim, nil, "ui")
  elseif tab == 4 then
    local log = {}

    for line in (c.text .. "\n"):gmatch("([^\n]*)\n") do
      if line:match("^vmm: ") or line:match("^%[%s*[%d.]+%]") then log[#log + 1] = line end
    end

    terminal(g, 20, top_y, self.w - 40, body_h - 8, #log > 0 and table.concat(log, "\n")
             or "nothing from its monitor or its kernel yet", false)
  else
    local lines = {}

    for line in sandboxes.format(s):gmatch("[^\n]+") do lines[#lines + 1] = line end

    terminal(g, 20, top_y, self.w - 40, math.min(body_h - 8, (#lines + 1) * (gfx.height("mono") + 2) + 16),
             table.concat(lines, "\n"), false)
    g:text(20, top_y + math.min(body_h - 8, (#lines + 1) * (gfx.height("mono") + 2) + 16) + 10,
           "/Home/Sandboxes/" .. s.name .. "/sandbox.txt", theme.text_dim, nil, "label")
  end
end

function detail:mouse(action, x, y)
  return ui.click(self, action, x, y, function(px, py)
    for _, b in ipairs(detail_buttons) do
      if px >= b.x and px < b.x + b.w and py >= b.y and py < b.y + b.h then
        return "d " .. b.text, function()
          print("sandboxes: pressed " .. b.text)
          b.act()
        end
      end
    end

    return "focus", function() end
  end)
end

-- Typed into the Console tab: to the sandbox, a key at a time.
function detail:key(code)
  if tab ~= 2 or not watched or type(code) ~= "number" or code < 0 or code > 255 then return false end

  sandboxes.type(watched, string.char(code == 13 and 10 or code))
  return true
end

win:add(detail)

--
-- **The foot**: what was last said.
--
local foot = ui.view{ x = SIDE, y = H - 34, w = W - SIDE, h = 34, follow = { "left", "right", "bottom" } }

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, 0, self.w, 1, theme.line_soft)
  g:text(16, (self.h - gfx.height()) // 2, said or "sandbox, at the prompt, does all of this too",
         theme.text_dim, nil, "ui")
end

win:add(foot)

--------------------------------------------------------------------------
-- New sandbox, as drawn: nothing given that is not chosen here.
--------------------------------------------------------------------------

local sheet

local function new_sandbox()
  if sheet then return end

  local SW, SH = 620, 476
  local MEMORY = { 128, 256, 512, 1024 }
  local image_names = {}

  for _, i in ipairs(images) do image_names[#image_names + 1] = { i.name, i.title .. " \u{b7} " .. (i.bytes // MB) .. " MB" } end

  sheet = ui.sheet{ title = "New sandbox", w = SW, h = SH }

  local function row(y, text)
    sheet:put(ui.label{ x = 20, y = y + 5, w = 110, text = text, role = "ui", color = "text_dim" })
  end

  sheet:put(ui.label{ x = 20, y = 18, w = SW - 40, text = "New sandbox", role = "heading" })
  sheet:put(ui.label{ x = 20, y = 44, w = SW - 40, text = "A small operating system, given only what you choose here.",
                      role = "label", color = "text_dim" })

  local y = 80
  local name = ui.field{ x = 140, y = y, w = SW - 160, h = 28, text = "" }

  row(y, "Name") sheet:put(name) y = y + 40

  local image = ui.dropdown{ x = 140, y = y, w = SW - 160, choices = image_names,
                             value = image_names[1] and image_names[1][1] or nil }

  row(y, "Image") sheet:put(image) y = y + 40

  local kind = ui.segments{ x = 140, y = y, items = { "A task: run once", "A service: keeps running", "A shell" }, on = 3 }

  row(y, "Kind") sheet:put(kind) y = y + 40

  local command = ui.field{ x = 140, y = y, w = SW - 160, h = 28, text = "", mono = true }

  row(y, "Command") sheet:put(command) y = y + 40

  local processors = ui.segments{ x = 140, y = y, items = { "1" }, on = 1 }

  row(y, "Processors") sheet:put(processors)
  sheet:put(ui.label{ x = 200, y = y + 5, w = 300, text = "one, for now", role = "label", color = "text_dim" })
  y = y + 40

  local memory = ui.segments{ x = 140, y = y, items = { "128 MB", "256 MB", "512 MB", "1 GB" }, on = 2 }

  row(y, "Memory") sheet:put(memory) y = y + 40

  -- A folder of /Home, where the guest sees it, and whether only to read.
  local folder = ui.field{ x = 140, y = y, w = 230, h = 28, text = "", hint = "/Home/..." }
  local folder_at = ui.field{ x = 380, y = y, w = 110, h = 28, text = "/work" }
  local folder_ro = ui.checkbox{ x = 500, y = y + 4, text = "read only", checked = false }

  row(y, "Folder") sheet:put(folder) sheet:put(folder_at) sheet:put(folder_ro)
  y = y + 40
  row(y, "Network")
  local network = ui.segments{ x = 140, y = y, items = { "None", "Outgoing only", "Ports" }, on = 1 }
  local ports = ui.field{ x = 140 + network.w + 10, y = y, w = SW - 160 - network.w - 10, h = 28,
                          text = "", hint = "3001:3000 - Kosmos's:its" }

  sheet:put(network) sheet:put(ports)
  y = y + 40

  local at_start = ui.checkbox{ x = 140, y = y, text = "with Kosmos", checked = false }
  local console_box = ui.checkbox{ x = 290, y = y, text = "open its console", checked = true }

  row(y, "Starts") sheet:put(at_start) sheet:put(console_box) y = y + 44

  local function close()
    win:remove(sheet)
    sheet = nil
  end

  sheet:put(ui.button{ x = SW - 20 - 150, y = SH - 48, w = 150, h = 30, text = "Create and start", go = true,
                       on_click = function()
                         local s = sandboxes.parse("", name.text)

                         s.image = image.value or "alpine"
                         s.kind = ({ "task", "service", "shell" })[kind.on]
                         s.command = command.text or ""
                         s.memory = MEMORY[memory.on] or 256
                         s.starts = at_start.checked and "with Kosmos" or "by hand"
                         s.network = network.on >= 2 and "outgoing" or "none"

                         -- Ports: Kosmos's carried in, and out as well.
                         if network.on == 3 then
                           for text in (ports.text or ""):gmatch("[^%s,]+") do
                             local from, to = sandboxes.port(text)

                             if not from then said = to print("sandboxes: " .. said) return end

                             s.ports[#s.ports + 1] = ("%d:%d"):format(from, to)
                           end

                           if #s.ports == 0 then said = "Ports wants a port - 3001:3000" print("sandboxes: " .. said) return end
                         end

                         if (folder.text or "") ~= "" then
                           local f, bad = sandboxes.folder(folder.text .. " " .. (folder_at.text ~= "" and folder_at.text or "/work")
                                                          .. (folder_ro.checked and " ro" or " rw"))

                           if not f then said = tostring(bad) print("sandboxes: " .. said) return end

                           s.folders = { sandboxes.folder_text(f) }
                         end

                         local ok, why = sandboxes.save(s)

                         if not ok then said = tostring(why) print("sandboxes: " .. said) return end

                         close()
                         refresh(true)
                         chosen = s.name
                         start(s.name)

                         if console_box.checked then open(s.name, 2) end

                         print("sandboxes: made " .. s.name)

                         if TELL then
                           local back = sandboxes.definition(s.name)

                           print(("sandboxes: %s written, network %s, ports %s"):format(s.name,
                                 back and back.network or "?", back and table.concat(back.ports, " ") or "?"))
                         end
                       end })
  sheet:put(ui.button{ x = SW - 20 - 150 - 100, y = SH - 48, w = 90, h = 30, text = "Cancel",
                       on_click = close })
  win:add(sheet)

  if TELL then
    print(("sandboxes: new sandbox sheet, name at %d,%d, create at %d,%d, memory at %d,%d, "
           .. "ports at %d,%d, port field at %d,%d"):format(
          sheet.panel.x + 140 + 20, sheet.panel.y + 80 + 14,
          sheet.panel.x + SW - 20 - 75, sheet.panel.y + SH - 33,
          sheet.panel.x + memory.x + 20, sheet.panel.y + memory.y + memory.h // 2,
          sheet.panel.x + network.x + network.w - 20, sheet.panel.y + network.y + network.h // 2,
          sheet.panel.x + ports.x + 20, sheet.panel.y + ports.y + 14))
  end
end

new_button.on_click = function() new_sandbox() end

--------------------------------------------------------------------------
-- The clock: twice a second.
--------------------------------------------------------------------------

local clock = ui.view{ x = 0, y = 0, w = 0, h = 0 }
local told = nil

function clock:tick()
  refresh(false)

  local c = counts()

  head.sub = ("%d running \u{b7} %d task%s \u{b7} %d MB of %d MB"):format(c.running, c.tasks,
             c.tasks == 1 and "" or "s", given_mb(), (machine.ram or 0) // MB)
  grid.hidden = watched ~= nil
  detail.hidden = watched == nil

  if TELL then
    local line = {}

    for _, s in ipairs(all) do line[#line + 1] = s.name .. "=" .. state_of(s) end

    line = table.concat(line, " ")

    if line ~= told then
      told = line
      print("sandboxes: states " .. line)
    end
  end
end

win:add(clock)

refresh(true)

if TELL then
  print(("sandboxes: window at %d,%d; side %d wide; cards %d tall, from %d,%d; new at %d,%d"):format(
        win.origin_x or 0, win.origin_y or 0, SIDE, CARD_H, SIDE + GAP, L.head + GAP,
        W - 90, L.head // 2))
end

win.tick_every = counter_hz // 2
win:run()
