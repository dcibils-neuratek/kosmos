-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Passwords
-- kosmos: section system
-- kosmos: needs keyring
--
-- Passwords: what this machine remembers, so nothing is typed twice
-- (`docs/keyring.md`, step K6; the drawing is `docs/keyring.html`).
--
-- **As the mockup draws it**: the kinds down the side, the list with its
-- search and sort in the middle, the entry on the right - its account, its
-- address, its password as dots with **Show** (Diego's decision 5), its
-- kind and dates, where it is used and how often, the shares it opens and
-- whether they connect when Kosmos starts, notes in Diego's own words,
-- **Sign in again...** and **Delete...**, which asks first and says what
-- follows.
--
-- **Through the `manage` door alone**: a launcher hands it only to a file
-- the image serves that says `kosmos: needs keyring` above, so this file
-- copied to `/Home` lists nothing. It lists without secrets, shows one when
-- asked (`fs.keyring_reveal`), edits a title and notes, and forgets; it
-- cannot put a password in - that is signing in again, where the server
-- checks it.
--
-- **Nothing is asked in a paint.** The list is read when the window opens,
-- after each change, and every five seconds on the clock, so a share signed
-- into meanwhile appears; a shown password is dropped from memory when the
-- selection moves or Hide is pressed, and never printed.

local ui = use("/Kosmos/Libraries/ui.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local theme = ui.theme
local L = ui.layout

local W, H = 940, 600
local SIDE = 200
local LISTW = 320
local ROWH = 56
local FOOT = 32

-- Said first, before a window: whether this copy was handed the door - the
-- one in the image is, the same file in `/Home` is not.
local handed = fs.has_keyring()
print(handed and "passwords: handed the keyring" or "passwords: not handed the keyring")

local win, err = ui.window{ title = "Passwords", w = W, h = H, x = 120, y = 80,
                            header = true }

if not win then
  print("passwords: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- What is kept, as the keyring says it.
--------------------------------------------------------------------------

local KINDS = { smb = "Shared folders", wifi = "Wi-Fi", web = "Websites", mail = "Mail" }
local KIND_ICON = { smb = "folder", wifi = "wifi", web = "globe", mail = "comment" }
local USERS = { smbfs = "smbfs, the SMB client" }
local SORTS = { "Last changed", "Title", "Kind", "Last used" }

local entries, shown = {}, {}
local file = "new"
local kind, query, order = "all", "", SORTS[1]
local chosen = nil                  -- the id of the entry on the right
local top = 1                       -- the first row the list draws
local revealed = nil                -- { id = , secret = } while shown
local confirming = nil              -- the entry Delete... asked about
local said = ""

local function title_of(e)
  return (e.title ~= "" and e.title) or e.service
end

local function matches(e)
  if kind ~= "all" and e.kind ~= kind then return false end
  if query == "" then return true end

  local q = query:lower()

  for _, f in ipairs({ e.title, e.account, e.service, e.notes }) do
    if (f or ""):lower():find(q, 1, true) then return true end
  end

  return false
end

local SORT_BY = {
  ["Last changed"] = function(a, b) return a.modified > b.modified end,
  ["Title"] = function(a, b) return title_of(a):lower() < title_of(b):lower() end,
  ["Kind"] = function(a, b)
    if a.kind ~= b.kind then return a.kind < b.kind end
    return title_of(a):lower() < title_of(b):lower()
  end,
  ["Last used"] = function(a, b) return a.used > b.used end,
}

local function chosen_entry()
  for _, e in ipairs(entries) do
    if e.id == chosen then return e end
  end
  return nil
end

local function filter()
  shown = {}

  for _, e in ipairs(entries) do
    if matches(e) then shown[#shown + 1] = e end
  end

  table.sort(shown, function(a, b)
    local by = SORT_BY[order](a, b)
    if by == SORT_BY[order](b, a) then return a.id > b.id end
    return by
  end)

  local still = false

  for _, e in ipairs(shown) do
    if e.id == chosen then still = true end
  end

  if not still then chosen = shown[1] and shown[1].id or nil end
end

local refresh_detail                -- forward: the right side follows the choice

local function load()
  local list, why = fs.keyring_list()
  local state = fs.keyring_state()

  if not list then
    said = tostring(why)
    entries = {}
  else
    entries = list
  end

  file = state and state.file or "unknown"
  filter()
  refresh_detail()
end

local function forget_shown()
  if revealed then revealed.secret = nil end
  revealed = nil
end

--------------------------------------------------------------------------
-- The window: the kinds, the list, the entry.
--------------------------------------------------------------------------

local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = H, follow = { "left", "top", "bottom" } }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
  g:text(L.head_in, (L.head - 1 - gfx.height("title")) // 2, "Passwords",
         theme.text, nil, "title")
  g:text(L.head_in, self.h - FOOT - 2 * gfx.height(), "This machine",
         theme.text_dim, nil, "ui")
  g:text(L.head_in, self.h - FOOT - gfx.height(), "Opened with the machine",
         theme.text_dim, nil, "ui")
end

local side                          -- made by `sides`, as the counts change

local function sides()
  local counts = { all = #entries }

  for _, e in ipairs(entries) do counts[e.kind] = (counts[e.kind] or 0) + 1 end

  local items = { { id = "all", name = ("All  %d"):format(#entries), icon = "key" } }

  -- A kind with nothing in it is not listed; Shared folders always is,
  -- being the one kind there is.
  for _, k in ipairs({ "smb", "wifi", "web", "mail" }) do
    if k == "smb" or (counts[k] or 0) > 0 then
      items[#items + 1] = { id = k, name = ("%s  %d"):format(KINDS[k], counts[k] or 0),
                            icon = KIND_ICON[k] }
    end
  end

  return items
end

local search = ui.field{ w = 200, text = "", hint = "Search" }
local SORT_CHOICES = {}

for _, name in ipairs(SORTS) do SORT_CHOICES[#SORT_CHOICES + 1] = { name, name } end

local sort = ui.dropdown{ choices = SORT_CHOICES, value = order }
local header = ui.header{ x = SIDE, y = 0, w = W - SIDE, title = "All",
                          title_bar = true, right = { search, sort } }

local list = ui.view{ x = SIDE, y = L.head, w = LISTW, h = H - L.head - FOOT,
                      follow = { "left", "top", "bottom" } }
list.focusable = true

local detail = ui.view{ x = SIDE + LISTW, y = L.head, w = W - SIDE - LISTW,
                        h = H - L.head - FOOT, follow = { "left", "right", "top", "bottom" } }

local foot = ui.view{ x = SIDE, y = H - FOOT, w = W - SIDE, h = FOOT,
                      follow = { "left", "right", "bottom" } }

-- The entry's controls: placed by `place_detail` beside what they belong to.
local show = ui.button{ text = "Show" }
local title_field = ui.field{ w = 300, text = "", hint = "a title" }
local notes = ui.field{ w = 300, text = "", hint = "your own words" }
local at_start = ui.switch{ on = false }
local again = ui.button{ text = "Sign in again\u{2026}" }
local delete = ui.button{ text = "Delete\u{2026}" }

-- The confirmation, over everything while it is open.
local veil = ui.view{ x = 0, y = 0, w = W, h = H, hidden = true,
                      follow = { "left", "right", "top", "bottom" } }
local keep = ui.button{ text = "Cancel", hidden = true }
local drop = ui.button{ text = "Delete", go = true, hidden = true }

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

local function now_epoch()
  return (clock.now() or {}).epoch or 0
end

local function date(unix)
  if not unix or unix == 0 then return "never" end
  return clock.long_string(clock.at(unix))
end

function list:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)

  local rows = self.h // ROWH

  for i = top, math.min(#shown, top + rows - 1) do
    local e = shown[i]
    local y = (i - top) * ROWH
    local on = e.id == chosen
    local ink = on and theme.text_on or theme.text
    local dim = on and theme.text_on or theme.text_dim

    if on then g:fill(6, y + 3, self.w - 13, ROWH - 6, theme.accent) end

    g:line_icon(16, y + (ROWH - 15) // 2, KIND_ICON[e.kind] or "key", dim)
    g:text(44, y + 9, ui.fitted(title_of(e), self.w - 60, "text"), ink, nil, "text")
    g:text(44, y + 9 + gfx.height("text"), ui.fitted(e.account, self.w - 60, "ui"), dim, nil, "ui")
  end

  if #shown == 0 then
    g:text(16, 16, query ~= "" and "Nothing kept matches." or "Nothing is kept yet.",
           theme.text_dim, nil, "ui")
  end
end

-- The rows of the entry: a name and what it is, a line apart.
local ROWS_AT = 70                  -- below the title
local LINE = 32

local function detail_rows(e)
  local shares = {}

  for name in (e.shares or ""):gmatch("[^%z]+") do shares[#shares + 1] = name end

  local password = revealed and revealed.id == e.id and revealed.secret
                   or string.rep("\u{2022}", 12)

  return {
    { "Title", "" },
    { "Account", e.account },
    { "Address", e.service },
    { "Password", password },
    { "Kind", KINDS[e.kind] or e.kind },
    { "Created", date(e.created) },
    { "Modified", date(e.modified) },
    { "Used", e.uses > 0 and ("%d %s, last %s"):format(e.uses,
        e.uses == 1 and "time" or "times", clock.relative(e.used, now_epoch()))
        or "not yet" },
    { "By", e.uses > 0 and (USERS[e.used_by_name] or e.used_by_name) or "" },
    { "Shares", #shares > 0 and table.concat(shares, ", ") or "none yet" },
    { "Connect at start", "" },
    { "Notes", "" },
  }
end

function detail:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  local e = chosen_entry()

  if not e then
    g:text(24, 24, "Choose an entry to see what is kept.", theme.text_dim, nil, "ui")
    return
  end

  g:line_icon(24, 22, KIND_ICON[e.kind] or "key", theme.text_dim)
  g:text(52, 18, ui.fitted(title_of(e), self.w - 76, "title"), theme.text, nil, "title")

  for i, row in ipairs(detail_rows(e)) do
    local y = ROWS_AT + (i - 1) * LINE

    g:text(24, y, row[1], theme.text_dim, nil, "ui")
    -- The password's row leaves room for Show at its end.
    local room = self.w - 194 - (row[1] == "Password" and show.w + 12 or 0)

    g:text(170, y, ui.fitted(row[2], room, "text"), theme.text, nil, "text")
  end
end

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.line_soft, 330))
  g:fill(0, 0, self.w, 1, theme.line_soft)

  local words = ("%d %s \u{00b7} sealed \u{00b7} key kept by this machine"):format(
    #entries, #entries == 1 and "entry" or "entries")

  if file == "set aside" then
    words = words .. " \u{00b7} an older keyring did not open and is kept aside"
  elseif file == "no disk" then
    words = "No disk: this machine has nowhere to keep a password"
  end

  if said ~= "" then words = said end

  g:text(16, (self.h - gfx.height("ui")) // 2, ui.fitted(words, self.w - 32, "ui"),
         theme.text_dim, nil, "ui")
end

function veil:draw(g)
  local e = confirming

  if not e then return end

  g:fill(0, 0, self.w, self.h, theme.mix(theme.window, theme.text, 120))

  local cw, ch = 520, 220
  local cx, cy = (self.w - cw) // 2, (self.h - ch) // 2
  local shares = (e.shares or ""):gsub("%z+$", ""):gsub("%z", " and ")

  g:fill(cx, cy, cw, ch, theme.window)
  g:text(cx + 24, cy + 20, ("Delete %s?"):format(ui.fitted(title_of(e), cw - 120, "title")),
         theme.text, nil, "title")

  local lines = ui.wrapped(("Forget the password for %s on %s? %sThe next sign-in "
    .. "will ask for the password, and nothing will connect when Kosmos starts. "
    .. "This cannot be undone. The server's own account is not changed."):format(
      e.account, e.service,
      shares ~= "" and (shares .. " stay open until they are disconnected. ") or ""),
    cw - 48, "ui")

  for i, line in ipairs(lines) do
    g:text(cx + 24, cy + 60 + (i - 1) * gfx.height("ui"), line, theme.text_dim, nil, "ui")
  end

end

-- The confirmation's card is 520 by 220 in the middle; its buttons at its
-- foot, Delete at the right.
local function place_veil()
  local cx, cy = (veil.w - 520) // 2, (veil.h - 220) // 2

  drop.x, drop.y = cx + 520 - 24 - drop.w, cy + 220 - 20 - drop.h
  keep.x, keep.y = drop.x - 8 - keep.w, drop.y
end

--------------------------------------------------------------------------
-- Placing what moves with the choice and the size.
--------------------------------------------------------------------------

local function place_detail()
  local e = chosen_entry()
  local x0 = SIDE + LISTW
  local here = e ~= nil

  for _, v in ipairs({ show, title_field, notes, at_start, again, delete }) do
    v.hidden = not here
  end

  if not here then return end

  -- Each beside its row's name: Title the first, Password the fourth,
  -- Connect at start the eleventh, Notes the twelfth (`detail_rows`).
  title_field.x, title_field.y, title_field.w = x0 + 170, L.head + ROWS_AT - 6, detail.w - 194
  show.x, show.y = x0 + detail.w - 24 - show.w, L.head + ROWS_AT + 3 * LINE - 6
  at_start.x, at_start.y = x0 + 170, L.head + ROWS_AT + 10 * LINE - 4
  notes.x, notes.y, notes.w = x0 + 170, L.head + ROWS_AT + 11 * LINE - 6, detail.w - 194
  again.x, again.y = x0 + 24, L.head + detail.h - 20 - again.h
  delete.x, delete.y = x0 + detail.w - 24 - delete.w, again.y
  show.text = (revealed and revealed.id == e.id) and "Hide" or "Show"
end

function refresh_detail()
  local e = chosen_entry()

  if e then
    title_field.text = e.title
    title_field.caret = #title_field.text + 1
    notes.text = e.notes
    notes.caret = #notes.text + 1
    at_start.on = e.at_start
  end

  header.title = kind == "all" and "All" or (KINDS[kind] or kind)
  header.sub = ("%d kept"):format(#shown)

  local items = sides()

  if side then
    side.items = items
  end

  place_detail()
  win.dirty = true
end

--------------------------------------------------------------------------
-- What is pressed.
--------------------------------------------------------------------------

local function choose(id)
  if id ~= chosen then forget_shown() end
  chosen = id
  refresh_detail()

  local e = chosen_entry()
  if e then print(("passwords: showing %d, %s"):format(e.id, title_of(e))) end
end

local function save_edit()
  local e = chosen_entry()

  if not e then return end

  local ok, why = fs.keyring_edit(e.id, title_field.text, notes.text, at_start.on)

  said = ok and "" or ("could not keep that: " .. tostring(why))
  if ok then print(("passwords: kept the title and notes of %d"):format(e.id)) end
  load()
end

local function ask_delete()
  local e = chosen_entry()

  if not e then return end

  confirming = e
  veil.hidden, keep.hidden, drop.hidden = false, false, false
  place_veil()
  print(("passwords: asked to delete %d, cancel at %d,%d delete at %d,%d"):format(
    e.id, keep.x + keep.w // 2, keep.y + keep.h // 2, drop.x + drop.w // 2,
    drop.y + drop.h // 2))
  win.dirty = true
end

local function close_confirm()
  confirming = nil
  veil.hidden, keep.hidden, drop.hidden = true, true, true
  win.dirty = true
end

keep.on_click = close_confirm

drop.on_click = function()
  local e = confirming

  close_confirm()
  if not e then return end

  local ok, why = fs.keyring_forget(e.id)

  said = ok and "" or ("could not delete it: " .. tostring(why))
  if ok then print(("passwords: deleted %d"):format(e.id)) end
  forget_shown()
  load()
end

delete.on_click = ask_delete

show.on_click = function()
  local e = chosen_entry()

  if not e then return end

  if revealed and revealed.id == e.id then
    forget_shown()
  else
    local secret, why = fs.keyring_reveal(e.id)

    if secret then
      revealed = { id = e.id, secret = secret }
      print(("passwords: shown %d"):format(e.id))   -- never the password
    else
      said = "could not show it: " .. tostring(why)
    end
  end

  place_detail()
  win.dirty = true
end

again.on_click = function()
  local e = chosen_entry()

  if not e then return end

  -- Connect to Server on its address: the way a changed password is changed.
  fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/connect.lua",
                           args = e.service })
end

title_field.on_enter = save_edit
notes.on_enter = save_edit
at_start.on_change = function() save_edit() end

function search:on_change()
  query = self.text
  filter()
  top = 1
  refresh_detail()
  print(("passwords: %d shown for %q"):format(#shown, query))
end

-- The list as it stands, in order, for the display suite.
local function said_order(why)
  local names = {}

  for _, e in ipairs(shown) do names[#names + 1] = title_of(e) end
  print(("passwords: %s: %s"):format(why, table.concat(names, " | ")))
end

sort.on_change = function(_, value)
  order = value
  filter()
  refresh_detail()
  said_order("sorted by " .. value:lower())
end

function list:mouse(action, x, y)
  if action ~= "press" then return false end

  local i = top + y // ROWH

  if shown[i] then
    choose(shown[i].id)
    win:focus_on(list)
  end

  return true
end

function list:wheel(n)
  top = math.max(1, math.min(top + n, math.max(1, #shown - self.h // ROWH + 1)))
  win.dirty = true
  return true
end

function list:key(c)
  local k = ui.keyparts(c)
  local at = 1

  for i, e in ipairs(shown) do
    if e.id == chosen then at = i end
  end

  if k == ui.UP and at > 1 then
    choose(shown[at - 1].id)
  elseif k == ui.DOWN and at < #shown then
    choose(shown[at + 1].id)
  elseif k == ui.DELETE or c == 127 or c == 8 then
    ask_delete()
  else
    return false
  end

  -- The chosen row kept in view.
  local rows = self.h // ROWH
  local now_at = 1

  for i, e in ipairs(shown) do
    if e.id == chosen then now_at = i end
  end

  if now_at < top then top = now_at end
  if now_at >= top + rows then top = now_at - rows + 1 end

  return true
end

side = ui.sidebar{ x = 0, y = L.head + 2, w = SIDE - 1, h = H - L.head - FOOT - 60,
                   items = sides(), selected = kind,
                   on_select = function(_, id)
                     kind = id
                     filter()
                     top = 1
                     forget_shown()
                     refresh_detail()
                   end }

function win:on_resize(w, h)
  W, H = w, h
  place_detail()
end

-- Every five seconds, what the keyring holds now: a share remembered
-- meanwhile appears, and a use is counted. Never in a paint.
local looked = sys.ticks()
local placed = false
local said_places              -- below: once the window is drawn

function win:on_frame()
  if not placed then
    placed = true
    said_places()
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

  self.poll_wait_ticks = 250
  if confirming or sys.ticks() - looked < 5 * hz then return false end

  looked = sys.ticks()

  local before = #entries

  load()
  return #entries ~= before
end

for _, v in ipairs({ side_ground, side, list, detail, foot, header, show, title_field,
                     notes, at_start, again, delete, veil, keep, drop }) do
  win:add(v)
end

if not handed then
  said = "This copy of Passwords was not handed the keyring: it is opened from Kosmos itself."
else
  load()
  print(("passwords: %d %s, the file %s"):format(#entries,
        #entries == 1 and "entry" or "entries", file))
  said_order("sorted by last changed")
end

refresh_detail()
win:focus_on(list)

-- Where each control is, in the window, for the display suite: it presses
-- them as a person would. **Once the window has been drawn**, since the
-- header places the search and the sort as it draws.
function said_places()
print(("passwords: places list %d,%d rows %d search %d,%d sort %d,%d %d show %d,%d "
       .. "notes %d,%d delete %d,%d"):format(
  -- The search and the sort are the header's, placed inside it.
  list.x, list.y, ROWH, header.x + search.x + search.w // 2,
  header.y + search.y + search.h // 2,
  header.x + sort.x + sort.w // 2, header.y + sort.y + sort.h // 2, sort.h,
  show.x + show.w // 2, show.y + show.h // 2, notes.x + 20, notes.y + notes.h // 2,
  delete.x + delete.w // 2, delete.y + delete.h // 2))
end

win:run()
