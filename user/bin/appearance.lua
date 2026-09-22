-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Appearance
-- kosmos: section preferences
-- The look of the desktop: which of the four looks, and the picture behind
-- everything. Nothing else.
--
--   wm appearance
--   wm appearance:--theme plexnight     a look, as a click on its card
--
-- Haiku keeps this under Preferences and calls it Appearance, which is
-- where the name comes from.
--
-- **Three choices, drawn before they were built** (`docs/looks.html`,
-- `roadmap.md` 5x and 5y). This panel had grown to a theme list, a colour
-- for the desktop and one for the Deskbar, five roles each with a face and
-- a size, and a title's shape - and on 22 September 2026 Diego, having used
-- it, said what that costs: "Too many config options make the system
-- vulnerable to changes and complicated", "We should stick to very few
-- options in colors and fonts", and "Let's just make 3 or 4 good design
-- options in colors and fonts and stick to those". So a look is a whole -
-- its colours, its faces, its Deskbar, designed together - and a person
-- picks a look, not its parts; and the layout is fixed, so no choice here
-- can move a widget in any window. What is left beside the look is the
-- wallpaper.
--
-- **The Deskbar's height was a third choice, 36, 44 or 52, for an
-- afternoon** (`roadmap.md` 5v). Diego, 22 September, on the ThinkPad:
-- "Taskbar size should not be changeable let's make it fixed at 32" - so
-- it is part of the fixed layout (`theme.metrics.deskbar`) and not here.
--
-- Everything is one message to the window manager, which holds the look
-- because it is the one process already talking to every window; each
-- application's kit follows without the application knowing. The choice is
-- written to `/home/.appearance` - the look and the wallpaper - and read
-- back at startup.

local ui = use("/lib/ui.lua")
local theme = ui.theme
local LOOKS = use("/lib/themes.lua")
local M = ui.metrics

local SETTINGS = "/home/.appearance"

--
-- **The layout, fixed** - every position a sum of the fixed layout's own
-- numbers (`theme.metrics`), so it is the same in every look and at every
-- face, as `docs/looks.html` draws it: the four looks as cards, each a
-- desktop in miniature above its name; the wallpapers, six rows; and a
-- line to say what happened.
--
local W       = 560
local PAD     = M.gap
local GAP     = M.gap
local CARD_W  = (W - 2 * PAD - 3 * GAP) // 4
local MINI_H  = 58
local CARD_H  = 6 + MINI_H + 4 + M.row + 2
local WALL_ROWS = 6

local LOOK_Y      = PAD
local CARDS_Y     = LOOK_Y + M.row
local WALL_Y      = CARDS_Y + CARD_H + GAP
local WALL_LIST_Y = WALL_Y + M.row
local WALL_H      = WALL_ROWS * M.row + 4
local STATUS_Y    = WALL_LIST_Y + WALL_H + GAP
local H           = STATUS_Y + M.row + PAD

local win, err = ui.window{ title = "Appearance", w = W, h = H,
                            x = 160, y = 110 }

if not win then
  print("appearance: " .. tostring(err))
  return
end

--
-- The looks, read by the parser the window manager reads them with, and
-- held in this process so a card can paint its look's colours.
--
for _, name in ipairs(LOOKS.order) do
  local palette, said = theme.read(LOOKS[name], "dark")

  theme.install(name, palette)

  for _, why in ipairs(said) do
    print("appearance: " .. name .. ": " .. why)
  end
end

local chosen_look      = LOOKS.order[1]
local chosen_wallpaper = nil       -- a path, or nil for the look's own desk

local status = ui.label{ x = PAD, y = STATUS_Y, w = W - 2 * PAD, text = "",
                         color = "text_dim" }

--
-- **Tell the window manager, then write it down.**
--
-- The look's colours and its faces, both: a look is a whole, and a machine
-- that still held faces somebody picked in the old panel gets the look's
-- back the moment a look is chosen. Written only after the window manager
-- accepted it, so the file cannot hold an appearance the system never
-- applied - and a write that failed says so in the log as well as here
-- (`log appearance`), which is what the ThinkPad needed on 22 September.
--
local function send()
  local look = theme.palettes[chosen_look] or {}
  local colours = {}

  for _, k in ipairs(theme.tokens) do colours[k] = look[k] end

  local reply, why = fs.send("/app/wm", { type = "theme", palette = colours,
                                          fonts = look.fonts })

  if not reply then
    status.text = "refused: " .. tostring(why)
    return nil, why
  end

  local ok, werr = fs.write(SETTINGS, { palette = chosen_look,
                                        wallpaper = chosen_wallpaper })

  if not ok then
    print("appearance: not saved to " .. SETTINGS .. ": " .. tostring(werr))
  end

  status.text = ok and ("saved: " .. (LOOKS.titles[chosen_look] or chosen_look))
                or ("applied, not saved: " .. tostring(werr))

  return reply
end

--------------------------------------------------------------------------
-- The look: four cards, each a desktop in miniature in its own colours -
-- the desk, the Deskbar across it, a focused tab and a window with a
-- selected row - and its name under it.
--------------------------------------------------------------------------

win:add(ui.label{ x = PAD, y = LOOK_Y, w = W - 2 * PAD, text = "Look",
                  role = "heading" })

