-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Drives: every drive this machine found, how it is split, and what is on it.
-- kosmos: application
-- kosmos: icon Device_Harddisk
--
-- USB step 6e, drawn first in `docs/drives.html` and approved on 14
-- September: "Tracker is for your files. Drives is for the drives
-- themselves" - what Haiku calls DriveSetup, Windows Disk Management and
-- macOS Disk Utility. **It shows before it changes anything**: Format... and
-- New partition... are drawn and greyed, because a mistake there erases a
-- drive, and they come one at a time after this has been lived with.
--
-- Three parts, as the drawing has them: the drives; the chosen drive's
-- partitions as a bar, not to scale, so a small partition beside a large
-- free space is still something you can see; and its partitions as rows,
-- with the one chosen opening in Tracker. The model is `drivelist.lua`, so
-- a prompt can ask it the same questions.

local ui = use("/lib/ui.lua")
local drivelist = use("/lib/drivelist.lua")

--
-- **The drawings' page** (`docs/apps.html`, `roadmap.md` 5zp): a header
-- with Open in Tracker, then a group's name, its card, 20, the next - the
-- kit's `ui.layout` numbers, where this had its own 12 and 18 and 30. The
-- tables are the drawings' tables: a card, a column head 32 tall over a
-- rule, rows of 32, and the chosen row a quiet fill rather than the accent.
--
local L = ui.layout
local ROW = 32

local W = 640
local DRIVES_Y = L.head + L.page_top
local DRIVES_H = ROW * 4 + 2
local MAP_Y = DRIVES_Y + L.to_card + DRIVES_H + L.between
local MAP_H = 54
local PARTS_Y = MAP_Y + L.to_card + MAP_H + 14
local PARTS_H = ROW * 5 + 2
local STATUS_Y = PARTS_Y + PARTS_H + 10
local H = STATUS_Y + gfx.height() + L.page_foot

local win, err = ui.window{ title = "Drives", w = W, h = H, x = 180, y = 90 }

if not win then
  print("drives: " .. tostring(err))
  return
end

local drives = drivelist.drives()
local chosen, part = 1, 1

-- The columns, as the drawing has them.
local DRIVE_COLS = { { "Drive", 14 }, { "Type", 330 }, { "Size", 470 } }
local PART_COLS = { { "#", 14 }, { "Name", 40 }, { "Filesystem", 210 },
                    { "Size", 320 }, { "Used", 400 }, { "In Tracker", 470 } }

