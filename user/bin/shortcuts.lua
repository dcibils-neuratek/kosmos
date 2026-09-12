-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Misc_Book
-- kosmos: section Applications
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

local ui = use("/lib/ui.lua")

--
-- Wide enough for the longest sentence in the list, which is a thing to
-- check by looking rather than by counting: at 460 two of them ran off the
-- right edge and were clipped mid-word, and a window whose whole purpose is
-- to be read is the worst place for that.
--
local W, H = 580, 420

local win, err = ui.window{ title = "Shortcuts", w = W, h = H, centre = true }

if not win then
  print("shortcuts: " .. tostring(err))
  return
end

--
-- Asked once, at open. These do not change while the desktop is running -
-- they are compiled into the window manager - so polling for them would be
-- work with a constant answer.
--
local reply = fs.send("/app/wm", { type = "shortcuts" })

local groups = {}

if reply and reply.ok then
  --
  -- Editing first, because it is the half somebody already knows and the
  -- fastest way to see that this machine does not want to be learnt from
  -- scratch. The unusual keys come after.
  --
  groups[#groups + 1] = { title = "Editing", rows = reply.edit or {} }
  groups[#groups + 1] = { title = "The Super key", rows = reply.super or {} }
  groups[#groups + 1] = { title = "Control-W, which introduces a command",
                          rows = reply.prefix or {} }
end

--------------------------------------------------------------------------
-- Drawn rather than built out of labels.
--
-- Two columns that have to line up down the whole window, which is the one
-- thing a column of `ui.label` widgets is bad at: each would need its x
-- worked out here anyway, and there would be sixty of them to keep in step
-- when the list from the window manager changes length.
--
-- The key goes in the mono face and the description in the interface one,
-- because a key is a thing you press and a sentence is a thing you read -
-- the same distinction the Terminal draws, for the same reason.
--------------------------------------------------------------------------
local PAD = 14
local KEY_W = 170

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

  for _, group in ipairs(groups) do
    g:text(PAD, y, group.title, "text_dim")
    y = y + GH + 4

    -- A rule under the heading, the width of the text area. Two pixels of
    -- it, because one is invisible on a screen this dense and three is a
    -- box somebody will read as a table.
    g:fill(PAD, y, self.w - PAD * 2, 1, "line")
    y = y + 6

    for _, row in ipairs(group.rows) do
      g:text(PAD, y, tostring(row.shown or "?"), "text", nil, "mono")
      g:text(PAD + KEY_W, y, tostring(row.what or ""), "text")
      y = y + GH + 3
    end

    y = y + PAD
  end
end

win:add(view)
win:run()