local cards = ui.view{
  x = PAD, y = CARDS_Y, w = W - 2 * PAD, h = CARD_H,

  draw = function(self, g)
    for i, name in ipairs(LOOKS.order) do
      local p = theme.palettes[name] or {}
      local x = (i - 1) * (CARD_W + GAP)
      local on = (name == chosen_look)
      local mx, my, mw = x + 6, 6, CARD_W - 12
      local ww = mw * 7 // 10

      g:fill(x, 0, CARD_W, CARD_H, "sunken")
      g:frame(x, 0, CARD_W, CARD_H, on and "ring" or "line")

      if on then g:frame(x + 1, 1, CARD_W - 2, CARD_H - 2, "ring") end

      g:fill(mx, my, mw, MINI_H, p.desktop or 0xff000000)
      g:fill(mx, my, mw, 8, p.bar or 0xff808080)
      g:fill(mx + 10, my + 12, 28, 6, p.tab or 0xffffcb00)
      g:fill(mx + 10, my + 18, ww, MINI_H - 24, p.window or 0xffd8d8d8)
      g:frame(mx + 10, my + 18, ww, MINI_H - 24, p.line or 0xff000000)
      g:fill(mx + 13, my + 24, ww - 6, 5, p.accent or 0xff2a55c9)

      g:text(x + 6, 6 + MINI_H + 4 + (M.row - gfx.height()) // 2,
             LOOKS.titles[name] or name, "text")
    end
  end,

  on_click = function(self, x, _)
    local name = LOOKS.order[x // (CARD_W + GAP) + 1]

    if name and name ~= chosen_look then
      chosen_look = name
      send()
    end
  end,
}

win:add(cards)

--------------------------------------------------------------------------
-- The wallpaper.
--
-- Whatever pictures are in `/home`, by name, then the ones the image
-- carries (`assets/wallpapers/`, in a `FULL=1` image), by the photographer
-- who took each - and "none" for the look's own desk. **Centred, never
-- stretched**, which the window manager does and this only names.
--------------------------------------------------------------------------

win:add(ui.label{ x = PAD, y = WALL_Y, w = W - 2 * PAD, text = "Wallpaper",
                  role = "heading" })

-- What each line of the list stands for: the name the window manager is
-- sent - a path in `/home`, or `wallpaper/<file>` in the image.
local wall_path = { ["None - the look's own desk"] = false }

-- `alexander-slattery-LI748t0BK8w.jpg` is Alexander Slattery: the words
-- before Unsplash's eleven-character photo id, each capitalised unless it
-- has a digit in it, which is how a username like `v2osk` is written.
local UNSPLASH_ID = string.rep("[%w_%-]", 11)

local function photographer(name)
  local who = name:match("^wallpaper/(.+)%-" .. UNSPLASH_ID .. "%.jpg$")

  if not who then return nil end

  local words = {}

  for word in who:gmatch("[^%-]+") do
    words[#words + 1] = word:find("%d") and word
                        or (word:sub(1, 1):upper() .. word:sub(2))
  end

  return table.concat(words, " ")
end

local function wallpapers()
  local out = { "None - the look's own desk" }

  for _, name in ipairs(fs.list("/home") or {}) do
    local suffix = name:lower():match("%.([%a]+)$")

    if suffix == "png" or suffix == "jpg" or suffix == "jpeg" then
      out[#out + 1] = name
      wall_path[name] = "/home/" .. name
    end
  end

  for _, name in ipairs(sys.asset() or {}) do
    local who = photographer(name)

    if who and not wall_path[who] then
      out[#out + 1] = who
      wall_path[who] = name
    end
  end

  return out
end

local wall_list = ui.list{
  x = PAD, y = WALL_LIST_Y, w = W - 2 * PAD, h = WALL_H,
  items = wallpapers(),
  on_select = function(_, item)
    chosen_wallpaper = wall_path[item] or nil

    local reply, why = fs.send("/app/wm", { type = "wallpaper",
                                            path = chosen_wallpaper })

    if not reply then
      status.text = "wallpaper: " .. tostring(why)
      return
    end

    -- Saved through the same door as the look, so one file is the record.
    send()
  end,
}

win:add(wall_list)

win:add(status)

--------------------------------------------------------------------------
-- What is in force, so the panel opens saying the truth.
--------------------------------------------------------------------------

local saved = fs.read(SETTINGS)

if type(saved) == "table" then
  if LOOKS[saved.palette] then chosen_look = saved.palette end

  chosen_wallpaper = saved.wallpaper
end

for i, item in ipairs(wall_list.items) do
  if (wall_path[item] or nil) == chosen_wallpaper then
    wall_list.selected = i
  end
end

status.text = "in force: " .. (LOOKS.titles[chosen_look] or chosen_look)

--
-- Said out loud, as it always was here: the window's own size, which the
-- fixed layout makes the same everywhere, how many looks it offers, and
-- which is chosen. `display`'s `appearance` phase holds it.
--
print(("appearance: %dx%d, %d looks, %s"):format(win.w, win.h, #LOOKS.order,
                                                 chosen_look))

--
-- **`--theme plexnight`**, a look chosen from a command line by the path a
-- click takes - so a script can set it, and a test can choose without
-- aiming the pointer at a card. It prints the faces the window manager says
-- it *holds* after the look arrived, which is what was loaded rather than
-- what was asked for.
--
do
  local want = (args or ""):match("%-%-theme%s+(%S+)")

  if want and not LOOKS[want] then
    print("appearance: no look called " .. want)
  elseif want then
    chosen_look = want

    local reply, why = send()
    local held = {}

    for _, role in ipairs(theme.roles) do
      local f = reply and reply.held and reply.held[role]

      held[#held + 1] = role .. "=" .. (f and (f.font .. "/" .. f.px) or "?")
    end

    print(("appearance: theme %s %s, held %s"):format(want,
          reply and "applied" or ("refused: " .. tostring(why)),
          table.concat(held, " ")))
  end
end

win:run()