--
-- A table's card and its column head: the drawings' `.th`, 32 tall, in the
-- dim `ui` face, over a one-pixel rule.
--
local function table_card(g, cols, w, h)
  g:fill_round(0, 0, w, h, "sunken", L.card_r)
  g:frame_round(0, 0, w, h, "line_soft", L.card_r)
  g:fill(1, ROW, w - 2, 1, "line_soft")

  for _, c in ipairs(cols) do
    g:text(c[2], (ROW - gfx.height()) // 2, c[1], "text_dim", "sunken")
  end
end

-- A row's words, centred in its 32.
local function cell(g, x, row_y, text, color)
  g:text(x, row_y + (ROW - gfx.height()) // 2, text, color or "text")
end

local function drive_of() return drives[chosen] end

--
-- The drives, one row each.
--
local drive_rows = ui.view{
  x = L.page_side, y = DRIVES_Y + L.to_card, w = W - 2 * L.page_side,
  h = DRIVES_H,

  draw = function(self, g)
    table_card(g, DRIVE_COLS, self.w, self.h)

    if #drives == 0 then
      cell(g, 14, ROW + 1, "No drives: nothing is plugged in, and there is "
           .. "no disk.", "text_dim")
      return
    end

    for i, d in ipairs(drives) do
      local y = ROW + 1 + (i - 1) * ROW

      if i == chosen then g:fill(1, y, self.w - 2, ROW, "line_soft") end

      cell(g, DRIVE_COLS[1][2], y, d.name)
      cell(g, DRIVE_COLS[2][2], y, d.kind, "text_dim")
      cell(g, DRIVE_COLS[3][2], y, drivelist.size(d.bytes))
    end
  end,

  on_click = function(self, x, y)
    local i = (y - ROW - 1) // ROW + 1

    if drives[i] then
      chosen, part = i, 1
      win:paint()
    end
  end,
}

--
-- The chosen drive's partitions as a bar. Not to scale: drawn to scale, a
-- 192 MB partition on a 115 GB stick is a pixel. Each volume is at least a
-- label's width; the space no volume claims takes what is left.
--
local map = ui.view{
  x = L.page_side, y = MAP_Y + L.to_card, w = W - 2 * L.page_side,
  h = MAP_H,

  draw = function(self, g)
    local d = drive_of()

    if not d then return end

    local pieces = {}

    for i, v in ipairs(d.volumes) do
      pieces[#pieces + 1] = { text = v.name, sub = v.filesystem .. " · "
                              .. drivelist.size(v.bytes), volume = i }
    end

    if (d.unclaimed or 0) > 1024 * 1024 then
      pieces[#pieces + 1] = {
        text = d.internal and "Not read yet" or "Free or unread",
        sub = drivelist.size(d.unclaimed), free = true,
      }
    end

    if #pieces == 0 then return end

    --
    -- Each a rounded block 7 apart, the chosen one in the accent: the
    -- drawings' controls, where these were bevelled boxes touching.
    --
    local gap = 7
    local x, each = 0, (self.w - gap * (#pieces - 1)) // #pieces

    for i, p in ipairs(pieces) do
      local w = (i == #pieces) and (self.w - x) or each
      local on = (p.volume == part)
      local fill = p.free and "window" or (on and "accent" or "sunken")

      g:fill_round(x, 0, w, self.h, fill, 7)

      if not on then g:frame_round(x, 0, w, self.h, "line_soft", 7) end

      local ty = (self.h - gfx.height("label") - gfx.height()) // 2

      g:text(x + 12, ty, p.text, on and "text_on" or "text", nil, "label")
      g:text(x + 12, ty + gfx.height("label"), p.sub,
             on and "text_on" or "text_dim")
      x = x + w + gap
    end
  end,

  on_click = function(self, x)
    local d = drive_of()

    if not d or #d.volumes == 0 then return end

    local count = #d.volumes + (((d.unclaimed or 0) > 1024 * 1024) and 1 or 0)
    local i = x // ((self.w + 7) // count) + 1

    if d.volumes[i] then
      part = i
      win:paint()
    end
  end,
}

--
-- Its partitions, as rows, and where each opens.
--
local part_rows = ui.view{
  x = L.page_side, y = PARTS_Y, w = W - 2 * L.page_side, h = PARTS_H,

  draw = function(self, g)
    local d = drive_of()

    table_card(g, PART_COLS, self.w, self.h)

    if not d then return end

    if d.internal then
      cell(g, 14, ROW + 1, "The machine's own disk. Its partitions are not "
           .. "read yet.", "text_dim")
      return
    end

    if #d.volumes == 0 then
      cell(g, 14, ROW + 1, "No filesystem Kosmos reads on this drive.",
           "text_dim")
      return
    end

    for i, v in ipairs(d.volumes) do
      local y = ROW + 1 + (i - 1) * ROW
      local used = (v.bytes or 0) - (v.free or 0)

      if i == part then g:fill(1, y, self.w - 2, ROW, "line_soft") end

      cell(g, PART_COLS[1][2], y, tostring((v.partition or 0) + 1), "text_dim")
      cell(g, PART_COLS[2][2], y, v.name)
      cell(g, PART_COLS[3][2], y, v.filesystem, "text_dim")
      cell(g, PART_COLS[4][2], y, drivelist.size(v.bytes))
      cell(g, PART_COLS[5][2], y, v.free_exact and drivelist.size(used) or "-")
      cell(g, PART_COLS[6][2], y,
           v.readable and drivelist.path(v) or "not opened",
           v.readable and "text" or "text_dim")
    end
  end,

  on_click = function(self, x, y)
    local d = drive_of()
    local i = (y - ROW - 1) // ROW + 1

    if d and d.volumes[i] then
      part = i
      win:paint()
    end
  end,
}

local status = ui.label{ x = L.page_side + 3, y = STATUS_Y,
                         w = W - 2 * L.page_side - 3, text = "",
                         color = "text_dim", role = "ui" }

local function open_in_tracker()
  local d = drive_of()
  local v = d and d.volumes[part]

  if not v or not v.readable then
    status.text = "Choose a filesystem Kosmos reads first."
    win:paint()
    return
  end

  local reply, why = fs.send("/app/wm", { type = "launch", program = "tracker",
                                          args = drivelist.path(v) })

  status.text = reply and ("Opened " .. drivelist.path(v) .. " in Tracker.")
                or ("Tracker would not open: " .. tostring(why))
  win:paint()
end

--
-- The header: how many drives, Open in Tracker as the verb, and behind the
-- dots the two that change a drive - greyed, as `docs/drives.html` has them:
-- "formatting and partitioning later, each on its own". Shown rather than
-- left out, because a Drives window that cannot say it will one day format
-- a stick is one where a person goes looking for how.
--
local more = ui.iconbutton{ icon = "more" }

local header = ui.header{
  x = 0, y = 0, w = W, title = "Drives",
  sub = (#drives == 1) and "1 drive" or (#drives .. " drives"),
  right = { ui.button{ text = "Open in Tracker", on_click = open_in_tracker },
            more },
}

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, {
    { text = "Format...", disabled = true },
    { text = "New partition...", disabled = true },
  })
end

local function heading(y, text)
  win:add(ui.label{ x = L.page_side + 3,
                    y = y + (L.group - gfx.height("heading")) // 2,
                    w = 300, text = text, role = "heading" })
end

win:add(header)
heading(DRIVES_Y, "Drives")
win:add(drive_rows)
heading(MAP_Y, "Partitions")
win:add(map)
win:add(part_rows)
win:add(status)

--
-- Plugged in and pulled out: asked again every second or so, which is two
-- servers answering what they already hold - nothing is read off a stick.
--
local watcher = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function watcher:tick()
  local now = drivelist.drives()
  local before, after = {}, {}

  for _, d in ipairs(drives) do before[#before + 1] = d.name .. #d.volumes end
  for _, d in ipairs(now) do after[#after + 1] = d.name .. #d.volumes end

  if table.concat(before, "|") ~= table.concat(after, "|") then
    drives = now
    if chosen > #drives then chosen, part = 1, 1 end
    win:paint()
  end
end

win:add(watcher)

-- Said once, as a window's first facts are, for whoever reads the log.
print(("drives: %d drive(s): %s"):format(#drives, (function()
  local names = {}
  for _, d in ipairs(drives) do
    names[#names + 1] = ("%s, %s, %d volume(s)"):format(d.name,
                                                         drivelist.size(d.bytes),
                                                         #d.volumes)
  end
  return #names > 0 and table.concat(names, "; ") or "none"
end)()))

win:run()
