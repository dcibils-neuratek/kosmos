-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Devices
-- kosmos: section preferences
-- One place to configure Kosmos, divided by part.
--
--   wm preferences
--   wm preferences:sound        opened on one category
--
-- Diego asked for it on 23 September 2026 with GNOME's Settings beside it:
-- "one place to configure all kosmos". `docs/preferences.html` is the page
-- it was drawn from and `roadmap.md` 5zh the agreement.
--
-- **It knows nothing about any particular setting.** Every row on every page
-- comes out of `user/lib/settings.lua`, which says what there is, which file
-- it lives in and what kind of control it wants. This file is the drawing;
-- that one is the design. Adding a setting to the system means adding a line
-- there, and it appears here.
--

local ui = use("/lib/ui.lua")
local settings = use("/lib/settings.lua")
local hardware = use("/lib/hardware.lua")
local theme = ui.theme

--
-- **The spacing is `docs/preferences.html`'s**, measured off the drawing
-- rather than chosen again here. Diego, 23 September 2026: "i want it to
-- look exactly like the mockup, spacing, button style, borders, titles,
-- rounded buttons and selectors".
--
-- The numbers that matter and why each is what it is:
--
--   SIDE     the category list, wide enough for "Date & Time" and no wider
--   PAD      from the window's edge to a card, and from a card to the next
--   ROW_H    a row with a note under its label: two lines and air
--   ROW_1    a row with only a label
--   CARD_R   the card's corner, which matches a control's (`ui.lua`)
--   HEAD_Y   from a card to the heading below it, and from that to the next
--
-- **A page is a column of at most `BODY_W`, centred.** The mockup's is 470
-- and the reason is not taste: a row is a label on the left and a control
-- on the right, and past about sixty characters the eye loses which control
-- belongs to which label. A window that is wider gets margins rather than
-- longer rows.
--
local SIDE   = 176
local PAD    = 18
local GAP    = 22           -- between a card and the next heading
local ROW_H  = 48           -- a row with a label and a note in it
local ROW_1  = 40           -- a row with only a label
local CARD_R = 10
local BODY_W = 470
local HEAD_Y = 8            -- from a heading to its card
local W, H   = 720, 520

local win, err = ui.window{ title = "Preferences", w = W, h = H,
                            x = 150, y = 100 }

if not win then
  print("preferences: " .. tostring(err))
  return
end

--
-- Which category the argument asked for, if any. `wm preferences:sound`.
--
-- `args` is the string after the colon, not a list: `wm preferences:sound`
-- arrives as "sound". Indexing it gave nil and every launch opened on the
-- first category, which looked exactly like a window with no argument.
local function wanted()
  local a = (tostring(args or "")):match("^%s*(%S*)")

  for _, c in ipairs(settings.CATEGORIES) do
    if c.id == a then return c.id end
  end

  return settings.CATEGORIES[1].id
end

local showing = wanted()

--
-- The page: cards of rows, drawn by the view and filled with controls.
--
-- **The card is drawn here rather than being a widget**, because it is a
-- rectangle behind things that are already placed and nothing else needs one
-- yet. The moment a second application wants grouped rows it becomes
-- `ui.group` and this loses twenty lines; until then a widget in the kit
-- would be a guess at what the second caller wants.
--
local page = ui.view{ x = SIDE, y = 0, w = W - SIDE, h = H }

local cards = {}            -- { y, h } for each card, in page coordinates

--
-- Where the column of cards sits: `BODY_W` wide, centred, and never wider
-- than the pane can hold.
--
local function column(w)
  local width = math.min(BODY_W, w - 2 * PAD)

  return (w - width) // 2, width
end

function page:draw(g)
  local x, width = column(self.w)

  g:fill(0, 0, self.w, self.h, theme.window)
  g:fill(0, 0, 1, self.h, theme.line_soft)

  for _, c in ipairs(cards) do
    --
    -- **Rounded when the look is flat**, which is the same rule its
    -- controls follow (`ui.lua`, `gc:raised`): a look that says nothing
    -- about light says everything with a line, and a square card under
    -- rounded buttons is the one combination that looks like a mistake.
    --
    if theme.flat then
      g:fill_round(x, c.y, width, c.h, theme.raised, CARD_R)
      g:frame_round(x, c.y, width, c.h, theme.line_soft, CARD_R)
    else
      g:fill(x, c.y, width, c.h, theme.raised)
      g:frame(x, c.y, width, c.h, theme.line_soft)
    end

    -- The lines between rows, not around them: one card, several rows. They
    -- stop short of the corners so a rule does not run into the arc.
    for _, at in ipairs(c.rules) do
      g:fill(x + 1, at, width - 2, 1, theme.line_soft)
    end
  end
end

win:add(page)

--
-- The sidebar, as a list of names with the gaps `settings.CATEGORIES` asks
-- for. A gap is an empty row rather than a second list: a list knows how to
-- scroll and how to follow the focus, and two of them would have to agree.
--
local names, index_of = {}, {}

