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

-- The drawing's pictures (`docs/sandboxes.html`), from the shipped set: a
-- sandbox, an image, a folder, a network.
local ICON_SANDBOX, ICON_IMAGE = "App_Playground.png", "Device_Harddisk.png"
local ICON_FOLDER, ICON_NETWORK = "Folder_generic.png", "Prefs_Network.png"

--------------------------------------------------------------------------
-- What there is, asked on the window's own clock.
--------------------------------------------------------------------------

local all, images, machine = {}, {}, {}
local before, busy, history = {}, {}, {}
local group, chosen, watched, tab = "all", nil, nil, 1
local helping = false                   -- the help page shown, in place of the cards
local apply_views                       -- forward: which of cards, a sandbox or the help shows
local console = {}                      -- name -> { at, text }
local said = nil                        -- the foot's line
local pending = {}                      -- name -> "starting" | "restarting"
local last_full = 0
local start                             -- forward: a restart calls it
local folder_words                      -- forward: the cards say folders
local amount_of                         -- forward: the sidebar says memory
local reports = {}                      -- name -> the file its start says in

--
-- **What the sandbox open in the window has said since it last looked**,
-- kept in `console`: true when something arrived. Read on every pass of the
-- window's loop while one is open (`win.on_frame`, below) - not on the
-- half-second clock the rest is refreshed on, which made a key's echo wait
-- up to half a second to appear (Diego, 13 October: "shouldnt be instant?").
--
--
-- **What arrived, added to what is shown - a backspace stepping back.**
-- Linux's shell echoes an erased character as 0x08 - with a space and
-- another 0x08 to blank it, `\b \b` - which Kosmos's console has always
-- obeyed (`console.c`) and this window drew as a glyph each (Diego, 13
-- October: "backspace is not working, is printing some weird characters").
-- It goes back over the last character, a whole UTF-8 one, and never past
-- the start of the line.
--
local function added(shown, more)
  if not more:find("\b", 1, true) then return shown .. more end

  for piece, back in more:gmatch("([^\b]*)(\b?)") do
    shown = shown .. piece

    if back ~= "" and #shown > 0 and shown:sub(-1) ~= "\n" then
      local cut = #shown

      while cut > 1 and shown:byte(cut) >= 0x80 and shown:byte(cut) < 0xc0 do cut = cut - 1 end

      shown = shown:sub(1, cut - 1)
    end
  end

  return shown
end

local function read_console()
  if not watched then return false end

  local c = console[watched] or { at = 0, text = "" }
  local text, next_at = sandboxes.read(watched, c.at)
  local arrived = text ~= nil and #text > 0

  if arrived then
    c.text = added(c.text, text):sub(-24000)
    c.at = next_at

    -- For its suite: each line of what arrived, as it arrives, and the
    -- last line as it is shown.
    if TELL then
      for line in text:gmatch("[^\n]+") do
        print("sandboxes: console " .. watched .. ": " .. line)
      end

      print("sandboxes: shown " .. watched .. ": " .. (c.text:match("([^\n]*)$") or ""))
    end
  end

  console[watched] = c
  return arrived
end

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

  read_console()
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
  helping = false
  watched, tab = name, which or 1
  console[name] = console[name] or { at = 0, text = "" }

  if apply_views then apply_views() end

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
local head_icon = ui.view{ w = 26, h = 26 }

function head_icon:draw(g) g:icon(0, 0, ICON_SANDBOX, 26) end

-- The drawing's ?, a ring round it: the help, as the row under the machine opens it.
local help_button = ui.view{ w = 24, h = 24 }

