-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Clock
-- kosmos: name Automations
-- kosmos: section applications
--
-- Automations: tasks strung together and run on a trigger, as drawn in
-- `docs/automations.html` (`docs/automations.md` A4).
--
-- On the left every automation, grouped by what starts it, each with when
-- it last ran and how it went, and a switch. On the right the one chosen:
-- **When** is its trigger and **Do** its steps in order, each a card that
-- says in a sentence what it does, its words in boxes - a press on one
-- changes it. After a run each card says how long its step took; Run now
-- lights them as they run. History shows every run, and a failure's
-- reason in words.
--
-- **The application is the thinnest layer there is.** What an automation
-- is, when it fires and what running one means are `automations.lua`'s;
-- running them on their triggers is `autod`'s, which this asks; pressing a
-- control is `automate.lua`'s; and the controls an application publishes
-- are the UI kit's. This shows and edits, and keeps the file.

local ui = use("/Kosmos/Libraries/ui.lua")
local automations = use("/Kosmos/Libraries/automations.lua")
local auto = use("/Kosmos/Libraries/automate.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local files = use("/Kosmos/Libraries/files.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local theme = ui.theme
local L = ui.layout

local W, H = 1120, 720
local SIDE_W = 290
local PAD = 22

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

local win, err = ui.window{ title = "Automations", w = W, h = H, centre = true, header = true,
                            resizable = true, tick_every = counter_hz // 4 }

if not win then
  print("automations: " .. tostring(err))
  return
end

W, H = win.w, win.h

--------------------------------------------------------------------------
-- What is shown: every automation, the one chosen, and what autod says.
--------------------------------------------------------------------------

local list = {}            -- { name =, a =, bad =, runs = }
local chosen = nil         -- a name
local status = {}          -- autod's, by name
local running = nil        -- { name, step } while autod runs one
local watching = nil       -- the name Run now started, until it ends
local sheet = nil

local GROUPS = {
  { id = "schedule", name = "ON A SCHEDULE", kinds = { at = true, every = true } },
  { id = "happens", name = "WHEN SOMETHING HAPPENS", kinds = { start = true, folder = true, drive = true } },
  { id = "hand", name = "BY HAND", kinds = { hand = true } },
}

local KIND_ICON = { at = "datetime", every = "recent", start = "startup", folder = "folder",
                    drive = "drive", hand = "run" }

local VERB_ICON = { open = "open", press = "mouse", menu = "menu", type = "keyboard",
                    choose = "textbox", set = "settings", read = "page", mail = "mail",
                    check = "globe", copy = "copy", move = "export", notify = "bell",
                    wait = "recent", wait_for = "recent", run = "terminal", stop_unless = "stop" }

local DAY_ORDER = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" }
local DAY_WORD = { mon = "Mon", tue = "Tue", wed = "Wed", thu = "Thu", fri = "Fri", sat = "Sat", sun = "Sun" }

local function load()
  list = {}

  for _, name in ipairs(automations.list()) do
    local a, why = automations.read(name)

    list[#list + 1] = { name = name, a = a, bad = not a and why or nil,
                        runs = automations.history(name) }
  end

  if chosen == nil or not (function()
    for _, e in ipairs(list) do if e.name == chosen then return true end end
  end)() then
    chosen = list[1] and list[1].name
  end
end

local function entry(name)
  for _, e in ipairs(list) do
    if e.name == name then return e end
  end
end

-- autod asked, on the window's clock and never in a paint.
local function ask_autod(req)
  local ok, r = pcall(fs.send, "/Running/autod", req)
  return ok and type(r) == "table" and r or nil
end

local function refresh_status()
  local r = ask_autod{ type = "status" }

  status = {}
  running = r and r.running or nil

  for _, s in ipairs(r and r.automations or {}) do status[s.name] = s end
  return r ~= nil
end

-- autod told to read the files again - started, when this is the first.
local function tell_autod()
  if ask_autod{ type = "reload" } then return true end

  if #automations.list() > 0 then
    local r = fs.send("/Running/wm", { type = "launch", program = "autod" })

    print("automations: autod started -> " .. tostring(r and r.ok))
  end
end

--------------------------------------------------------------------------
-- Words.
--------------------------------------------------------------------------

local function days_words(days)
  if not days or #days == 0 then return "every day" end

  local set = {}
  for _, d in ipairs(days) do set[d] = true end

  if #days == 5 and set.mon and set.tue and set.wed and set.thu and set.fri then return "weekdays" end
  if #days == 2 and set.sat and set.sun then return "weekends" end

  local out = {}
  for _, d in ipairs(DAY_ORDER) do if set[d] then out[#out + 1] = DAY_WORD[d] end end
  return table.concat(out, " ")
end

-- What starts it, as the list says it.
local function trigger_words(w)
  if not w then return "" end

  if w.kind == "at" then
    local d = days_words(w.days)
    return (d:sub(1, 1):upper() .. d:sub(2)) .. " at " .. tostring(w.time)
  end
  if w.kind == "every" then return ("Every %s min"):format(tostring(w.minutes)) end
  if w.kind == "start" then return "When Kosmos starts" end
  if w.kind == "folder" then return "A file in " .. tostring(w.path) end
  if w.kind == "drive" then
    return w.name and ("When the stick %s appears"):format(w.name) or "When a drive appears"
  end
  return "By hand"
end

local function time_of(epoch)
  local t = epoch and clock.at(epoch)
  return t and ("%d:%02d"):format(t.hour, t.min) or "?"
end

-- A day as the list says it: today, yesterday, tomorrow, or "2 Oct".
local function day_of(epoch)
  local now = clock.now()

  if not (epoch and now) then return "" end

  local offset = clock.offset()
  local d = (epoch + offset * 60) // 86400 - (now.epoch + offset * 60) // 86400

  if d == 1 then return "tomorrow" end
  return clock.day_word(epoch, now.epoch, offset)
end

-- How the last run went, and its colour.
local function last_words(e)
  local r = e.runs[#e.runs]

  if not r then return "not run yet", theme.text_dim end

  local when = day_of(r.at)
  local at = when ~= "today" and when or time_of(r.at)

  if not r.ok then return "failed " .. at, theme.bad end
  if r.stopped then return "ran " .. at .. ", nothing new", theme.good end
  return "ran " .. at .. ", done", theme.good
end

-- A step's sentence as parts: words, and `{ chip = }` the boxes a press
-- changes.
local function parts_of(s)
  local v = s.verb
  local P = {}

  local function w(t) P[#P + 1] = { text = t } end
  local function c(t) P[#P + 1] = { chip = tostring(t) } end

  if v == "open" then w("Open ") c(s.app)
  elseif v == "press" then w("Press ") c(tostring(s.app) .. " \u{203a} " .. tostring(s.control))
  elseif v == "menu" then w("Choose ") c(("%s \u{203a} %s \u{203a} %s"):format(s.app, s.menu, s.item))
  elseif v == "type" then w("Type ") c(s.text) w(" into ") c(tostring(s.app) .. " \u{203a} " .. tostring(s.control))
  elseif v == "choose" then w("Choose ") c(s.item or s.control) w(" in ") c(s.app)
  elseif v == "set" then w("Set ") c(tostring(s.app) .. " \u{203a} " .. tostring(s.control)) w(" to ") c(s.value)
  elseif v == "read" then w("Read ") c(tostring(s.app) .. " \u{203a} " .. tostring(s.control))
    w(" as ") c("{" .. (s.as or "value") .. "}")
  elseif v == "mail" then w("Send a mail to ") c(s.to) if s.subject then w(" about ") c(s.subject) end
  elseif v == "check" then w("Check ") c((tostring(s.url):gsub("^https?://", "")))
    if s.has then w(" for ") c(s.has) end
    if s.lacks then w(" lacking ") c(s.lacks) end
  elseif v == "copy" or v == "move" then w(v == "copy" and "Copy " or "Move ") c(s.from) w(" to ") c(s.to)
  elseif v == "notify" then w("Notify ") c(s.title)
  elseif v == "wait" then w("Wait ") c(tostring(s.seconds) .. " s")
  elseif v == "wait_for" then w("Wait for ") c(s.app)
  elseif v == "run" then w("Run ") c(s.program) if s.args then w(" with ") c(s.args) end
  elseif v == "stop_unless" then w("Stop unless ") c(s.text) w(" has ") c(s.has)
  else w(tostring(v)) end

  return P
end

-- The grey line under a card: what the step does, in a person's words.
local function note_of(s)
  local v = s.verb

  if v == "open" then return "Its window, as a click in the Deskbar would." end
  if v == "press" then return ("The button in %s, by its name - not a place on the screen."):format(s.app) end
  if v == "menu" then return "A menu item, chosen as a click would." end
  if v == "type" then return "Its words replaced by these." end
  if v == "read" then return "What it shows, for the steps after this one." end
  if v == "mail" then return "Through Mail's Outbox; maild sends it." end
  if v == "check" then return "Goes on only if the page has the words; finds {page title}." end
  if v == "copy" or v == "move" then return "Into the folder when it is one; finds {file}." end
  if v == "notify" then return "A banner, as any program's." end
  if v == "stop_unless" then return "The rest runs only when it has the words." end
  if v == "run" then return "A program, to its end." end
  return ""
end

--------------------------------------------------------------------------
-- Saving.
--------------------------------------------------------------------------

local function save(a, old_name)
  local ok, why = automations.write(a)

  if not ok then
    print("automations: not kept - " .. tostring(why))
    return false, why
  end

  if old_name and old_name ~= a.name then
    files.remove(automations.DIR .. "/" .. old_name .. ".automation")
    local h = automations.HISTORY .. "/" .. old_name

    if fs.getattr(h) then files.move(h, automations.HISTORY .. "/" .. a.name) end
  end

  print("automations: kept " .. a.name)
  chosen = a.name
  load()
  tell_autod()
  win:paint()
  return true
end

local function unique(base)
  local name, n = base, 1

  while entry(name) do
    n = n + 1
    name = base .. " " .. n
  end

  return name
end

--------------------------------------------------------------------------
-- The header.
--------------------------------------------------------------------------

local function counts()
  local on = 0

  for _, e in ipairs(list) do
    if e.a and e.a.on ~= false then on = on + 1 end
  end

  return ("%d automation%s, %d on"):format(#list, #list == 1 and "" or "s", on)
end

local open_step_sheet, open_when_sheet, open_history, add_step

local function run_now()
  local e = entry(chosen)

  if not e or not e.a then return end

  if not ask_autod{ type = "status" } then
    tell_autod()
    for _ = 1, 40 do
      if ask_autod{ type = "status" } then break end
      sys.sleep(5)
    end
  end

  local r = ask_autod{ type = "run", name = e.name }

  watching = r and r.ok and e.name or nil
  print(("automations: run now %s -> %s"):format(e.name, tostring(r and r.ok)))
end

local new_b = ui.button{ text = "+ New", on_click = function()
  local name = unique("New automation")

  save({ name = name, on = true, when = { kind = "hand" }, steps = {} })
end }

local run_b = ui.button{ text = "Run now", go = true, on_click = run_now }

local dots = ui.iconbutton{ icon = "more" }

dots.menu = function()
  local e = entry(chosen)

  return {
    { text = "Rename\u{2026}", disabled = not (e and e.a), on_choose = function()
      local s = ui.sheet{ title = "Rename", w = 380, h = 150 }
      local f = ui.field{ x = 18, y = 52, w = 344, text = e.name, hint = "Name" }
      s:put(f)
      s:put(ui.button{ x = 380 - 18 - 76 - 10 - 90, y = 100, w = 90, text = "Cancel",
                       on_click = function() s:close() sheet = nil end })
      s:put(ui.button{ x = 380 - 18 - 76, y = 100, w = 76, text = "Rename", go = true,
                       on_click = function()
                         local a = e.a
                         local old = a.name
                         a.name = f.text
                         if save(a, old) then s:close() sheet = nil else a.name = old end
                       end })
      sheet = s
      win:add(s)
      win:focus_on(f)
    end },
    { text = "Duplicate", disabled = not (e and e.a), on_choose = function()
      local copy = fs.read(automations.DIR .. "/" .. e.name .. ".automation")
      if type(copy) == "table" then
        copy.name = unique(e.name .. " copy")
        save(copy)
      end
    end },
    { text = "History", disabled = not e, on_choose = function() open_history() end },
    { separator = true },
    { text = "Delete", disabled = not e, on_choose = function()
      files.remove(automations.DIR .. "/" .. e.name .. ".automation")
      print("automations: deleted " .. e.name)
      chosen = nil
      load()
      tell_autod()
    end },
  }
end

dots.on_click = function()
  win:open_menu(win.origin_x + W - 140, win.origin_y + L.head, dots.menu())
end

local header = ui.header{ x = 0, y = 0, w = W, title = "Automations", sub = "",
                          right = { new_b, run_b, dots }, title_bar = true, window = win }

--------------------------------------------------------------------------
-- The list.
--------------------------------------------------------------------------

local ROW_H = 74

local side = ui.view{ x = 0, y = L.head, w = SIDE_W, h = H - L.head,
                      follow = { "left", "top", "bottom" } }
side.rows = {}

-- A switch as the kit draws one: a pill, its knob to the right when on.
local function draw_switch(g, x, y, on)
  g:fill_round(x, y, 30, 18, on and theme.accent or theme.line, 9)
  g:fill_round(on and (x + 14) or (x + 2), y + 2, 14, 14, 0xffffffff, 7)
end

function side:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)

  local y = 14

  self.rows = {}

  for _, group in ipairs(GROUPS) do
    local members = {}

    for _, e in ipairs(list) do
      local kind = e.a and e.a.when.kind or "hand"

      if group.kinds[kind] then members[#members + 1] = e end
    end

    if #members > 0 then
      g:text(18, y, group.name, theme.text_dim, nil, "label")
      y = y + 24

      for _, e in ipairs(members) do
        local on = e.a and e.a.on ~= false
        local here = e.name == chosen

        if here then g:fill_round(8, y, self.w - 16, ROW_H - 6, theme.mix(theme.sunken, theme.accent, 140), 8) end

        g:fill_round(18, y + 14, 28, 28, theme.raised, 7)
        g:line_icon(25, y + 21, e.a and KIND_ICON[e.a.when.kind] or "warning",
                    here and theme.accent or theme.text_dim)

        local tx, tw = 56, self.w - 56 - 52

        g:text(tx, y + 10, ui.fitted(e.name, tw, "ui"), theme.text, nil, "ui")

        if e.bad then
          g:text(tx, y + 30, ui.fitted(e.bad, tw, "label"), theme.bad, nil, "label")
        else
          local said, colour = last_words(e)
          local first = trigger_words(e.a.when) .. " \u{00b7} "

          g:text(tx, y + 30, ui.fitted(first, tw, "label"), theme.text_dim, nil, "label")
          g:text(tx, y + 48, ui.fitted(said, tw, "label"), colour, nil, "label")
          draw_switch(g, self.w - 48, y + 20, on)
        end

        self.rows[#self.rows + 1] = { y = y, h = ROW_H - 6, name = e.name,
                                      switch = not e.bad and { x = self.w - 52, y = y + 14, w = 40, h = 30 } }
        y = y + ROW_H
      end

      y = y + 8
    end
  end

  if #list == 0 then
    g:text(18, 20, "No automations yet.", theme.text_dim, nil, "ui")
    g:text(18, 44, "+ New makes one.", theme.text_dim, nil, "label")
  end
end

local function toggle(name)
  local e = entry(name)

  if not (e and e.a) then return end

  e.a.on = (e.a.on == false)
  print(("automations: %s %s"):format(name, e.a.on and "on" or "off"))
  save(e.a)
end

local function choose(name)
  chosen = name
  print("automations: chose " .. tostring(name))
  win:paint()
end

function side:mouse(action, x, y)
  if action ~= "release" then return true end

  for _, r in ipairs(self.rows) do
    if y >= r.y and y < r.y + r.h then
      local s = r.switch

      if s and x >= s.x and x < s.x + s.w and y >= s.y and y < s.y + s.h then
        toggle(r.name)
      else
        choose(r.name)
      end
      return true
    end
  end

  return true
end

--------------------------------------------------------------------------
-- The one chosen: its When and its Do.
--------------------------------------------------------------------------

local main = ui.view{ x = SIDE_W, y = L.head, w = W - SIDE_W, h = H - L.head,
                      follow = { "left", "right", "top", "bottom" } }
main.top = 0
main.links = {}

local CHIP_FILL = function() return theme.mix(theme.window, theme.accent, 170) end

-- Words and boxes along a line; returns where it ended.
local function sentence(g, x, y, parts)
  for _, p in ipairs(parts) do
    if p.chip then
      local tw = gfx.measure(p.chip, "mono")

      g:fill_round(x, y - 2, tw + 10, gfx.height("mono") + 4, CHIP_FILL(), 4)
      g:text(x + 5, y, p.chip, theme.text, nil, "mono")
      x = x + tw + 14
    else
      g:text(x, y, p.text, theme.text, nil, "ui")
      x = x + gfx.measure(p.text, "ui")
    end
  end

  return x
end

local function link(x, y, w, h, fn)
  main.links[#main.links + 1] = { x = x, y = y, w = w, h = h, fn = fn }
end

-- When it fires next, in words.
local function next_words(w)
  local now = clock.now()
  local t = now and automations.next_time(w, now, (status[chosen] or {}).last)

  if w.kind == "start" then return "Each time Kosmos starts, once the desktop is up." end
  if w.kind == "folder" then return "Looked at every five seconds; finds {file}." end
  if w.kind == "drive" then return "Looked for every five seconds; finds {drive}." end
  if w.kind == "hand" then return "Run now runs it, or automate automation at the prompt." end
  if not t then return "" end

  local at = time_of(t)

  if w.kind == "every" then return ("The next is at %s."):format(at) end
  return ("The next is %s at %s."):format(day_of(t), at)
end

function main:draw(g)
  local e = entry(chosen)
  local cw = self.w - 2 * PAD
  local y = 18 - self.top

  self.links = {}

  if not e then
    g:text(PAD, y + 10, "Nothing chosen.", theme.text_dim, nil, "title")
    return
  end

  -- The name, and what it has done.
  local NAME_PX = 24

  g:text(PAD, y - 4, e.name, theme.text, nil, "ui", NAME_PX)

  local sub = ("ran %d time%s"):format(#e.runs, #e.runs == 1 and "" or "s")

  g:text(PAD + gfx.measure(e.name, ui.sized("ui", NAME_PX)) + 12, y + 6, sub, theme.text_dim, nil, "label")
  y = y + 44

  if e.bad then
    g:text(PAD, y, e.bad, theme.bad, nil, "ui")
    return
  end

  local a = e.a

  -- When.
  g:text(PAD, y, "WHEN", theme.text_dim, nil, "label")
  y = y + 22
  g:fill_round(PAD, y, cw, 64, theme.raised, 10)
  g:frame_round(PAD, y, cw, 64, theme.mix(theme.raised, theme.accent, 450), 10)
  g:fill_round(PAD + 14, y + 14, 36, 36, theme.mix(theme.raised, theme.accent, 220), 8)
  g:line_icon(PAD + 24, y + 24, KIND_ICON[a.when.kind], theme.text, 15)

  local w = a.when
  local P = {}

  if w.kind == "at" then
    P = { { text = "At " }, { chip = w.time }, { text = " on " },
          { chip = (w.days and #w.days > 0) and (function()
              local out = {}
              local set = {}
              for _, d in ipairs(w.days) do set[d] = true end
              for _, d in ipairs(DAY_ORDER) do if set[d] then out[#out + 1] = DAY_WORD[d] end end
              return table.concat(out, " ")
            end)() or "every day" } }
  elseif w.kind == "every" then P = { { text = "Every " }, { chip = tostring(w.minutes) }, { text = " minutes" } }
  elseif w.kind == "start" then P = { { text = "When Kosmos starts" } }
  elseif w.kind == "folder" then P = { { text = "When a file appears in " }, { chip = w.path } }
  elseif w.kind == "drive" then P = { { text = "When " }, { chip = w.name or "a drive" }, { text = " appears" } }
  else P = { { text = "By hand" } } end

  sentence(g, PAD + 62, y + 13, P)
  g:text(PAD + 62, y + 36, next_words(w), theme.text_dim, nil, "label")

  local change_w = gfx.measure("Change", "ui") + 26

  g:fill_round(PAD + cw - change_w - 14, y + 16, change_w, 32, theme.window, 7)
  g:frame_round(PAD + cw - change_w - 14, y + 16, change_w, 32, theme.line, 7)
  g:text(PAD + cw - change_w - 1, y + 23, "Change", theme.text, nil, "ui")
  link(PAD, y, cw, 64, function() open_when_sheet() end)
  y = y + 84

  -- Do.
  g:text(PAD, y, "DO", theme.text_dim, nil, "label")
  y = y + 22

  local last = e.runs[#e.runs]
  local lit = running and running.name == e.name and running.step or nil

  for i, s in ipairs(a.steps) do
    local now_on = lit == i
    local fill = now_on and theme.mix(theme.raised, theme.accent, 140) or theme.raised

    g:fill_round(PAD, y, cw, 62, fill, 10)
    g:frame_round(PAD, y, cw, 62, now_on and theme.accent or theme.line_soft, 10)
    g:text(PAD + 16, y + 22, tostring(i), theme.text_dim, nil, "ui")
    g:fill_round(PAD + 40, y + 13, 36, 36, theme.mix(theme.raised, theme.accent, 180), 8)
    g:line_icon(PAD + 50, y + 23, VERB_ICON[s.verb] or "run", theme.text, 15)
    sentence(g, PAD + 88, y + 12, parts_of(s))
    g:text(PAD + 88, y + 36, ui.fitted(note_of(s), cw - 200, "label"), theme.text_dim, nil, "label")

    local r = last and last.steps and last.steps[i]

    if r and not now_on then
      local said = r.ok and ("%.1f s \u{2713}"):format((r.ms or 0) / 1000) or "failed \u{2717}"

      g:text(PAD + cw - 16 - gfx.measure(said, "label"), y + 23, said,
             r.ok and theme.good or theme.bad, nil, "label")
    elseif now_on then
      g:text(PAD + cw - 16 - gfx.measure("running", "label"), y + 23, "running", theme.accent, nil, "label")
    end

    local n = i
    link(PAD, y, cw, 62, function() open_step_sheet(n) end)
    y = y + 70
  end

  -- Add a step.
  local add_words = "+ Add a step  \u{00b7}  Open, Press, Type, Send a mail, Check a page, Copy, Move, Notify, Wait, Run a program, Stop unless\u{2026}"

  g:frame_round(PAD, y, cw, 44, theme.line, 10)
  g:text(PAD + 16, y + 13, ui.fitted(add_words, cw - 32, "ui"), theme.text_dim, nil, "ui")
  link(PAD, y, cw, 44, function() add_step() end)
  y = y + 60

  -- The last run.
  if last then
    local done = 0
    for _, st in ipairs(last.steps or {}) do if st.ok then done = done + 1 end end

    local said = last.ok and (last.stopped and ("Last run %s at %s: stopped - %s."):format(
                   day_of(last.at), time_of(last.at), tostring(last.why))
                   or ("Last run %s at %s: %d step%s, all done, in %.1f s."):format(
                   day_of(last.at), time_of(last.at),
                   done, done == 1 and "" or "s", (last.ms or 0) / 1000))
                 or ("Last run %s at %s failed: %s"):format(day_of(last.at),
                   time_of(last.at), tostring(last.why))

    g:text(PAD, y, ui.fitted(said, cw - 90, "label"), last.ok and theme.text_dim or theme.bad, nil, "label")
  else
    g:text(PAD, y, "Not run yet.", theme.text_dim, nil, "label")
  end

  local hw = gfx.measure("History", "label")

  g:text(PAD + cw - hw, y, "History", theme.accent, nil, "label")
  link(PAD + cw - hw - 6, y - 4, hw + 12, 24, function() open_history() end)

  self.content_h = y + self.top + 40
end

function main:mouse(action, x, y)
  if action ~= "release" then return true end

  for _, l in ipairs(self.links) do
    if x >= l.x and x < l.x + l.w and y >= l.y and y < l.y + l.h then
      l.fn()
      win:paint()
      return true
    end
  end

  return true
end

function main:wheel(n)
  local most = math.max(0, (self.content_h or 0) - self.h)

  self.top = math.max(0, math.min(most, self.top - n * 40))
  return true
end

-- On the window's clock: what autod is doing, and the history when a run
-- that was watched has ended.
function main:tick()
  local was = running

  refresh_status()

  if was and not running then
    load()
    if watching then
      local e = entry(watching)
      local r = e and e.runs[#e.runs]

      print(("automations: %s ran - %s"):format(watching, r and (r.ok and (r.stopped and "stopped" or "done")
            or ("failed: " .. tostring(r.why))) or "?"))
      watching = nil
    end
  end

  header.sub = counts()
end

--------------------------------------------------------------------------
-- The sheets: a step's words, the trigger, the controls picker, history.
--------------------------------------------------------------------------

local FIELD_NAMES = {
  app = "Application", control = "Control", menu = "Menu", item = "Item", text = "Text",
  value = "Value", to = "To", subject = "Subject", url = "Address", has = "Has the words",
  lacks = "Lacks the words", from = "From", title = "Title", body = "Line under it",
  seconds = "Seconds", program = "Program", args = "Its words", as = "Found as", file = "File",
}

-- Each verb's words, the ones it needs first.
local VERB_FIELDS = {
  open = { "app", "file" }, press = { "app", "control" }, menu = { "app", "menu", "item" },
  type = { "app", "control", "text" }, choose = { "app", "control", "item" },
  set = { "app", "control", "value" }, read = { "app", "control", "as" },
  mail = { "to", "subject", "text" }, check = { "url", "has", "lacks" },
  copy = { "from", "to" }, move = { "from", "to" }, notify = { "title", "body" },
  wait = { "seconds" }, wait_for = { "app", "seconds" }, run = { "program", "args" },
  stop_unless = { "text", "has" },
}

local VERB_NAMES = {
  { "open", "Open an application" }, { "press", "Press a control" }, { "menu", "Choose a menu item" },
  { "type", "Type into a field" }, { "choose", "Choose a tab or an item" }, { "set", "Set a switch" },
  { "read", "Read a control" }, { "mail", "Send a mail" }, { "check", "Check a page" },
  { "copy", "Copy" }, { "move", "Move" }, { "notify", "Notify" }, { "wait", "Wait" },
  { "wait_for", "Wait for an application" }, { "run", "Run a program" }, { "stop_unless", "Stop unless" },
}

local open_picker

function open_step_sheet(i, fresh)
  local e = entry(chosen)
  local step = e and e.a and e.a.steps[i]

  if not step then return end

  local names = VERB_FIELDS[step.verb] or {}
  local sh = 100 + #names * 62
  local s = ui.sheet{ title = automations.said(step), w = 520, h = sh }
  local fields = {}

  for j, name in ipairs(names) do
    local y = 50 + (j - 1) * 62

    s:put(ui.label{ x = 18, y = y, text = FIELD_NAMES[name] or name, color = "text_dim" })

    local f = ui.field{ x = 18, y = y + 20, w = (name == "control" or name == "item") and 380 or 484,
                        text = step[name] ~= nil and tostring(step[name]) or "",
                        hint = FIELD_NAMES[name] or name }

    fields[name] = f
    s:put(f)

    if name == "control" then
      s:put(ui.button{ x = 410, y = y + 20, w = 92, text = "Pick\u{2026}", on_click = function()
        open_picker(fields.app and fields.app.text, function(app, address)
          if fields.app then fields.app.text = app end
          f.text = address
        end)
      end })
    end
  end

  local function close() s:close() sheet = nil end

  s:put(ui.button{ x = 18, y = sh - 48, w = 90, text = "Remove", on_click = function()
    table.remove(e.a.steps, i)
    save(e.a)
    close()
  end })
  s:put(ui.button{ x = 520 - 18 - 76 - 10 - 90, y = sh - 48, w = 90, text = "Cancel", on_click = function()
    if fresh then table.remove(e.a.steps, i) save(e.a) end
    close()
  end })
  s:put(ui.button{ x = 520 - 18 - 76, y = sh - 48, w = 76, text = "Keep", go = true, on_click = function()
    for name, f in pairs(fields) do
      local t = f.text

      if t == "" then
        step[name] = nil
      elseif name == "seconds" then
        step[name] = tonumber(t) or t
      else
        step[name] = t
      end
    end

    local ok, why = automations.check(e.a)

    if not ok then
      print("automations: " .. tostring(why))
      s.title = why
      return
    end

    save(e.a)
    close()
  end })

  sheet = s
  win:add(s)
  if names[1] then win:focus_on(fields[names[1]]) end
  print(("automations: editing step %d, %s"):format(i, step.verb))
end

-- The verbs a step can be, as the menu offers them - and published as
-- `menu/Add a step/<verb>`, so the menu need not be open to choose one.
local function add_items()
  local e = entry(chosen)

  if not (e and e.a) then return {} end

  local items = {}

  for _, v in ipairs(VERB_NAMES) do
    items[#items + 1] = { text = v[2], on_choose = function()
      local step = { verb = v[1] }

      -- Placeholders a person replaces in the sheet that opens.
      for _, name in ipairs(automations.VERBS[v[1]] or {}) do step[name] = "" end

      e.a.steps[#e.a.steps + 1] = step
      open_step_sheet(#e.a.steps, true)
    end }
  end

  return items
end

function add_step()
  local items = add_items()

  if #items == 0 then return end

  local m = win:open_menu(win.origin_x + SIDE_W + PAD, win.origin_y + win.h // 2, items)

  if m then print(("automations: add a step, %d verbs"):format(#items)) end
end

-- The trigger: what starts it, and its words.
function open_when_sheet()
  local e = entry(chosen)

  if not (e and e.a) then return end

  local w = e.a.when
  local s = ui.sheet{ title = "When it runs", w = 520, h = 330 }
  local kind = w.kind
  local KINDS = { { "at", "At a time" }, { "every", "Every so many minutes" },
                  { "start", "When Kosmos starts" }, { "folder", "When a file appears in a folder" },
                  { "drive", "When a drive appears" }, { "hand", "By hand" } }
  local time = ui.field{ x = 18, y = 120, w = 120, text = w.time or "8:00", hint = "8:00", name = "Time" }
  local minutes = ui.field{ x = 18, y = 120, w = 120, text = tostring(w.minutes or 30), hint = "30",
                            name = "Minutes" }
  local path = ui.field{ x = 18, y = 120, w = 484, text = w.path or "/Home/Downloads",
                         hint = "/Home/Downloads", name = "Folder" }
  local drive = ui.field{ x = 18, y = 120, w = 240, text = w.name or "", hint = "Any drive", name = "Drive" }
  local set = {}

  for _, d in ipairs(w.days or {}) do set[d] = true end

  local boxes = {}

  for j, d in ipairs(DAY_ORDER) do
    boxes[d] = ui.checkbox{ x = 18 + (j - 1) * 68, y = 170, text = DAY_WORD[d], checked = set[d] or false }
  end

  local label = ui.label{ x = 18, y = 100, text = "", color = "text_dim" }

  local function show()
    time.hidden = kind ~= "at"
    minutes.hidden = kind ~= "every"
    path.hidden = kind ~= "folder"
    drive.hidden = kind ~= "drive"
    for _, b in pairs(boxes) do b.hidden = kind ~= "at" end
    label.text = ({ at = "The time, and the days - none ticked is every day", every = "Minutes",
                    folder = "The folder", drive = "The drive's name" })[kind] or ""
  end

  s:put(ui.label{ x = 18, y = 46, text = "What starts it", color = "text_dim" })
  s:put(ui.dropdown{ x = 18, y = 64, w = 484, choices = KINDS, value = kind, name = "What starts it",
                     on_change = function(_, v) kind = v show() end })

  for _, v in ipairs({ label, time, minutes, path, drive }) do s:put(v) end
  for _, d in ipairs(DAY_ORDER) do s:put(boxes[d]) end

  local function close() s:close() sheet = nil end

  s:put(ui.button{ x = 520 - 18 - 76 - 10 - 90, y = 282, w = 90, text = "Cancel", on_click = close })
  s:put(ui.button{ x = 520 - 18 - 76, y = 282, w = 76, text = "Keep", go = true, on_click = function()
    local new = { kind = kind }

    if kind == "at" then
      new.time = time.text
      new.days = {}
      for _, d in ipairs(DAY_ORDER) do if boxes[d].checked then new.days[#new.days + 1] = d end end
    elseif kind == "every" then
      new.minutes = tonumber(minutes.text)
    elseif kind == "folder" then
      new.path = path.text
    elseif kind == "drive" then
      new.name = drive.text ~= "" and drive.text or nil
    end

    local old = e.a.when
    e.a.when = new

    local ok, why = automations.check(e.a)

    if not ok then
      e.a.when = old
      s.title = why
      print("automations: " .. tostring(why))
      return
    end

    save(e.a)
    close()
  end })

  show()
  sheet = s
  win:add(s)
  print("automations: when, " .. kind)
end

-- The open applications and what each publishes, for a step's control.
function open_picker(app_name, chose)
  local names = {}

  for _, w in ipairs((wmproto.windows() or {}).windows or {}) do
    local t = tostring(w.title or "")

    if t ~= "" and t ~= "Automations" and t ~= "Deskbar" then names[#names + 1] = t end
  end

  local current = 1

  for i, n in ipairs(names) do
    if app_name and auto.names(n, app_name) then current = i end
  end

  local p = ui.sheet{ title = "Press a control \u{00b7} in an application", w = 640, h = 480 }
  local rows = {}
  local picked = nil

  local box = ui.view{ x = 18, y = 96, w = 604, h = 320 }

  local function fill()
    rows = {}
    picked = nil

    local a = names[current] and auto.app(names[current])
    local all = a and a:controls() or {}

    for _, c in ipairs(all) do
      if not c.disabled then rows[#rows + 1] = c end
    end
  end

  function box:draw(g)
    g:fill_round(0, 0, self.w, self.h, theme.sunken, 8)

    local y = 8

    for i, c in ipairs(rows) do
      if y + 26 > self.h then break end

      if picked == i then g:fill_round(4, y - 3, self.w - 8, 26, theme.mix(theme.sunken, theme.accent, 250), 6) end

      local label = c.address:sub(#c.kind + 2):gsub("/", " \u{203a} ")

      g:text(14, y, ui.fitted(label, 300, "ui"), theme.text, nil, "ui")
      g:text(320, y + 2, ui.fitted("controls/" .. c.address, self.w - 334, "label"), theme.text_dim, nil, "label")
      y = y + 28
    end

    if #rows == 0 then g:text(14, 10, "Nothing it publishes can be pressed.", theme.text_dim, nil, "ui") end
  end

  function box:mouse(action, _, y)
    if action == "release" then
      local i = (y - 5) // 28 + 1

      if rows[i] then picked = i end
    end
    return true
  end

  local seg = ui.segments{ x = 18, y = 52, items = names, on = current,
                           on_change = function(_, i) current = i fill() end }

  p:put(seg)
  p:put(box)
  p:put(ui.button{ x = 640 - 18 - 86 - 10 - 90, y = 432, w = 90, text = "Cancel",
                   on_click = function() p:close() end })
  p:put(ui.button{ x = 640 - 18 - 86, y = 432, w = 86, text = "Choose", go = true, on_click = function()
    if picked and rows[picked] then chose(names[current], rows[picked].address) end
    p:close()
  end })

  fill()
  win:add(p)
  print(("automations: picker, %d applications, %d controls in %s"):format(#names, #rows,
        tostring(names[current])))
end

function open_history()
  local e = entry(chosen)

  if not e then return end

  local s = ui.sheet{ title = e.name .. " \u{00b7} history", w = 640, h = 460 }
  local box = ui.view{ x = 18, y = 50, w = 604, h = 350 }

  function box:draw(g)
    local y = 4
    local runs = e.runs

    if #runs == 0 then
      g:text(4, y, "It has not run yet.", theme.text_dim, nil, "ui")
      return
    end

    for i = #runs, 1, -1 do
      local r = runs[i]

      if y + 56 > self.h then break end

      local day = day_of(r.at)
      local first = r.steps and r.steps[1]
      local what = first and first.said or "no steps"
      local said = r.ok and (r.stopped and ("Stopped: " .. tostring(r.why))
                             or ("%d step%s, %.1f s"):format(#r.steps, #r.steps == 1 and "" or "s", (r.ms or 0) / 1000))
                   or ("Failed: " .. tostring(r.why))

      g:text(4, y, ("%s %s"):format(day, time_of(r.at)), theme.text_dim, nil, "label")
      g:text(120, y, ui.fitted(what, self.w - 160, "ui"), theme.text, nil, "ui")
      g:text(120, y + 22, ui.fitted(said, self.w - 160, "label"), r.ok and theme.text_dim or theme.bad, nil, "label")
      g:text(self.w - 20, y, r.ok and "\u{2713}" or "\u{2717}", r.ok and theme.good or theme.bad, nil, "ui")
      y = y + 56
    end
  end

  s:put(box)
  s:put(ui.button{ x = 640 - 18 - 76, y = 412, w = 76, text = "Done", go = true,
                   on_click = function() s:close() sheet = nil end })
  sheet = s
  win:add(s)
  print(("automations: history of %s, %d runs"):format(e.name, #e.runs))
end

--------------------------------------------------------------------------
-- Published by name (`window:publish_controls`): the list's rows and
-- switches, and each step - so the window can be driven as any other.
--------------------------------------------------------------------------

win:publish_controls(function()
  local out = {}

  for _, e in ipairs(list) do
    local name = e.name

    out[#out + 1] = { kind = "row", label = name, press = function() choose(name) end,
                      read = function() return last_words(e) end }

    if e.a then
      out[#out + 1] = { kind = "switch", label = name,
                        read = function() return e.a.on ~= false and "on" or "off" end,
                        write = function(v)
                          local on = tostring(v):lower() == "on"
                          if on ~= (e.a.on ~= false) then toggle(name) end
                        end,
                        press = function() toggle(name) end }
    end
  end

  local e = entry(chosen)

  for i, s in ipairs(e and e.a and e.a.steps or {}) do
    out[#out + 1] = { kind = "step", label = tostring(i), read = function() return automations.said(s) end,
                      press = function() open_step_sheet(i) end }
  end

  out[#out + 1] = { kind = "button", label = "Change", press = function() open_when_sheet() end }
  out[#out + 1] = { kind = "button", label = "Add a step", press = function() add_step() end }

  for _, c in ipairs(ui.menu_controls("Add a step", add_items())) do out[#out + 1] = c end
  out[#out + 1] = { kind = "button", label = "History", press = function() open_history() end }
  return out
end)

for _, v in ipairs({ side, main, header }) do win:add(v) end

load()
refresh_status()
header.sub = counts()
win:focus_on(main)
print(("automations: window at %d,%d, %d listed, autod %s"):format(win.origin_x or 0, win.origin_y or 0,
      #list, next(status) and "running" or (ask_autod{ type = "status" } and "running" or "not running")))

win:run()
