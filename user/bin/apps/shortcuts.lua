-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Misc_Book
-- kosmos: name Shortcuts
-- kosmos: needs screen
--
-- Shortcuts: every key that works everywhere in Kosmos, and the ones a
-- person makes (`docs/shortcuts.md`, drawn in `docs/shortcuts.html`).
--
--   Super + / , the dock or the Kosmos menu; Super + º over everything
--
-- **Global keys, Super's.** What is listed here is what the window manager
-- answers before any application sees it - the desktop's, the windows', the
-- screen's and text editing's - asked of the window manager, which is the
-- one process that decides them (`wmproto.shortcuts`). An application's own
-- keys are Control's and its own business, and are not here.
--
-- **Made and changed here, kept as a setting.** Add a shortcut opens a sheet
-- that listens for the keys - the window manager posts them here rather
-- than acting on them - and asks what they do; Change does the same for a
-- row. What differs from the window manager's own is saved as
-- `/Home/Preferences/shortcuts` and handed to the window manager, which
-- reads the same file when it starts.
--
-- Its keys: Control-N adds a shortcut, the arrows go along the rows and
-- Enter changes the one chosen, Escape stops listening or puts the sheet
-- away. Control, because a window's own keys are Control's.
--

local ui = use("/Kosmos/Libraries/ui.lua")
local keys = use("/Kosmos/Libraries/keys.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local prefs = use("/Kosmos/Libraries/prefs.lua")
local menu = use("/Kosmos/Libraries/deskbarmenu.lua")
local theme = ui.theme

local W, H = 900, 600
local SIDE_W = 236
local FOOT_H = 46
local L = ui.layout

--
-- **Modal, from Super and º** (Diego, 3 October 2026): the same window as a
-- popup in the middle of the screen, over everything, closed by Escape or a
-- press anywhere outside it.
--
local MODAL = tostring(args or ""):match("%-%-modal") ~= nil
local spec = { title = "Shortcuts", w = W, h = H, centre = true, header = true }

if MODAL then
  local screen = gfx.screen()
  local sw, sh = 1920, 1080

  if screen then sw, sh = screen:size() end

  spec = { title = "Shortcuts", w = W, h = H, popup = true, header = true,
           x = (sw - W) // 2, y = (sh - H) // 2 }
end

local win, err = ui.window(spec)

if not win then
  print("shortcuts: " .. tostring(err))
  return
end

W, H = win.w, win.h

--------------------------------------------------------------------------
-- What there is.
--------------------------------------------------------------------------

-- The sidebar's groups, as the drawing has them, each with its picture.
local GROUPS = {
  { id = "all",     name = "All",         picture = "Misc_Book.png" },
  { id = "desktop", name = "The desktop", picture = "App_Launchbox.png" },
  { id = "windows", name = "Windows",     picture = "Misc_Deskbar_Group.png" },
  { id = "screen",  name = "The screen",  picture = "App_ShowImage.png" },
  { id = "text",    name = "Text",        picture = "File_Text.png" },
}
local YOURS = { id = "yours", name = "Yours", picture = "Misc_Book.png" }
local GROUP = { yours = YOURS }

for _, g in ipairs(GROUPS) do GROUP[g.id] = g end

local rows = {}             -- every row, as the window manager says them
local apps = {}             -- what a shortcut can open: { path, name }
local chosen = "desktop"    -- the sidebar's row
local query = ""            -- what the search holds

-- An application's name, as the menu shows it.
local function name_of(program)
  for _, a in ipairs(apps) do
    if a.path == program then return a.name end
  end

  local stem = tostring(program):match("([^/]+)%.lua$") or tostring(program)

  return stem:sub(1, 1):upper() .. stem:sub(2)
end

local function load_apps()
  apps = {}

  for short, attrs in pairs(menu.programs(fs) or {}) do
    -- Its name for a person is `title` (`kosmos: name`), the file's when
    -- it declares none.
    local title = (attrs.title and attrs.title ~= "") and attrs.title
                  or (short:sub(1, 1):upper() .. short:sub(2))

    apps[#apps + 1] = { path = "/Kosmos/Apps/" .. short .. ".lua", name = tostring(title) }
  end

  table.sort(apps, function(a, b) return a.name:lower() < b.name:lower() end)
end

-- Every row from the window manager, a person's own given their words.
local function load()
  rows = wmproto.shortcuts() or {}

  for _, r in ipairs(rows) do
    if r.group == "yours" then r.what = "Open " .. name_of(r.program) end
  end
end

local function count(group)
  local n = 0

  for _, r in ipairs(rows) do
    if group == "all" and r.group ~= "yours" or r.group == group then n = n + 1 end
  end

  return n
end

-- Whether a row is one this window may change: a Super key the window
-- manager says can move, or a person's own.
local function changeable(r)
  return r.group == "yours" or (r.action and not r.fixed)
end

--------------------------------------------------------------------------
-- Search: every word of the query in what a row does or in its keys, a
-- word for a key spelled as the key it names ("ctrl" is Ctrl, "cmd" Super).
--------------------------------------------------------------------------

local function words_of(q)
  local out = {}

  for w in tostring(q):lower():gmatch("%S+") do out[#out + 1] = w end

  return out
end

local function caps_words(chord)
  local out = {}

  for _, step in ipairs(keys.caps(chord or "") or {}) do
    for _, c in ipairs(step) do out[#out + 1] = c:lower() end
  end

  return out
end

-- A word that names a key - a modifier, or one character - is looked for
-- among the keys only: "ctrl w" is Control and W, not every row with a w in
-- its words (the first version found Super Ctrl drag that way).
local function names_a_key(w)
  return keys.spelling(w) ~= w:lower() or utf8.len(w) == 1
         or ({ super = 1, ctrl = 1, alt = 1, shift = 1, tab = 1, space = 1 })[w:lower()] ~= nil
end

local function matches(r, words)
  local what = tostring(r.what or ""):lower()
  local caps = caps_words(r.keys)

  for _, w in ipairs(words) do
    local key = keys.spelling(w)
    local found = false

    for _, c in ipairs(caps) do
      if c == key then found = true end
    end

    if not found and not names_a_key(w) then found = what:find(w, 1, true) ~= nil end

    if not found then return false end
  end

  return true
end

-- What the main side shows: a list of sections, each a group and its rows.
local function sections()
  local out = {}
  local words = words_of(query)
  local order = { "desktop", "windows", "screen", "text", "yours" }

  local function section(id, test)
    local list = {}

    for _, r in ipairs(rows) do
      if r.group == id and test(r) then list[#list + 1] = r end
    end

    if #list > 0 or (id == "yours" and #words == 0) then
      out[#out + 1] = { group = GROUP[id], rows = list }
    end
  end

  if #words > 0 then
    for _, id in ipairs(order) do
      section(id, function(r) return matches(r, words) end)
    end
  elseif chosen == "all" then
    for _, id in ipairs(order) do
      if id ~= "yours" then section(id, function() return true end) end
    end
  else
    section(chosen, function() return true end)
  end

  return out, words
end

--------------------------------------------------------------------------
-- Changing: the row listening, the chord it heard, and what it would take.
--------------------------------------------------------------------------

local editing = nil         -- { row = r, heard = chord }  while a row listens
local sheet = nil           -- the Add a shortcut sheet, while it is shown
local status = ""           -- what the footer says while something listens

local function listen(on)
  local r, why = fs.send("/Running/wm", { type = "keys", listen = on and win.handle or false })

  if on and not r then print("shortcuts: could not listen: " .. tostring(why)) end

  return r ~= nil
end

-- Who has `chord` now, other than `except`.
local function holder(chord, except)
  for _, r in ipairs(rows) do
    if r ~= except and r.keys == chord and chord ~= "" then return r end
  end

  return nil
end

-- What the keys do now, in words, for a sentence about them.
local function does(r)
  local what = tostring(r.what or "")

  return what:sub(1, 1):lower() .. what:sub(2)
end

local function spaced(chord)
  return (tostring(chord):gsub("%+", " "):gsub(",", " then "))
end

-- The setting, from the rows: each Super key whose keys differ from the
-- window manager's own, and each of a person's own. Saved, then handed to
-- the window manager, which applies it at once.
local function save()
  local entries = {}

  for _, r in ipairs(rows) do
    if r.group == "yours" then
      entries[#entries + 1] = { keys = r.keys, action = "open", program = r.program }
    elseif r.action and not r.fixed and r.keys ~= r.default then
      entries[#entries + 1] = { keys = r.keys, action = r.action }
    end
  end

  local ok, why = prefs.write("shortcuts", entries)

  if not ok then print("shortcuts: not saved: " .. tostring(why)) end

  fs.send("/Running/wm", { type = "keys", shortcuts = entries })
  print(("shortcuts: saved, %d entries"):format(#entries))
  load()
end

-- `chord` given to `r`, and whoever had it left without keys - a person's
-- own taken off altogether, since a shortcut without keys does nothing.
local function assign(r, chord)
  local other = holder(chord, r)

  if other then
    if other.group == "yours" then
      for i, x in ipairs(rows) do
        if x == other then table.remove(rows, i) break end
      end
    else
      other.keys = ""
    end
  end

  r.keys = chord
  save()
end

-- What the heard keys mean for `r`: free, someone else's that can be
-- taken, or a key that stays where it is.
local function verdict(r, chord)
  local other = holder(chord, r)

  if not other then return "free", ("%s is free."):format(spaced(chord)) end

  if other.fixed or not changeable(other) then
    return "fixed", ("%s is for %s, which stays."):format(spaced(chord), does(other))
  end

  -- "Super M already opens Mail, one of yours" - the drawing's sentence.
  if other.group == "yours" then
    return "taken", ("%s already opens %s, one of yours."):format(spaced(chord),
                                                              name_of(other.program)), other
  end

  return "taken", ("%s is already for %s."):format(spaced(chord), does(other)), other
end

local function stop_editing()
  if editing then
    editing = nil
    listen(false)
  end

  status = ""
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

-- The book before the title, as the drawing has it.
local mark = ui.view{ x = 0, y = 0, w = 22, h = 22 }

function mark:draw(g) g:icon(0, 0, "Misc_Book.png", 22) end

local main             -- the keys, drawn below
local search = ui.field{ w = 340, text = "", hint = "Search every shortcut", icon = "search",
                         on_change = function(_, text)
                           query = text or ""
                           if main then main.top = 0 end
                         end }

local header = ui.header{ x = 0, y = 0, w = W, title = "Shortcuts",
                          left = { mark }, after = { search }, title_bar = true,
                          window = win }

local side = ui.sidebar{ x = 0, y = L.head + 10, w = SIDE_W, h = H - L.head - 10,
                         filled = true, follow = { "left", "top", "bottom" },
                         on_select = function(_, id)
                           chosen = id
                           stop_editing()
                           if main then main.top = 0 end
                           print("shortcuts: chose " .. tostring(id))
                         end }

local function side_items()
  local items = { { heading = true, name = "KOSMOS", id = "#kosmos" } }

  for _, g in ipairs(GROUPS) do
    items[#items + 1] = { id = g.id, name = g.name, picture = g.picture,
                          note = tostring(count(g.id)) }
  end

  items[#items + 1] = { gap = true }
  items[#items + 1] = { heading = true, name = "MADE HERE", id = "#made" }
  items[#items + 1] = { id = "yours", name = "Yours", picture = YOURS.picture,
                        note = tostring(count("yours")) }

  return items
end

-- The column the sidebar sits in, a shade apart from the keys, with its
-- rule: the drawing's two halves.
local column = ui.view{ x = 0, y = L.head, w = SIDE_W, h = H - L.head,
                        follow = { "left", "top", "bottom" } }

function column:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
end

--------------------------------------------------------------------------
-- The keys: each group's title with its picture, and its rows - what a key
-- does on the left, the keys themselves on the right, every other row a
-- shade lighter, and Change beside a key that can be changed.
--------------------------------------------------------------------------

local PAD, ROW_GAP = 20, 0

main = ui.view{ x = SIDE_W, y = L.head, w = W - SIDE_W, h = H - L.head - FOOT_H,
                follow = { "left", "right", "top", "bottom" } }
main.focusable = true
main.top = 0
main.links = {}            -- what was drawn where, for a press
main.at = 0                -- the row the keyboard is on

local said = nil

local function row_h() return ui.keycaps_height() + 14 end

-- The rows in the order they are drawn, for the keyboard.
local function drawn_rows()
  local out = {}

  for _, s in ipairs((sections())) do
    for _, r in ipairs(s.rows) do out[#out + 1] = r end
  end

  return out
end

-- The matched words lit behind the text, for a search.
local function lit(g, x, y, text, words)
  local low = text:lower()

  for _, w in ipairs(words) do
    local from = 1

    while true do
      local a, b = low:find(w, from, true)

      if not a then break end

      local px = gfx.measure(text:sub(1, a - 1))
      local pw = gfx.measure(text:sub(a, b))

      g:fill_round(x + px - 1, y - 1, pw + 2, gfx.height() + 2, theme.mix(theme.accent,
                   theme.window, 650), 3)
      from = b + 1
    end
  end
end

function main:draw(g)
  local list, words = sections()
  local y = 16 - self.top
  local rh = row_h()
  local ox, oy = self:in_window()
  local screen_x, screen_y = win.origin_x + ox, win.origin_y + oy
  local log = {}

  self.links = {}

  for _, s in ipairs(list) do
    g:icon(PAD, y + 2, s.group.picture, 18)
    g:text(PAD + 26, y + 2, s.group.name, theme.text, nil, "label")
    y = y + 34

    for i, r in ipairs(s.rows) do
      local is_edit = editing and editing.row == r
      local at_key = self.keyed and drawn_rows()[self.at] == r

      if is_edit then
        -- The drawing's faint tint of the accent, and its outline.
        g:fill_round(PAD - 6, y, self.w - 2 * PAD + 12, rh, theme.mix(theme.accent,
                     theme.window, 840), 7)
        g:frame_round(PAD - 6, y, self.w - 2 * PAD + 12, rh, theme.accent, 7)
      elseif i % 2 == 0 then
        g:fill_round(PAD - 6, y, self.w - 2 * PAD + 12, rh, theme.line_soft, 7)
      end

      if at_key and not is_edit then
        g:frame_round(PAD - 6, y, self.w - 2 * PAD + 12, rh, theme.ring, 7)
      end

      -- What it does, and its note in the dim face beside it.
      local tx, ty = PAD + 6, y + (rh - gfx.height()) // 2
      local what = tostring(r.what or "")

      if #words > 0 then
        local plain = {}

        for _, w in ipairs(words) do
          if not names_a_key(w) then plain[#plain + 1] = w end
        end

        lit(g, tx, ty, what, plain)
      end

      g:text(tx, ty, what, theme.text)

      local note = r.note

      if r.default and r.keys ~= r.default and not is_edit then
        note = "changed from " .. spaced(r.default)
      end

      if note then
        g:text(tx + gfx.measure(what) + 10, ty + 1, note, theme.text_dim, nil,
               nil)
      end

      -- At the right: the link, then the keys before it.
      local right = self.w - PAD
      local link = nil

      if is_edit then
        link = { word = r.group == "yours" and "Remove" or "Default", kind = "default" }
      elseif changeable(r) then
        link = { word = "Change", kind = "change" }
      end

      if link then
        local lw = gfx.measure(link.word, nil)

        g:text(right - lw, ty + 1, link.word, theme.accent, nil, nil)
        self.links[#self.links + 1] = { x = right - lw - 4, y = y, w = lw + 8, h = rh,
                                        row = r, kind = link.kind }
        log[#log + 1] = ("%s %s at %d,%d"):format(link.kind, r.action or r.keys,
                        screen_x + right - lw // 2, screen_y + y + rh // 2)
        right = right - lw - 12
      end

      local chord = is_edit and editing.heard or r.keys
      local ky = y + (rh - ui.keycaps_height()) // 2

      if chord and chord ~= "" then
        local cw = ui.keycaps_width(chord)

        g:keycaps(right - cw, ky, chord)

        if is_edit then
          g:frame_round(right - cw - 4, ky - 3, cw + 8, ui.keycaps_height() + 6,
                        theme.accent, 7)
        end
      elseif is_edit then
        local hint = "Press the new keys"

        g:text(right - gfx.measure(hint), ty, hint, theme.text_dim)
      else
        local none = "no keys"

        g:text(right - gfx.measure(none), ty, none, theme.text_dim)
      end

      y = y + rh + ROW_GAP

      -- The keys another shortcut has, said under the row with the choice.
      if is_edit and editing.heard then
        local kind, sentence = verdict(r, editing.heard)

        if kind ~= "free" then
          local box_h = gfx.height() + 16

          g:fill_round(PAD + 4, y + 4, self.w - 2 * PAD - 8, box_h,
                       theme.mix(ui.warning_colours(), theme.window, 820), 7)

          local x = PAD + 14

          g:text(x, y + 12, sentence, theme.text)
          x = x + gfx.measure(sentence) + 6

          if kind == "taken" then
            local use_ = "Use it here"

            g:text(x, y + 12, use_, theme.text, nil, "label")
            self.links[#self.links + 1] = { x = x - 4, y = y + 4, w = gfx.measure(use_, "label") + 8,
                                            h = box_h, row = r, kind = "use" }
            log[#log + 1] = ("use at %d,%d"):format(screen_x + x + 20, screen_y + y + 4 + box_h // 2)
            x = x + gfx.measure(use_, "label") + 6
            g:text(x, y + 12, "or press other keys.", theme.text_dim)
          end

          y = y + box_h + 8
        end
      end
    end

    -- Yours ends with its own way in.
    if s.group.id == "yours" and #words == 0 then
      local add = "+ Add a shortcut"

      g:text(PAD + 6, y + 8, add, theme.accent, nil, "label")
      self.links[#self.links + 1] = { x = PAD, y = y, w = gfx.measure(add, "label") + 12,
                                      h = rh, kind = "add" }
      log[#log + 1] = ("add at %d,%d"):format(screen_x + PAD + 40, screen_y + y + 8 + gfx.height() // 2)
      y = y + rh
    end

    y = y + 14
  end

  if #list == 0 then
    g:text(PAD, 20, "Nothing does that.", theme.text_dim)
  end

  self.content_h = y + self.top

  -- Said when what is shown changes, for `tools/run_shortcuts.py`: the
  -- sections and every row's keys, and where each link is on the screen.
  local shown = {}

  for _, s in ipairs(list) do
    local ks = {}

    for _, r in ipairs(s.rows) do ks[#ks + 1] = r.keys ~= "" and r.keys or "-" end

    shown[#shown + 1] = s.group.name .. " (" .. table.concat(ks, " ") .. ")"
  end

  local line = ("shortcuts: showing %s: %s%s"):format(
                 #words > 0 and ("search \"" .. query .. "\"") or chosen, table.concat(shown, "; "),
                 editing and (" - changing " .. tostring(editing.row.action or editing.row.keys)
                              .. (editing.heard and (", heard " .. editing.heard) or "")) or "")

  if line ~= said then
    said = line
    print(line)
    if #log > 0 then print("shortcuts: links " .. table.concat(log, "; ")) end
  end
end

function main:wheel(n)
  local most = math.max(0, (self.content_h or 0) - self.h)

  self.top = math.max(0, math.min(most, self.top - n * 3 * row_h()))
  return true
end

--------------------------------------------------------------------------
-- The sheet: Add a shortcut.
--------------------------------------------------------------------------

local ACTIONS_WORDS = {}           -- the system's things a shortcut can do

local function open_sheet()
  if sheet then return end

  stop_editing()
  load_apps()

  local chord = nil
  local kind = "open"
  local target = nil

  for _, a in ipairs(apps) do
    if a.name == "Terminal" then target = a.path end
  end

  target = target or (apps[1] and apps[1].path)

  local s = ui.sheet{ title = "Add a shortcut", w = 420, h = 312,
                      on_close = function()
                        sheet = nil
                        listen(false)
                        print("shortcuts: the sheet put away")
                      end }

  local keys_box = ui.view{ x = 18, y = 72, w = 384, h = 44 }

  function keys_box:draw(g)
    g:fill_round(0, 0, self.w, self.h, theme.accent, 8)
    g:fill_round(2, 2, self.w - 4, self.h - 4, theme.sunken, 7)

    if chord then
      local cw = g:keycaps(12, (self.h - ui.keycaps_height()) // 2, chord)

      g:text(12 + cw + 12, (self.h - gfx.height()) // 2,
             "pressed - press others to change them", theme.text_dim, nil, nil)
    else
      g:text(12, (self.h - gfx.height()) // 2, "Press the keys, with Super", theme.text_dim)
    end
  end

  local kinds = { { "open", "Open an application" } }
  local actions = {}

  for _, r in ipairs(rows) do
    if r.action and not r.fixed and r.group ~= "yours" then
      actions[#actions + 1] = { r.action, tostring(r.what) }
      ACTIONS_WORDS[r.action] = r.what
    end
  end

  if #actions > 0 then kinds[#kinds + 1] = { "system", "Do what a system key does" } end

  local function app_choices()
    local out = {}

    for _, a in ipairs(apps) do out[#out + 1] = { a.path, a.name } end

    return out
  end

  local second = ui.dropdown{ x = 18, y = 186, w = 384, choices = app_choices(), value = target,
                              on_change = function(_, v) target = v end }

  local first = ui.dropdown{ x = 18, y = 146, w = 384, choices = kinds, value = kind,
                             on_change = function(_, v)
                               kind = v
                               second.choices = (v == "open") and app_choices() or actions
                               second.value = second.choices[1] and second.choices[1][1]
                               target = second.value
                             end }

  local said_label = ui.view{ x = 18, y = 226, w = 384, h = 22 }

  function said_label:draw(g)
    if not chord then return end

    local kind_, sentence = verdict({}, chord)
    local colour = kind_ == "free" and theme.good or ui.warning_colours()

    if kind_ == "taken" then sentence = sentence .. " Add takes them." end

    g:text(0, 2, sentence, colour, nil, nil)
  end

  local function add()
    if not chord or not target then return end

    local kind_ = verdict({}, chord)

    if kind_ == "fixed" then return end

    local r

    if kind == "open" then
      r = { group = "yours", action = "open", program = target, keys = "" }
      rows[#rows + 1] = r
    else
      for _, x in ipairs(rows) do
        if x.action == target and x.group ~= "yours" then r = x end
      end
    end

    if r then
      print(("shortcuts: added %s, %s"):format(chord, kind == "open" and target or target))
      assign(r, chord)
    end

    s:close()
  end

  local cancel = ui.button{ x = 420 - 18 - 76 - 10 - 90, y = 262, w = 90, text = "Cancel",
                            on_click = function() s:close() end }
  local go = ui.button{ x = 420 - 18 - 76, y = 262, w = 76, text = "Add", go = true,
                        on_click = add }

  local label_keys = ui.label{ x = 18, y = 50, text = "Keys", color = "text_dim" }
  local label_does = ui.label{ x = 18, y = 124, text = "What it does", color = "text_dim" }

  for _, c in ipairs({ label_keys, keys_box, label_does, first, second, said_label, cancel, go }) do
    s:put(c)
  end

  s.chord = function(c) chord = c end
  s.add = add
  sheet = s
  win:add(s)
  win:focus_on(go)
  listen(true)

  local ox, oy = s.panel.x, s.panel.y

  print(("shortcuts: the sheet open, add at %d,%d, cancel at %d,%d"):format(
        win.origin_x + ox + go.x + go.w // 2, win.origin_y + oy + go.y + 14,
        win.origin_x + ox + cancel.x + cancel.w // 2, win.origin_y + oy + cancel.y + 14))
end

function main:mouse(action, x, y)
  if action ~= "press" then return true end

  for _, l in ipairs(self.links) do
    if x >= l.x and x < l.x + l.w and y >= l.y and y < l.y + l.h then
      if l.kind == "add" then
        open_sheet()
      elseif l.kind == "change" then
        stop_editing()
        editing = { row = l.row }
        status = "Press the new keys - Escape stops"
        listen(true)
        print("shortcuts: changing " .. tostring(l.row.action or l.row.keys))
      elseif l.kind == "default" and editing then
        local r = editing.row

        stop_editing()

        if r.group == "yours" then
          for i, x2 in ipairs(rows) do
            if x2 == r then table.remove(rows, i) break end
          end

          save()
        else
          assign(r, r.default)
        end

        print("shortcuts: back to its default, " .. tostring(r.action or r.keys))
      elseif l.kind == "use" and editing and editing.heard then
        local r, chord = editing.row, editing.heard

        stop_editing()
        assign(r, chord)
        print(("shortcuts: %s took %s"):format(tostring(r.action), chord))
      end

      return true
    end
  end

  return true
end

-- The keyboard along the rows: the arrows choose, Enter changes.
function main:key(c)
  local list = drawn_rows()

  if c == ui.UP or c == ui.DOWN then
    self.keyed = true
    self.at = math.max(1, math.min(#list, self.at + (c == ui.UP and -1 or 1)))
    return true
  end

  if (c == 13 or c == 10) and list[self.at] and changeable(list[self.at]) then
    editing = { row = list[self.at] }
    listen(true)
    print("shortcuts: changing " .. tostring(list[self.at].action or list[self.at].keys))
    return true
  end

  return false
end

--------------------------------------------------------------------------
-- The foot: how many, and Print as a cheat sheet.
--------------------------------------------------------------------------

local foot = ui.view{ x = SIDE_W, y = H - FOOT_H, w = W - SIDE_W, h = FOOT_H,
                      follow = { "left", "right", "bottom" } }

local print_button = ui.button{ text = "Print as a cheat sheet",
                                on_click = function()
                                  if win.cheat_sheet then win.cheat_sheet() end
                                end }

foot:add(print_button)

function foot:draw(g)
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local total, made, changed = 0, 0, 0

  for _, r in ipairs(rows) do
    if r.group == "yours" then made = made + 1 else total = total + 1 end
    if r.default and r.keys ~= r.default then changed = changed + 1 end
  end

  local words = status ~= "" and status
                or (made + changed > 0) and ("%d shortcuts \u{00b7} %d made here, %d changed"):format(
                     total + made, made, changed)
                or ("%d shortcuts \u{00b7} every key that works everywhere"):format(total)

  g:text(PAD, (self.h - gfx.height()) // 2, words, theme.text_dim)

  print_button.x = self.w - PAD - print_button.w
  print_button.y = (self.h - print_button.h) // 2
end

--------------------------------------------------------------------------
-- Keys and events.
--------------------------------------------------------------------------

-- A chord the window manager heard for this window while it listened.
function win:on_event(ev)
  if ev.type ~= "chord" then return false end

  print("shortcuts: heard " .. tostring(ev.keys))

  if sheet and sheet.chord then
    sheet.chord(ev.keys)
    return true
  end

  if editing then
    editing.heard = ev.keys

    local kind = verdict(editing.row, ev.keys)

    if kind == "free" then
      local r = editing.row

      stop_editing()
      assign(r, ev.keys)
      print(("shortcuts: %s took %s"):format(tostring(r.action or "yours"), ev.keys))
    else
      print("shortcuts: " .. select(2, verdict(editing.row, ev.keys)))
    end

    return true
  end

  return false
end

function win:on_key(c)
  -- Control-N: Add a shortcut. The window's own key, so Control's.
  if c == 14 then
    open_sheet()
    return true
  end

  if c == 27 then
    if sheet then
      sheet:close()
      return true
    end

    if editing then
      stop_editing()
      print("shortcuts: stopped listening")
      return true
    end

    if MODAL then
      self:close()
      return true
    end
  end

  -- A character typed anywhere searches: the field takes it and the keys.
  if not sheet and not editing and c >= 32 and c < 127 then
    self:focus_on(search)
    search:key(c)
    return true
  end

  -- Enter on the sheet adds.
  if sheet and (c == 13 or c == 10) then
    sheet.add()
    return true
  end

  return false
end

load()
load_apps()
side.items = side_items()
side.selected = chosen

-- Kept current: the counts follow every change.
local side_draw = side.draw

function side:draw(g)
  self.items = side_items()
  side_draw(self, g)
end

win:add(column)
win:add(side)
win:add(main)
win:add(foot)
win:add(header)

-- The keys along the rows to start with, and the field's hint showing, as
-- the drawing has it; a character typed goes to the search (`on_key`).
win:focus_on(main)

do
  local parts = {}

  for _, g in ipairs(GROUPS) do parts[#parts + 1] = ("%s %d"):format(g.name, count(g.id)) end

  parts[#parts + 1] = ("Yours %d"):format(count("yours"))

  -- And any key the window manager spelled so it could not be drawn.
  local refused = {}

  for _, r in ipairs(rows) do
    if r.keys ~= "" and not keys.caps(r.keys) then refused[#refused + 1] = r.keys end
  end

  print(("shortcuts: groups %s; %d not keys%s"):format(table.concat(parts, ", "), #refused,
        #refused > 0 and (": " .. table.concat(refused, ", ")) or ""))
end

-- Print as a cheat sheet comes with K4 (`docs/shortcuts.md`).
win.cheat_sheet = nil

win:run()
