-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Misc_Book
-- kosmos: name Keyboard Shortcuts
-- kosmos: section preferences
-- kosmos: needs screen
--
-- shortcuts: what the keyboard does, asked of the thing that decides.
--
--   Super + / , or from the Kosmos menu
--
--------------------------------------------------------------------------
-- Why this asks rather than knows.
--
-- Every key in here is taken by the window manager *before* any application
-- sees it - that is what a reserved key is - so the only process that can
-- say what they do is the one that takes them. A list written in this file
-- would be a guess about another process's behaviour, and a guess that goes
-- stale silently: the shortcut still works, the window still says something,
-- and the two stop being the same thing without anybody noticing.
--
-- `docs/cheatsheet.html` is the cautionary example rather than a rival. It
-- is a good document and it is a copy, and it was written before the Super
-- key existed, so on the day this was written it listed none of the five
-- most useful keys on the machine. Nothing failed. Nobody was told.
--
-- So `wm.lua` declares each binding with the sentence that describes it, in
-- the same array it builds its dispatch table from - a shortcut cannot be
-- added there without saying what it does - and this window asks for the
-- list. It cannot be out of date, because there is only one of it.
--------------------------------------------------------------------------

local ui = use("/Kosmos/Libraries/ui.lua")
local keys = use("/Kosmos/Libraries/keys.lua")

--
-- Wide enough for the longest sentence in the list, which is a thing to
-- check by looking rather than by counting: at 460 two of them ran off the
-- right edge and were clipped mid-word, and a window whose whole purpose is
-- to be read is the worst place for that.
--
local W, H = 720, 640

--
-- **Modal, from Super and º** (Diego, 3 October 2026: "pressing º should show
-- a modal window of the shortcuts of the kosmos os", Super and º his
-- choice): `--modal`, a popup in the middle of the screen over everything,
-- closed by Escape or a press anywhere outside it - the window manager's
-- rule for popups. From the menu or Super and /, a window as before.
--
local MODAL = tostring(args or ""):match("%-%-modal") ~= nil
local spec = { title = "Shortcuts", w = W, h = H, centre = true }

if MODAL then
  local screen = gfx.screen()
  local sw, sh = 1920, 1080

  if screen then sw, sh = screen:size() end

  spec = { title = "Shortcuts", w = W, h = H, popup = true,
           x = (sw - W) // 2, y = (sh - H) // 2 }
end

local win, err = ui.window(spec)

if not win then
  print("shortcuts: " .. tostring(err))
  return
end

--
-- Asked once, at open. These do not change while the desktop is running -
-- they are compiled into the window manager - so polling for them would be
-- work with a constant answer.
--
local rows = use("/Kosmos/Libraries/wmproto.lua").shortcuts()
local reply = rows and { ok = true, rows = rows } or nil

local groups = {}

if reply and reply.ok then
  --
  -- Editing first, because it is the half somebody already knows and the
  -- fastest way to see that this machine does not want to be learnt from
  -- scratch. The unusual keys come after. Each row says its group
  -- (`wm/keys.lua`); this window, until it is redrawn as `docs/shortcuts.html`
  -- has it, shows them in these three.
  --
  local titles = { { "text", "Editing" },
                   { "desktop", "The Super key" }, { "screen", "The Super key" },
                   { "windows", "Control-W, which introduces a command" } }
  local by_title = {}

  for _, pair in ipairs(titles) do
    local g = by_title[pair[2]]

    if not g then
      g = { title = pair[2], rows = {} }
      by_title[pair[2]] = g
      groups[#groups + 1] = g
    end

    for _, row in ipairs(reply.rows or {}) do
      if row.group == pair[1] and row.keys ~= "" then
        g.rows[#g.rows + 1] = { shown = row.keys, what = row.what }
      end
    end
  end

  for _, row in ipairs(reply.rows) do
    if row.group == "yours" then
      by_title["The Super key"].rows[#by_title["The Super key"].rows + 1] =
        { shown = row.keys, what = "Open " .. tostring(row.program) }
    end
  end
end

--------------------------------------------------------------------------
-- Drawn rather than built out of labels.
--
-- Two columns that have to line up down the whole window, which is the one
-- thing a column of `ui.label` widgets is bad at: each would need its x
-- worked out here anyway, and there would be sixty of them to keep in step
-- when the list from the window manager changes length.
--
-- The key is drawn as keycaps and the description as text, because a key
-- is a thing you press and a sentence is a thing you read.
--------------------------------------------------------------------------
local PAD = 14

local view = ui.view{ x = 0, y = 0, w = W, h = H,
                      follow = { left = true, right = true,
                                 top = true, bottom = true } }

function view:draw(g)
  local GH = gfx.font.h
  local y = PAD

  if #groups == 0 then
    g:text(PAD, y, "The window manager did not answer.", "bad")
    return
  end

  -- The words start where the widest row's keys end, measured as they are
  -- drawn, since a cap's width is its word's in the face of the day.
  local KEY_W = 0

  for _, group in ipairs(groups) do
    for _, row in ipairs(group.rows) do
      KEY_W = math.max(KEY_W, ui.keycaps_width(tostring(row.shown or "?")))
    end
  end

  KEY_W = KEY_W + 18

  -- Said once, for `tools/run_dock.py`: how many keys, and how many of them
  -- were not chords `keys.caps` could draw - which a list spelling a key
  -- wrongly would show as words rather than as keys.
  if not self.said then
    local count, refused = 0, {}

    for _, group in ipairs(groups) do
      for _, row in ipairs(group.rows) do
        count = count + 1

        if not keys.caps(tostring(row.shown or "")) then
          refused[#refused + 1] = tostring(row.shown)
        end
      end
    end

    self.said = true
    print(("shortcuts: %d keys in %d groups, drawn as keycaps; %d not keys%s"):format(
          count, #groups, #refused,
          #refused > 0 and (": " .. table.concat(refused, ", ")) or ""))
  end

  for _, group in ipairs(groups) do
    g:text(PAD, y, group.title, "text_dim")
    y = y + GH + 4

    -- A rule under the heading, the width of the text area. Two pixels of
    -- it, because one is invisible on a screen this dense and three is a
    -- box somebody will read as a table.
    g:fill(PAD, y, self.w - PAD * 2, 1, "line")
    y = y + 6

    -- Each key drawn as the keys themselves (`g:keycaps`), and its words
    -- on the caps' line.
    for _, row in ipairs(group.rows) do
      g:keycaps(PAD, y, tostring(row.shown or "?"))
      g:text(PAD + KEY_W, y + 2, tostring(row.what or ""), "text")
      y = y + ui.keycaps_height() + 4
    end

    y = y + PAD
  end
end

win:add(view)
if MODAL then
  function win:on_key(c)
    if c == 27 then
      self:close()
      return true
    end

    return false
  end
end

win:run()
