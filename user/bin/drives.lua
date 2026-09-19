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

local PAD = 12
local ROW = gfx.height("ui") + 6

-- Laid out from the top, and the window as tall as what is in it.
local DRIVES_Y = PAD
local DRIVES_H = ROW * 4 + 4
local MAP_Y = DRIVES_Y + DRIVES_H + 18
local MAP_H = 54
local PARTS_Y = MAP_Y + MAP_H + 30
local PARTS_H = ROW * 5 + 4
local BUTTONS_Y = PARTS_Y + PARTS_H + 12
local STATUS_Y = BUTTONS_Y + ROW + 16
local W, H = 640, STATUS_Y + ROW + PAD

local win, err = ui.window{ title = "Drives", w = W, h = H, x = 180, y = 90 }

if not win then
  print("drives: " .. tostring(err))
  return
end

local drives = drivelist.drives()
local chosen, part = 1, 1

-- The columns, as the drawing has them.
local DRIVE_COLS = { { "Drive", 8 }, { "Type", 300 }, { "Size", 420 } }
local PART_COLS = { { "#", 8 }, { "Name", 34 }, { "Filesystem", 210 },
                    { "Size", 320 }, { "Used", 400 }, { "In Tracker", 470 } }

local function header(g, cols, w)
  g:fill(0, 0, w, ROW, "raised")
  g:fill(0, ROW, w, 1, "line_soft")

  for _, c in ipairs(cols) do
    g:text(c[2], 3, c[1], "text_dim", "raised")
  end
end

local function drive_of() return drives[chosen] end

--
-- The drives, one row each.
--
local drive_rows = ui.view{
  x = PAD, y = DRIVES_Y, w = W - PAD * 2, h = DRIVES_H,

  draw = function(self, g)
    g:sunken(0, 0, self.w, self.h, "sunken")
    header(g, DRIVE_COLS, self.w)

    if #drives == 0 then
      g:text(8, ROW + 4, "No drives: nothing is plugged in, and there is no "
             .. "disk.", "text_dim", "sunken")
      return
    end

    for i, d in ipairs(drives) do
      local y = ROW + 2 + (i - 1) * ROW
      local bg = (i == chosen) and "accent" or "sunken"

      if i == chosen then g:fill(2, y, self.w - 4, ROW, "accent") end

      g:text(DRIVE_COLS[1][2], y + 3, d.name, "text", bg)
      g:text(DRIVE_COLS[2][2], y + 3, d.kind, "text", bg)
      g:text(DRIVE_COLS[3][2], y + 3, drivelist.size(d.bytes), "text", bg)
    end
  end,

  on_click = function(self, x, y)
    local i = (y - ROW - 2) // ROW + 1

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
  x = PAD, y = MAP_Y, w = W - PAD * 2, h = MAP_H,

  draw = function(self, g)
    local d = drive_of()

    g:sunken(0, 0, self.w, self.h, "window")

    if not d then return end

    local pieces = {}

    for i, v in ipairs(d.volumes) do
      pieces[#pieces + 1] = { text = v.name, sub = v.filesystem .. ", "
                              .. drivelist.size(v.bytes), volume = i }
    end

    if (d.unclaimed or 0) > 1024 * 1024 then
      pieces[#pieces + 1] = {
        text = d.internal and "Not read yet" or "Free or unread",
        sub = drivelist.size(d.unclaimed), free = true,
      }
    end

    if #pieces == 0 then return end

    local x, each = 2, (self.w - 4) // #pieces

    for i, p in ipairs(pieces) do
      local w = (i == #pieces) and (self.w - 2 - x) or each
      local face = p.free and "window"
                   or (p.volume == part and "accent" or "raised")

      if p.free then
        g:fill(x, 2, w - 2, self.h - 4, "window")
        g:frame(x, 2, w - 2, self.h - 4, "line_soft")
      else
        g:raised(x, 2, w - 2, self.h - 4, face)
      end

      g:text(x + 8, 10, p.text, "text", face)
      g:text(x + 8, 10 + ROW, p.sub, "text_dim", face)
      x = x + w
    end
  end,

  on_click = function(self, x)
    local d = drive_of()

    if not d or #d.volumes == 0 then return end

    local count = #d.volumes + (((d.unclaimed or 0) > 1024 * 1024) and 1 or 0)
    local i = (x - 2) // ((self.w - 4) // count) + 1

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
  x = PAD, y = PARTS_Y, w = W - PAD * 2, h = PARTS_H,

  draw = function(self, g)
    local d = drive_of()

    g:sunken(0, 0, self.w, self.h, "sunken")
    header(g, PART_COLS, self.w)

    if not d then return end

    if d.internal then
      g:text(8, ROW + 4, "The machine's own disk. Its partitions are not "
             .. "read yet.", "text_dim", "sunken")
      return
    end

    if #d.volumes == 0 then
      g:text(8, ROW + 4, "No filesystem Kosmos reads on this drive.",
             "text_dim", "sunken")
      return
    end

    for i, v in ipairs(d.volumes) do
      local y = ROW + 2 + (i - 1) * ROW
      local bg = (i == part) and "accent" or "sunken"
      local used = (v.bytes or 0) - (v.free or 0)

      if i == part then g:fill(2, y, self.w - 4, ROW, "accent") end

      g:text(PART_COLS[1][2], y + 3, tostring((v.partition or 0) + 1), "text", bg)
      g:text(PART_COLS[2][2], y + 3, v.name, "text", bg)
      g:text(PART_COLS[3][2], y + 3, v.filesystem, "text", bg)
      g:text(PART_COLS[4][2], y + 3, drivelist.size(v.bytes), "text", bg)
      g:text(PART_COLS[5][2], y + 3,
             v.free_exact and drivelist.size(used) or "-", "text", bg)
      g:text(PART_COLS[6][2], y + 3,
             v.readable and drivelist.path(v) or "not opened",
             v.readable and "text" or "text_dim", bg)
    end
  end,

  on_click = function(self, x, y)
    local d = drive_of()
    local i = (y - ROW - 2) // ROW + 1

    if d and d.volumes[i] then
      part = i
      win:paint()
    end
  end,
}

local status = ui.label{ x = PAD, y = STATUS_Y, w = W - PAD * 2, text = "" }

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

win:add(ui.label{ x = PAD, y = MAP_Y - 18, w = 300, text = "Partitions" })
win:add(drive_rows)
win:add(map)
win:add(part_rows)
win:add(ui.button{ x = PAD, y = BUTTONS_Y, w = 150, text = "Open in Tracker",
                   on_click = open_in_tracker })
win:add(ui.button{ x = PAD + 160, y = BUTTONS_Y, w = 120, text = "Format...",
                   disabled = true })
win:add(ui.button{ x = PAD + 290, y = BUTTONS_Y, w = 150,
                   text = "New partition...", disabled = true })
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