function help_button:draw(g)
  g:frame_round(0, 0, 24, 24, theme.line, 12)
  g:text((24 - gfx.measure("?", "ui")) // 2, (24 - gfx.height("ui")) // 2, "?", theme.text_dim, nil, "ui")
end

function help_button:mouse(action, x, y)
  return ui.click(self, action, x, y, function()
    return "help", function()
      helping, watched = true, nil
      apply_views()
      print("sandboxes: help")
    end
  end)
end

local head = ui.header{ x = 0, y = 0, w = W, title = "Sandboxes", sub = "", left = { head_icon },
                        right = { help_button, new_button }, title_bar = true }

win:add(head)

-- "256 MB of 2894 MB" as the drawing says it: "0.3 of 2.8 GB" once either is a gigabyte.
amount_of = function(mb, of)
  if of >= 1024 then return ("%.1f of %.0f GB"):format(mb / 1024, of / 1024) end

  return ("%d of %d MB"):format(mb, of)
end

local function find(name)
  for _, s in ipairs(all) do
    if s.name == name then return s end
  end
end

-- An image's name for a person: its package's title, else its folder's name.
local function image_title(id)
  for _, i in ipairs(images) do
    if i.name == id then return i.title, i.bytes end
  end

  return id, nil
end

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

  -- Watching one, its own group is the one lit, as the drawing has it.
  local lit = group

  if watched then
    local w = find(watched)
    local st = w and state_of(w)

    lit = (st == "task") and "tasks" or (st == "running" or st == "starting" or st == "restarting")
          and "running" or "stopped"
  end

  for _, it in ipairs({ { "all", "All" }, { "running", "Running" }, { "stopped", "Stopped" }, { "tasks", "Tasks" } }) do
    local on = lit == it[1] and not helping

    if on then g:fill_round(8, y, self.w - 16, 30, theme.accent, 7) end

    local ink = on and 0xffffffff or theme.text

    g:icon(16, y + 5, ICON_SANDBOX, 20)
    g:text(44, y + (30 - gfx.height()) // 2, it[2], ink, nil, "ui")

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
    local size = ("%d MB"):format(i.bytes // MB)
    local lines = ui.wrapped(i.title, self.w - 44 - 22 - gfx.measure(size, "label") - 8, "ui", 2)

    g:icon(16, y + 1, ICON_IMAGE, 20)

    for k, line in ipairs(lines) do
      g:text(44, y + (k - 1) * (gfx.height() + 2), line, theme.text, nil, "ui")
    end

    g:text(self.w - 22 - gfx.measure(size, "label"), y + 2, size, theme.text_dim, nil, "label")
    y = y + #lines * (gfx.height() + 2) + 8
  end

  g:text(20, y, "+ Get an image", theme.accent, nil, "ui")
  side_rows[#side_rows + 1] = { y = y - 4, h = gfx.height() + 8, get_image = true }
  y = y + gfx.height() + 22

  y = draw_group(g, y, "Machine")
  g:text(20, y, ("%d of %d processors"):format(c.running + c.tasks, machine.cpus or 1),
         theme.text_dim, nil, "ui")
  y = y + gfx.height() + 4
  g:text(20, y, ("%s given"):format(amount_of(given_mb(), (machine.ram or 0) // MB)),
         theme.text_dim, nil, "ui")
  y = y + gfx.height() + 4

  -- What the processor and its firmware allow (`sysinfo.virt`, probed at
  -- boot): on a PC, whether VT-x is there and what its setup decided.
  local virt = machine.guests and machine.virt == 4 and "Virtualization: EL2"
               or machine.guests and "Virtualization: VT-x, EPT"
               or ({ [1] = "VT-x is off in this machine's setup: turn on Intel Virtualization Technology",
                     [2] = "VT-x is on and this processor lacks what Kosmos needs of it (EPT)",
                     [3] = "VT-x is here and not locked; Kosmos does not drive it yet" })[machine.virt]
               or "This machine cannot run sandboxes yet"

  for _, line in ipairs(ui.wrapped(virt, self.w - 36, "ui", 3)) do
    g:text(20, y, line, machine.guests and theme.text_dim or theme.io_out, nil, "ui")
    y = y + gfx.height() + 2
  end

  -- The help, a row of its own, lit while it is shown.
  y = y + 10

  if helping then g:fill_round(8, y, self.w - 16, 30, theme.accent, 7) end

  g:text(20, y + (30 - gfx.height()) // 2, "How sandboxes work", helping and 0xffffffff or theme.text, nil, "ui")
  side_rows[#side_rows + 1] = { y = y, h = 30, help = true }

  if TELL and side.told_help ~= y then
    side.told_help = y
    print(("sandboxes: help row at %d,%d"):format(60, L.head + y + 15))
  end
end

function side:mouse(action, x, y)
  return ui.click(self, action, x, y, function(_, py)
    for _, r in ipairs(side_rows) do
      if py >= r.y and py < r.y + r.h then
        if r.help then
          return "help", function() helping, watched = true, nil apply_views() print("sandboxes: help") end
        end

        if r.get_image then
          return "get", function()
            fs.send("/Running/wm", { type = "launch", program = "packages", wait = false })
            print("sandboxes: get an image")
          end
        end

        return r.group, function()
          group, watched, helping = r.group, nil, false
          apply_views()
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

-- What a sandbox was given, as a chip with its picture: a folder, a port, its network.
local function chip(g, x, y, text, icon)
  local inset = icon and 26 or 9
  local w = gfx.measure(text, "label") + inset + 9

  g:fill_round(x, y, w, 24, theme.mix(theme.raised, theme.line_soft, 500), 6)

  if icon then g:icon(x + 6, y + 4, icon, 16) end

  g:text(x + inset, y + (24 - gfx.height("label")) // 2, text, theme.text, nil, "label")
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
  local image = image_title(s.image)
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

  g:icon(ix, y + 16, ICON_SANDBOX, 36)

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

  for _, p2 in ipairs(s.ports or {}) do cx = chip(g, cx, y + 104, "port " .. port_words(p2), ICON_NETWORK) end
  for _, f in ipairs(s.folders or {}) do cx = chip(g, cx, y + 104, folder_words(f), ICON_FOLDER) end

  if s.network == "outgoing" then
    chip(g, cx, y + 104, #(s.ports or {}) > 0 and "outgoing" or "outgoing only", ICON_NETWORK)
  elseif #(s.ports or {}) == 0 then
    chip(g, cx, y + 104, "no network", ICON_NETWORK)
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

  -- Its picture, its name and what it runs, as the drawing has them; back
  -- to every sandbox is a group in the sidebar, or the "..." menu.
  g:icon(x, y - 4, ICON_SANDBOX, 40)
  g:text(x + 52, y - 4, s.name, theme.text, nil, "title")
  g:text(x + 52, y - 2 + gfx.height("title"), ui.fitted(subtitle(s) .. (s.now and s.now.pid
         and (" \u{b7} vmm process %d"):format(s.now.pid) or ""), self.w - 440, "label"),
         theme.text_dim, nil, "label")

  local st = state_of(s)
  local rx = self.w - 20
  local rx_more = rx - 40

  local function right(text, fn, primary)
    local w = gfx.measure(text) + 24

    rx = rx - w
    act(text, fn, primary, rx, y)
    rx = rx - 8
  end

  -- "...": what is not a button - the sandbox's deletion, and back.
  right("...", function()
    local items = { { text = "Every sandbox", on_choose = function() watched = nil print("sandboxes: back") end } }

    if st ~= "running" and st ~= "task" and not s.task then
      items[#items + 1] = { separator = true }
      items[#items + 1] = { text = "Delete " .. s.name, on_choose = function() delete(s.name) watched = nil end }
    end

    win:open_menu(win.origin_x + SIDE + rx_more, win.origin_y + L.head + y + 30, items)
  end)
  rx_more = rx + 8

  if st == "running" or st == "task" then
    if not s.task then right("Restart", function() restart(s.name) end) end
    right("Stop", function() stop(s.name) end)
    right("Console", function() tab = 2 print("sandboxes: tab Console") end)
  elseif not s.task then
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
    local title, bytes = image_title(s.image)
    local ports = {}

    for _, p2 in ipairs(s.ports or {}) do ports[#ports + 1] = "port " .. port_words(p2) end

    local given = {
      { "Image", bytes and ("%s, %d MB"):format(title, bytes // MB) or title },
      { "Folder", #(s.folders or {}) > 0 and folder_words(s.folders[1]) .. (#s.folders > 1 and (" and " .. (#s.folders - 1) .. " more") or "") or "none" },
      { "Network", #ports > 0 and table.concat(ports, ", ") .. (s.network == "outgoing" and ", outgoing" or "")
                   or network_words(s) },
      { "Starts", s.starts == "with Kosmos" and "with Kosmos" or "by hand" },
      { "Stops", s.kind == "service" and "never on its own" or (s.command or "") ~= "" and "when its command ends"
                 or "when its shell ends" },
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
    -- The form is on the window (`settings_open`); under it, where it is kept.
    g:text(20, self.h - 26, "Kept in /Home/Sandboxes/" .. s.name .. "/sandbox.txt", theme.text_dim, nil, "label")
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


--------------------------------------------------------------------------
-- **How sandboxes work** - the help, as drawn (`docs/sandboxes.html`, "How
-- sandboxes work: the help"; Diego, 13 October: "the sandboxes app needs a
-- help screen that tells the user how it operates and works", "we need to
-- tell the user that sandboxes does not behave like qemu that emulates a
-- full pc", "it also runs only oses that are targeted for the host
-- architecture", "we also need to explain the uses and benefits"). A page
-- in place of the cards, scrolled with the wheel.
--------------------------------------------------------------------------

local LEAD = "A sandbox is a small machine of its own, with one operating system in it. Its programs run on "
  .. "this processor directly, at its speed, inside walls the processor itself keeps - Intel's VT-x on a PC, "
  .. "EL2 on an ARM board - and they reach only what you gave it."

-- Each card: its title, then paragraphs, bullets ({ lead, rest }) or rows ({ term, words, true }).
local HELP = {
  left = {
    { "Not a whole PC", {
      "QEMU and VirtualBox imitate a whole computer - its firmware, graphics card, disks and USB - so they "
      .. "can start anything, slowly. A sandbox does not: it has a console, a clock and a few virtio devices "
      .. "for folders and the network, and nothing else.",
      "So it runs a system that starts from a kernel and has virtio drivers - Linux: Alpine, Debian, a "
      .. "minimal Ubuntu - and not Windows or an installer's ISO. That is why one starts in under a second "
      .. "and costs almost nothing while it waits." } },
    { "Only for this machine's processor", {
      "The guest's instructions are this processor's own: x86-64 Linux on a PC, ARM64 Linux on an ARM "
      .. "board such as a Raspberry Pi. Running the other kind would mean translating every instruction, "
      .. "which is what emulators do and why they are slow. Alpine comes for both, so a sandbox is defined "
      .. "the same way on either." } },
    { "What it is for", {
      { "Something from elsewhere, kept apart", "a tool, a compiler, a script from the internet. It sees "
        .. "only the folders you give it, read only if you say, and the network only if you allow it." },
      { "Linux's programs without porting them", "Kosmos is not Unix; Linux's software runs here as itself." },
      { "A task, thrown away", "starts in about a second, does one thing, says how it ended, and is gone." },
      { "A service, kept running", "a small server answering on a port Kosmos carries in, started with "
        .. "Kosmos if you like." },
      { "Fast and light", "nothing is emulated, and a sandbox costs the memory you give it." } } },
  },
  right = {
    { "How it works", {
      { "Images", "the systems you can start, installed like any package with + Get an image. Kept in "
        .. "/Home/Sandboxes/Images.", true },
      { "A sandbox", "a name, an image and what it is given - its definition, in /Home/Sandboxes/<name>. "
        .. "Change it in its Settings; it takes effect at its next start.", true },
      { "Its kind", "a task runs one command and is thrown away; a service keeps running; a shell is a "
        .. "prompt you type into, in its Console.", true },
      { "Folders", "a folder of /Home, seen inside where you choose - read only, or read and write.", true },
      { "Network", "none; outgoing only, through Kosmos's own connections; or ports, Kosmos's carried in "
        .. "to the guest's.", true } } },
    { "At the prompt", { mono = true,
      "sandbox run alpine -- uname -a",
      "sandbox new web --image alpine --service \\",
      "    --port 8080:80 -- httpd -f -p 80",
      "sandbox list",
      "sandbox console web",
      "sandbox stop web" } },
  },
}

local help_view = ui.view{ x = SIDE, y = L.head, w = W - SIDE, h = H - L.head - 34,
                           follow = { "left", "right", "top", "bottom" }, hidden = true }
local help_top, help_tall = 0, 0

-- A card at `x`, `y`, `w` wide: drawn when `g` is given, measured always; its height.
local function help_card(g, card, x, y, w)
  local body = card[2]
  local tx, room = x + 14, w - 28
  local cy = y + 12 + gfx.height("heading") + 6
  local lh = gfx.height("label") + 3
  local ops = {}

  local function line(at, text, ink, face) ops[#ops + 1] = { at, text, ink, face } end

  if body.mono then
    for _, text in ipairs(body) do line(cy, text, 0xffd7dde8, "mono") cy = cy + gfx.height("mono") + 3 end

    cy = cy + 4
  else
    for _, item in ipairs(body) do
      if type(item) == "string" then
        for _, l in ipairs(ui.wrapped(item, room, "label", 40)) do line(cy, l, theme.text_dim, "label") cy = cy + lh end

        cy = cy + 6
      elseif item[3] then                                   -- a row: its term, then its words
        local words = ui.wrapped(item[2], room - 84, "label", 20)

        ops[#ops + 1] = { cy, item[1], theme.text_dim, "label", 0 }

        for k, l in ipairs(words) do ops[#ops + 1] = { cy + (k - 1) * lh, l, theme.text, "label", 84 } end

        cy = cy + #words * lh + 5
      else                                                  -- a bullet: its lead, then the rest
        local words = ui.wrapped(item[1] .. " - " .. item[2], room - 14, "label", 20)

        ops[#ops + 1] = { cy, "\u{2022}", theme.text_dim, "label", 0 }

        for k, l in ipairs(words) do
          ops[#ops + 1] = { cy + (k - 1) * lh, l, k == 1 and theme.text or theme.text_dim, "label", 14 }
        end

        cy = cy + #words * lh + 5
      end
    end
  end

  local h = cy - y + 8

  if g then
    g:fill_round(x, y, w, h, theme.raised, 10)
    g:frame_round(x, y, w, h, theme.line_soft, 10)
    g:text(tx, y + 12, card[1], theme.text, nil, "heading")

    if body.mono then
      g:fill_round(tx - 4, y + 12 + gfx.height("heading") + 2, room + 8, h - gfx.height("heading") - 22, 0xff0f141c, 8)
    end

    for _, op in ipairs(ops) do
      g:text(tx + (op[5] or 0) + (body.mono and 6 or 0), op[1], op[2], op[3],
             body.mono and 0xff0f141c or nil, op[4])
    end
  end

  return h
end

function help_view:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  local x, y = 22, 18 - help_top

  g:text(x, y, "How sandboxes work", theme.text, nil, "title")
  y = y + gfx.height("title") + 6

  for _, l in ipairs(ui.wrapped(LEAD, self.w - 44, "ui", 6)) do
    g:text(x, y, l, theme.text_dim, nil, "ui")
    y = y + gfx.height() + 2
  end

  y = y + 12

  local cw = (self.w - 44 - 12) // 2
  local tall = 0

  for side_i, list in ipairs({ HELP.left, HELP.right }) do
    local cx, cy = x + (side_i - 1) * (cw + 12), y

    for _, card in ipairs(list) do
      cy = cy + help_card(g, card, cx, cy, cw) + 12
    end

    tall = math.max(tall, cy)
  end

  help_tall = tall + help_top + 12

  if TELL and not self.told then
    self.told = true
    print(("sandboxes: help shown, %d cards, %d tall"):format(#HELP.left + #HELP.right, help_tall))
  end
end

function help_view:wheel(n)
  help_top = math.max(0, math.min(help_top + n * 60, math.max(0, help_tall - self.h)))
  return true
end

win:add(help_view)

-- The cards, one sandbox, or the help: switched where it is chosen, not on the next tick.
apply_views = function()
  grid.hidden = watched ~= nil or helping
  detail.hidden = watched == nil or helping
  help_view.hidden = not helping
end

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
-- **A sandbox's definition, as a form** - the New sheet as drawn
-- (`docs/sandboxes.html`, "A new one"), and the same rows in a sandbox's
-- Settings tab, filled with what it has: Diego, 13 October, "the settings
-- for a machine is non editable, it should be all editable". Its image, its
-- kind and command, its memory, its folders and its network, and whether it
-- starts with Kosmos; its name only when it is new, since the name is its
-- folder in /Home/Sandboxes.
--------------------------------------------------------------------------

local panel_kit = use("/Kosmos/Libraries/panel.lua")
local KINDS = { "task", "service", "shell" }
local MEMORY_CHOICES = { 128, 512, 1024, 2048 }

local function memory_words(mb)
  return mb >= 1024 and ("%d GB"):format(mb // 1024) or ("%d MB"):format(mb)
end

--
-- A row of chips - a folder, a port - each with its picture and an x that
-- takes it away, then the drawing's "+ Add a folder" or "+ Port" in the
-- accent. A chip pressed elsewhere than its x is `on_click`ed: a folder's
-- turns between read only and read and write.
--
local function chip_row(spec)
  local v = ui.view{ x = spec.x, y = spec.y, w = spec.w, h = 28 }
  local spots = {}

  function v:draw(g)
    local x = 0

    spots = {}

    for i, it in ipairs(spec.items()) do
      local inset = it[2] and 26 or 9
      local w = gfx.measure(it[1], "label") + inset + 26

      if x + w > self.w then break end

      g:fill_round(x, 2, w, 24, theme.mix(theme.raised, theme.line_soft, 500), 6)

      if it[2] then g:icon(x + 6, 6, it[2], 16) end

      g:text(x + inset, 2 + (24 - gfx.height("label")) // 2, it[1], theme.text, nil, "label")
      g:text(x + w - 15, 2 + (24 - gfx.height("label")) // 2, "\u{d7}", theme.text_dim, nil, "label")
      spots[#spots + 1] = { x = x, w = w, i = i }
      x = x + w + 8
    end

    if spec.add_text and x + gfx.measure(spec.add_text) <= self.w then
      g:text(x + 2, (28 - gfx.height()) // 2, spec.add_text, theme.accent, nil, "ui")
      spots[#spots + 1] = { x = x, w = gfx.measure(spec.add_text) + 4, add = true }
    end
  end

  function v:mouse(action, px, py)
    return ui.click(self, action, px, py, function(cx)
      for _, sp in ipairs(spots) do
        if cx >= sp.x and cx < sp.x + sp.w then
          if sp.add then return "add", function() spec.on_add() end end

          if cx >= sp.x + sp.w - 20 then
            return "remove " .. sp.i, function() spec.on_remove(sp.i) end
          end

          return "chip " .. sp.i, function() if spec.on_click then spec.on_click(sp.i) end end
        end
      end
    end)
  end

  return v
end

--
-- The form, its rows put by `put` at `ox`, `oy`, `fw` wide, filled from `s`
-- - a definition, or a new one's defaults. `form.read()` is the definition
-- the rows say, or nil and why.
--
local function make_form(put, ox, oy, fw, s, editing)
  local f = { widgets = {} }
  local cx, cw = ox + 120, fw - 120
  local y = 0

  local function add(w) f.widgets[#f.widgets + 1] = w put(w) return w end
  local function label(at, text)
    add(ui.label{ x = ox, y = oy + at + 5, w = 112, text = text, role = "ui", color = "text_dim" })
  end

  if not editing then
    f.name = add(ui.field{ x = cx, y = oy + y, w = cw, h = 28, text = "" })
    label(y, "Name") y = y + 40
  end

  local choices, found = {}, false

  for _, i in ipairs(images) do
    choices[#choices + 1] = { i.name, i.title .. " \u{b7} " .. (i.bytes // MB) .. " MB" }
    found = found or i.name == s.image
  end

  if s.image and not found then choices[#choices + 1] = { s.image, s.image .. " - not installed" } end

  f.image = add(ui.dropdown{ x = cx, y = oy + y, w = cw, choices = choices,
                             value = s.image or (choices[1] and choices[1][1]) or nil })
  label(y, "Image") y = y + 40

  local kind_on = 3

  for i, k in ipairs(KINDS) do if s.kind == k then kind_on = i end end

  f.kind = add(ui.segments{ x = cx, y = oy + y, on = kind_on,
                            items = { "A task: run once, then thrown away", "A service: keeps running", "A shell" } })
  label(y, "Kind") y = y + 40

  f.command = add(ui.field{ x = cx, y = oy + y, w = cw, h = 28, text = s.command or "", mono = true })
  label(y, "Command") y = y + 40

  f.processors = add(ui.segments{ x = cx, y = oy + y, items = { "1" }, on = 1 })
  add(ui.label{ x = cx + f.processors.w + 10, y = oy + y + 5, w = 260, role = "label", color = "text_dim",
                text = ("of %d - one a sandbox, for now"):format(machine.cpus or 1) })
  label(y, "Processors") y = y + 40

  local mem = {}
  local mem_on = 2

  for _, mb in ipairs(MEMORY_CHOICES) do mem[#mem + 1] = mb end

  if s.memory then
    local known = false

    for _, mb in ipairs(mem) do known = known or mb == s.memory end

    if not known then mem[#mem + 1] = s.memory table.sort(mem) end

    for i, mb in ipairs(mem) do if mb == s.memory then mem_on = i end end
  end

  local mem_words = {}

  for i, mb in ipairs(mem) do mem_words[i] = memory_words(mb) end

  f.memory = add(ui.segments{ x = cx, y = oy + y, items = mem_words, on = mem_on })
  f.memory_values = mem
  label(y, "Memory") y = y + 40

  -- Its folders, each from the Open window's folder mode, where the guest
  -- sees it, and read only or not - the chip pressed turns it.
  f.folders = {}

  for _, text in ipairs(s.folders or {}) do
    local one = sandboxes.folder(text)

    if one then f.folders[#f.folders + 1] = one end
  end

  local folder_row
  folder_row = add(chip_row{ x = cx, y = oy + y, w = cw,
    items = function()
      local out = {}

      for _, one in ipairs(f.folders) do
        out[#out + 1] = { ("%s \u{2192} %s, %s"):format(one.host, one.at,
                          one.read_only and "read only" or "read and write"), ICON_FOLDER }
      end

      return out
    end,
    add_text = #f.folders < 4 and "+ Add a folder" or nil,
    on_click = function(i) f.folders[i].read_only = not f.folders[i].read_only end,
    on_remove = function(i) table.remove(f.folders, i) end,
    on_add = function()
      local chooser = panel_kit.folder{ start = "/Home", title = "A folder for the sandbox", button = "Give",
        on_choose = function(path)
          local taken, at = {}, "/srv"

          for _, one in ipairs(f.folders) do taken[one.at] = true end

          for n = 2, 9 do if taken[at] then at = "/srv" .. n end end

          if #f.folders < 4 and path:match("^/Home/") then
            f.folders[#f.folders + 1] = { host = path, at = at, read_only = false }
            print("sandboxes: folder " .. path .. " at " .. at)
          end
        end }

      if chooser then chooser:run() end
    end })
  label(y, "Folders") y = y + 40

  -- Its network: none, outgoing, or ports carried in - each port a chip,
  -- typed into the field and added with Return or "+ Port".
  f.ports = {}

  for _, text in ipairs(s.ports or {}) do f.ports[#f.ports + 1] = text end

  local net_on = (#f.ports > 0) and 3 or (s.network == "outgoing" and 2 or 1)

  f.network = add(ui.segments{ x = cx, y = oy + y, items = { "None", "Outgoing only", "Ports" }, on = net_on })
  label(y, "Network")

  local px = cx + f.network.w + 10
  local port_field = add(ui.field{ x = px, y = oy + y, w = 96, h = 28, text = "", hint = "3001:3000" })

  local function add_port()
    local from, to = sandboxes.port(port_field.text or "")

    if not from then said = tostring(to) print("sandboxes: " .. said) return end

    if #f.ports < 4 then f.ports[#f.ports + 1] = ("%d:%d"):format(from, to) end

    port_field.text = ""
    f.network.on = 3
  end

  port_field.on_enter = function() add_port() end
  f.port_field = port_field
  f.port_row = add(chip_row{ x = px + 106, y = oy + y, w = cx + cw - px - 106,
    items = function()
      local out = {}

      for _, text in ipairs(f.ports) do out[#out + 1] = { port_words(text), ICON_NETWORK } end

      return out
    end,
    add_text = "+ Port",
    on_remove = function(i) table.remove(f.ports, i) end,
    on_add = add_port })
  y = y + 40

  f.at_start = add(ui.checkbox{ x = cx, y = oy + y + 4, text = "with Kosmos", checked = s.starts == "with Kosmos" })

  if not editing then
    f.console = add(ui.checkbox{ x = cx + 150, y = oy + y + 4, text = "open its console", checked = true })
  end

  label(y, "Starts") y = y + 40
  f.h = y

  function f.read(base)
    local d = base or sandboxes.parse("", f.name and f.name.text or "")

    d.image = f.image.value or d.image or "alpine"
    d.kind = KINDS[f.kind.on]
    d.command = f.command.text or ""
    d.memory = f.memory_values[f.memory.on] or 512
    d.starts = f.at_start.checked and "with Kosmos" or "by hand"
    d.network = f.network.on == 2 and "outgoing" or (f.network.on == 3 and #f.ports == 0 and "none") or
                (f.network.on == 1 and "none") or d.network or "none"

    if f.network.on == 3 then
      if (f.port_field.text or "") ~= "" then add_port() end
      if #f.ports == 0 then return nil, "Ports wants a port - 3001:3000" end

      -- Ports, new, reach out as well, as the sheet always had it; an
      -- edited sandbox keeps whether it reached out.
      d.network = (not editing or s.network == "outgoing") and "outgoing" or "none"
      d.ports = {}

      for _, p2 in ipairs(f.ports) do d.ports[#d.ports + 1] = p2 end
    else
      d.ports = {}
    end

    d.folders = {}

    for _, one in ipairs(f.folders) do d.folders[#d.folders + 1] = sandboxes.folder_text(one) end

    return d
  end

  return f
end

--------------------------------------------------------------------------
-- New sandbox, as drawn: nothing given that is not chosen here.
--------------------------------------------------------------------------

local sheet

local function new_sandbox()
  if sheet then return end

  local SW = 720
  local form

  sheet = ui.sheet{ w = SW, h = 560 }                  -- its heading below, as drawn
  sheet:put(ui.label{ x = 20, y = 18, w = SW - 40, text = "New sandbox", role = "heading" })
  sheet:put(ui.label{ x = 20, y = 44, w = SW - 40, text = "A small operating system, given only what you choose here.",
                      role = "label", color = "text_dim" })

  form = make_form(function(w) sheet:put(w) end, 20, 80, SW - 40,
                   { image = images[1] and images[1].name, kind = "shell", memory = 512, network = "none" }, false)

  local SH = 80 + form.h + 52

  sheet.h = SH

  if sheet.panel then sheet.panel.h = SH end

  local function close()
    win:remove(sheet)
    sheet = nil
  end

  sheet:put(ui.button{ x = SW - 20 - 150, y = SH - 48, w = 150, h = 30, text = "Create and start", go = true,
                       on_click = function()
                         local s, why = form.read()

                         if not s then said = tostring(why) print("sandboxes: " .. said) return end

                         local ok, bad = sandboxes.save(s)

                         if not ok then said = tostring(bad) print("sandboxes: " .. said) return end

                         close()
                         refresh(true)
                         chosen = s.name
                         start(s.name)

                         if form.console.checked then open(s.name, 2) end

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
          sheet.panel.x + form.name.x + 20, sheet.panel.y + form.name.y + 14,
          sheet.panel.x + SW - 20 - 75, sheet.panel.y + SH - 33,
          sheet.panel.x + form.memory.x + 20, sheet.panel.y + form.memory.y + form.memory.h // 2,
          sheet.panel.x + form.network.x + form.network.w - 20, sheet.panel.y + form.network.y + form.network.h // 2,
          sheet.panel.x + form.port_field.x + 20, sheet.panel.y + form.port_field.y + 14))
  end
end

--------------------------------------------------------------------------
-- **Settings**, the fifth tab: the same form, on the window, filled with
-- the sandbox's own; Save writes its definition and says when it takes
-- effect - at its next start, or now with Save and restart.
--------------------------------------------------------------------------

local settings = { name = nil, form = nil, widgets = {} }

local function settings_close()
  for _, w in ipairs(settings.widgets) do win:remove(w) end

  settings = { name = nil, form = nil, widgets = {} }
end

local function settings_open(s)
  settings_close()
  settings.name = s.name

  local ox, oy = SIDE + 20, L.head + 110
  local function put(w) settings.widgets[#settings.widgets + 1] = w win:add(w) end

  settings.form = make_form(put, ox, oy, math.min(760, W - SIDE - 40), s, true)

  local by = oy + settings.form.h + 6
  local function save(and_restart)
    local d, why = settings.form.read(sandboxes.definition(s.name) or s)

    if not d then said = tostring(why) print("sandboxes: " .. said) return end

    d.name = s.name

    local ok, bad = sandboxes.save(d)

    if not ok then said = tostring(bad) print("sandboxes: " .. said) return end

    refresh(true)

    local running = s.now ~= nil

    said = running and not and_restart and (s.name .. " saved - it takes effect at its next start")
           or (s.name .. " saved")
    print(("sandboxes: %s settings saved, memory %d, network %s, ports %s, folders %d"):format(
          s.name, d.memory, d.network, table.concat(d.ports, " "), #d.folders))

    if TELL then
      local back = sandboxes.definition(s.name)

      print(("sandboxes: %s kept, memory %s"):format(s.name, back and back.memory or "?"))
    end

    if and_restart and running then restart(s.name) end
  end

  put(ui.button{ x = ox + 120, y = by, w = 90, h = 30, text = "Save", go = true, on_click = function() save(false) end })

  if s.now then
    put(ui.button{ x = ox + 220, y = by, w = 150, h = 30, text = "Save and restart",
                   on_click = function() save(true) end })
  end

  if TELL then
    print(("sandboxes: settings of %s, memory at %d,%d, save at %d,%d"):format(s.name,
          settings.form.memory.x + 20, settings.form.memory.y + settings.form.memory.h // 2, ox + 165, by + 15))
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
  apply_views()

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

--
-- **While a running sandbox is open, a pass every scheduler tick** - 4 ms - each
-- asking its monitor for what is new, and painting only when something
-- was; with none open, the window waits on its clock as before.
--
function win:on_frame()
  local want = (watched and tab == 5) and watched or nil

  if want ~= settings.name then
    if want and find(want) then settings_open(find(want)) else settings_close() end
  end

  local running = false

  for _, s in ipairs(all) do
    if s.name == watched and s.now then running = true end
  end

  self.poll_wait_ticks = running and 1 or nil

  return running and read_console()
end

win:run()