for _, c in ipairs(settings.CATEGORIES) do
  names[#names + 1] = c.name
  index_of[c.name] = c.id

  if c.gap_after then names[#names + 1] = "" end
end

local rebuild                      -- forward, so the list can call it

local side = ui.list{
  x = 0, y = PAD, w = SIDE, h = H - 2 * PAD,
  items = names,
  on_select = function(_, item)
    local id = index_of[item]

    if id and id ~= showing then
      showing = id
      rebuild()
    end
  end,
}

win:add(side)

--
-- One row's control, whichever kind it is.
--
-- Everything that is not stored is shown rather than hidden, which is the
-- decision `settings.lua` records: a machine you can only configure by
-- rebuilding it is what this exists to end, so `smp=N` and the rest are here
-- with what they are set to and a line saying a restart is needed.
--
--
-- **A setting the window manager draws with, told at once.**
--
-- Rounded corners and drop shadows are its business rather than any
-- window's: it composes the desktop. A setting written to a file and left
-- there would take effect at the next restart, which is a setting nobody
-- believes in - `instant-feedback` applied to something that is not even a
-- control.
--
-- The same `theme` request the Appearance panel sends for the look, with
-- one field in it. A manager that does not know the field ignores it, which
-- is what makes adding another of these one line here.
--
local function live(it, value)
  if not it.live then return end

  local ok, why = fs.send("/app/wm", { type = "theme", [it.live] = value })

  if not ok then
    print("preferences: " .. tostring(it.live) .. ": " .. tostring(why))
  end
end

local function control_for(it, x, y, changed)
  if it.kind == "switch" then
    return ui.switch{ x = x, y = y, on = settings.get(it) == true,
                      on_change = function(_, on)
                        settings.set(it, on)
                        live(it, on)
                        if changed then changed() end
                      end }
  end

  if it.kind == "choice" and it.choices then
    return ui.dropdown{ x = x, y = y, choices = it.choices,
                        value = settings.get(it),
                        on_change = function(_, v)
                          settings.set(it, v)
                          if changed then changed() end
                        end }
  end

  return nil
end

--
-- What a row shows on the right when it has no control: a value, a hint, or
-- the word a `boot` setting needs.
--
--
-- What the machine is, read once: the About rows and nothing else.
--
-- `sys.build()` for the version, `hardware.name(sys.info())` for what the
-- machine calls itself and `/dev/memory` for its size - the same three doors
-- `neofetch` reads, so the two programs cannot disagree about what this
-- machine is. A second way of asking would be a second answer eventually.
--
local function facts()
  local b = sys.build() or {}
  local mem = fs.read("/dev/memory") or {}

  return {
    version = tostring(b.version or "?"),
    machine = tostring(hardware.name(sys.info() or {}) or "this machine"),
    memory = ("%d MB"):format(mem.total_mb or 0),
  }
end

local fact = facts()

local function value_text(it)
  -- The option's name is in the note; this is the part that is not obvious
  -- from reading the row, and the part a long note must not squeeze out.
  if it.kind == "boot" then return "Needs a restart" end
  if it.kind == "action" then return "Show" end
  if it.kind == "fact" then return fact[it.fact] or "-" end
  if it.kind == "level" then return "" end
  if it.kind == "text" then return "" end

  return tostring(settings.get(it) or "")
end

--
-- Build the page for `showing`.
--
rebuild = function()
  for i = #page.children, 1, -1 do page.children[i] = nil end
  cards = {}

  local cx, width = column(page.w)
  local y = PAD

  for _, group in ipairs(settings.groups(showing)) do
    -- The heading sits just left of the card's own inset, so a page reads
    -- as a column of headings with their cards under them rather than as
    -- two columns.
    page:add(ui.label{ x = cx + 2, y = y, w = width,
                       text = group.name, role = "heading" })

    y = y + gfx.height("heading") + HEAD_Y

    local card = { y = y, h = 0, rules = {} }
    local first = true

    for _, it in ipairs(group.items) do
      local h = it.note and ROW_H or ROW_1

      if not first then card.rules[#card.rules + 1] = y end
      first = false

      --
      -- **The control first, then the words get what is left.** A fixed
      -- reserve on the right was the obvious way and it clipped the longest
      -- note on the first page: a label is clipped to its own width, so a
      -- sentence that does not fit simply stops, with no sign that it did.
      -- Asking the control how wide it is costs nothing and cannot be wrong.
      --
      local right = cx + width - 14
      local c = control_for(it, 0, 0, nil)
      local taken = 0

      if c then
        c.x = right - c.w
        c.y = y + (h - c.h) // 2
        page:add(c)
        taken = c.w
      else
        local t = value_text(it)

        if t ~= "" then
          taken = gfx.measure(t)
          page:add(ui.label{ x = right - taken, y = y + (h - gfx.height()) // 2,
                             w = taken + 2, text = t,
                             color = theme.text_dim })
        end
      end

      local words = right - taken - 14 - (cx + 14)

      --
      -- **Two lines centred in the row as a pair**, not a label at a fixed
      -- offset with a note under it. A row with a note is two lines of
      -- text and a row without is one, and a row that put the first line
      -- in the same place either way leaves the single-line one sitting
      -- high in its own box.
      --
      local lines = it.note and 2 or 1
      local block = lines * gfx.height()
      local ty = y + (h - block) // 2

      if it.label ~= "" then
        page:add(ui.label{ x = cx + 14, y = ty, w = words,
                           text = it.label })
      end

      if it.note then
        page:add(ui.label{ x = cx + 14, y = ty + gfx.height(), w = words,
                           text = it.note, color = theme.text_dim })
      end

      y = y + h
    end

    card.h = y - card.y
    cards[#cards + 1] = card
    y = y + GAP
  end

  win:paint()
end

--
-- The list starts on the category the argument asked for.
--
for i, n in ipairs(names) do
  if index_of[n] == showing then side.selected = i end
end

rebuild()

win:run()
