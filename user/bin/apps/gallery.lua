-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Playground
-- kosmos: section demos
-- The widget gallery. Every control the kit has, in one window.
--
--   wm gallery
--
--   click          focus and operate a control
--   Tab            move the focus
--   Enter / Space  activate
--   arrows         move in a list, or a level
--
-- Started by `wm`, which hands it /app/wm and nothing else it did not
-- already have. It draws by sending commands; the pixels stay in the
-- window manager, which is why this window would survive this program
-- hanging.
--
-- **As `docs/apps.html` draws it** (`roadmap.md` 5zp): a header, a card of
-- controls each named on its left, and the list under it at the page's
-- margins. It was labels and buttons placed by hand at x = 16 and 150, a
-- panel of words beside them, and a status line at the bottom - which is
-- what "no margin or spacing" looked like in the one window whose job is to
-- show the kit off.
--
-- What the last control did goes where every converted window says what it
-- is doing: beside the title.

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme
local L = ui.layout

local W = 460
local LIST_ROWS = 3                      -- of five, so the list scrolls
local ITEMS = { "threads", "address spaces", "endpoints", "capabilities",
                "and nothing else" }

local header                             -- where `say` writes

local function say(text)
  if header then header.sub = text end
end

--
-- The card of controls, one of each, each named on its left.
--
-- **A function, called twice.** A control measures its words when it is
-- made and the faces arrive with the window, so the real ones are made
-- after it opens - but the window has to be asked for at its height, and a
-- page's height is its rows. Every row here is a control of a fixed height
-- inside the drawings' padding, so a page built before the window is the
-- right height with the wrong widths, which is all the window needs.
--
local function page()
  local c = {}

  c.press = ui.button{ text = "Press me",
                       on_click = function() say("the button was pressed") end }
  c.verb = ui.button{ text = "And me", go = true,
                      on_click = function() say("the verb") end }
  c.switch = ui.switch{ on = true,
                        on_change = function(_, on)
                          say(on and "switched on" or "switched off")
                        end }
  c.tick = ui.checkbox{ text = "", h = 25,
                        on_change = function(_, on)
                          say(on and "ticked" or "not ticked")
                        end }

  local choices = {}

  for _, item in ipairs(ITEMS) do choices[#choices + 1] = { item, item } end

  c.choice = ui.dropdown{ choices = choices, value = ITEMS[1],
                          on_change = function(_, value)
                            say("chose " .. value)
                          end }
  c.level = ui.slider{ value = 60,
                       on_change = function(_, value)
                         say(("at %d of 100"):format(value))
                       end }
  c.words = ui.field{ w = 220, text = "editable text",
                      on_enter = function(_, t) say("entered: " .. t) end }

  c.cards = ui.cards{
    x = 0, y = L.head, w = W, h = 1,
    follow = { "left", "right", "top" },
    groups = {
      { name = "Controls", rows = {
          { label = "A button", control = c.press },
          { label = "The verb", control = c.verb },
          { label = "A switch", control = c.switch },
          { label = "A tick", control = c.tick },
          { label = "A choice", control = c.choice },
          { label = "A level", control = c.level },
          { label = "Some words", control = c.words } } },
    },
  }

  c.cards.h = c.cards.content_h
  return c
end

--
-- The list, under the card at the drawings' spacing - a group's name, then
-- the list 26 below it - and the page's note under that.
--
local content_h = page().cards.content_h
local list_name_y = L.head + content_h + L.between
local list_y = list_name_y + L.to_card
local list_h = 4 + LIST_ROWS * ui.metrics.row
local foot_y = list_y + list_h + 10
local H = foot_y + L.line + L.page_foot

local win, err = ui.window{ title = "gallery", w = W, h = H, x = 60, y = 90 }

if not win then
  print("gallery: " .. tostring(err))
  return
end

local c = page()

local more = ui.iconbutton{ icon = "more" }

header = ui.header{ x = 0, y = 0, w = W, title = "Widgets",
                    sub = "the kit's vocabulary", right = { more } }

local list = ui.list{ x = L.page_side, y = list_y, w = W - 2 * L.page_side,
                      h = list_h, items = ITEMS,
                      on_select = function(_, item) say("chose " .. item) end }

local foot = ui.label{ x = L.page_side + 3, y = foot_y,
                       w = W - 2 * L.page_side - 3,
                       text = "Click, or Tab and Enter.", color = "text_dim",
                       role = "ui" }

--
-- Everything back as it opened: the menu the dots open, and the kit's menu
-- being one more thing this window shows.
--
more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "Put everything back", on_choose = function()
        c.switch.on, c.tick.checked = true, false
        c.choice.value, c.level.value = ITEMS[1], 60
        c.words.text = "editable text"
        list.selected = 1
        say("the kit's vocabulary")
      end },
  })
end

--
-- **Added in the order Tab should visit them**: the card's controls from the
-- top, the list, and the header's dots last - so the focus starts on the
-- first button, as it did when the gallery was a column of controls, and
-- the header is where the keyboard arrives after the page rather than
-- before it.
--
win:add(c.cards)
win:add(ui.label{ x = L.page_side + 3,
                  y = list_name_y + (L.group - gfx.height("heading")) // 2,
                  w = 200, text = "A list", role = "heading" })
win:add(list)
win:add(foot)
win:add(header)

--
-- Where things are, in points inside the window - for the display harness,
-- which clicks the two buttons, the list's rows and the field, and measures
-- the list's selection bar, and would otherwise hold a copy of this layout that
-- drifts from it. It did: the clicks phase aimed at a status line at 296
-- and rows 16 apart for two versions after both had gone.
--
local function centre(v)
  return c.cards.x + v.x + v.w // 2, c.cards.y + v.y + v.h // 2
end

local px, py = centre(c.press)
local vx, vy = centre(c.verb)
local fx, fy = centre(c.words)

print(("gallery: %dx%d, list at %d,%d %dx%d, buttons at %d,%d %d,%d, "
       .. "field at %d,%d")
      :format(W, H, list.x, list.y, list.w, list.h, px, py, vx, vy, fx, fy))

win:run()
