-- kosmos: application
-- Paint.
--
--   wm paint
--
--   drag on the canvas    draw
--   click a colour        pick it
--   click a size          thin, medium, thick
--   c                     clear
--
-- `gfx.md` 19.7 walks through "Paint drawing a brush stroke" as the example
-- of the whole graphics path, and `docs/` carried an `example-paint.lua`
-- illustrating it - written before the system existed, using `require` and
-- a `gfx.surface(w, h)` that never had that shape. It could not run. This
-- is the same program against the APIs that exist.
--
-- **A direct-rendering window**, because a canvas is a bitmap: the pixels
-- *are* the document, and describing them as drawing commands to be
-- replayed by somebody else would mean keeping the stroke history for ever.
-- So this process owns the memory and the compositor reads it.
--
-- Every stroke is `disc` calls, one per pointer position - a few a frame,
-- each writing a few hundred pixels. `gfx.md` 19.11 has the rule that makes
-- that the right shape rather than a guess.

local ui = use("/lib/ui.lua")

local PAL_W  = 96
local CANVAS_W, CANVAS_H = 520, 380
local BAR_H  = 24
local W, H = PAL_W + CANVAS_W, CANVAS_H + BAR_H

local win, err = ui.window{ title = "Paint", w = W, h = H, x = 120, y = 80,
                            direct = true }

if not win then
  print("paint: " .. tostring(err))
  return
end

if not win:surface() then
  print("paint: this window did not get a shared surface")
  return
end

local COLOURS = {
  0xff000000, 0xffffffff, 0xff808080, 0xffc0c0c0,
  0xffe00000, 0xffe07000, 0xffe0e000, 0xff20a020,
  0xff2060e0, 0xff8020c0, 0xff00c0c0, 0xffe060a0,
}

local SIZES = { 2, 5, 12 }

local colour = COLOURS[1]
local size   = SIZES[2]

-- Smooth edges, on by default because a painting program is the one place
-- a stair-stepped curve is the thing you are looking at. `a` turns it off,
-- which is worth having: the hard edge is what you want when you are
-- drawing something that will be read back pixel by pixel.
local smooth = true

-- The picture, kept by this program because a direct window's buffers are
-- swapped underneath it: whatever was drawn into one is not in the other.
-- The canvas is the truth and each frame copies it across.
local canvas = gfx.surface { w = CANVAS_W, h = CANVAS_H }
canvas:fill(0, 0, CANVAS_W, CANVAS_H, 0xffffffff)

local dirty = nil          -- what changed on the canvas since the last frame
local full  = 2            -- both buffers need a whole frame to begin with

local function touch(x, y, r)
  local b = { x = x - r - 1, y = y - r - 1, w = r * 2 + 3, h = r * 2 + 3 }

  if not dirty then
    dirty = b
  else
    local x0 = math.min(dirty.x, b.x)
    local y0 = math.min(dirty.y, b.y)
    local x1 = math.max(dirty.x + dirty.w, b.x + b.w)
    local y1 = math.max(dirty.y + dirty.h, b.y + b.h)

    dirty = { x = x0, y = y0, w = x1 - x0, h = y1 - y0 }
  end
end

-- A stroke is a line of discs. Interpolated because the pointer arrives a
-- few times a second and a dot per event is a dotted line.
local function stroke(x0, y0, x1, y1)
  local dx, dy = x1 - x0, y1 - y0
  local steps = math.max(math.abs(dx), math.abs(dy), 1)

  for i = 0, steps do
    local x = x0 + dx * i // steps
    local y = y0 + dy * i // steps

    canvas:disc(x, y, size, colour, smooth)
    touch(x, y, size)
  end
end

--------------------------------------------------------------------------

local function draw_palette(s)
  s:fill(0, 0, PAL_W, H, 0xff202830)

  for i, c in ipairs(COLOURS) do
    local col = (i - 1) % 3
    local row = (i - 1) // 3
    local x, y = 8 + col * 28, 8 + row * 28

    s:fill(x, y, 24, 24, c)

    if c == colour then
      s:fill(x - 2, y - 2, 28, 2, 0xffffc700)
      s:fill(x - 2, y + 24, 28, 2, 0xffffc700)
      s:fill(x - 2, y - 2, 2, 28, 0xffffc700)
      s:fill(x + 24, y - 2, 2, 28, 0xffffc700)
    end
  end

  local top = 8 + 4 * 28 + 12

  for i, r in ipairs(SIZES) do
    local y = top + (i - 1) * 34

    s:fill(8, y, 76, 30, (r == size) and 0xff30404e or 0xff182028)
    s:disc(46, y + 15, r, 0xffe0e0e0, smooth)
  end

  local foot = top + 3 * 34 + 8

  s:fill(8, foot, 76, 22, smooth and 0xff30404e or 0xff182028)
  s:text(14, foot + 4, smooth and "smooth" or "hard", 0xffe0e0e0,
         smooth and 0xff30404e or 0xff182028)

  s:text(8, foot + 32, "a smooths", 0xff8b949e, 0xff202830)
  s:text(8, foot + 48, "c clears", 0xff8b949e, 0xff202830)
end

local function present(s)
  if full > 0 then
    full = full - 1
    draw_palette(s)
    s:blit(canvas, 0, 0, CANVAS_W, CANVAS_H, PAL_W, 0)
    s:fill(0, H - BAR_H, W, BAR_H, 0xff202830)
    s:text(PAL_W + 8, H - BAR_H + 5,
           ("%d x %d, drag to draw"):format(CANVAS_W, CANVAS_H),
           0xff8b949e, 0xff202830)
    return { x = 0, y = 0, w = W, h = H }
  end

  draw_palette(s)

  if not dirty then
    return { x = 0, y = 0, w = PAL_W, h = H }
  end

  -- Only the part of the canvas that changed, moved to where the canvas
  -- sits in the window.
  local x = math.max(0, dirty.x)
  local y = math.max(0, dirty.y)
  local w = math.min(CANVAS_W, dirty.x + dirty.w) - x
  local h = math.min(CANVAS_H, dirty.y + dirty.h) - y

  if w <= 0 or h <= 0 then
    return { x = 0, y = 0, w = PAL_W, h = H }
  end

  s:blit(canvas, x, y, w, h, PAL_W + x, y)

  return { x = 0, y = 0, w = PAL_W + x + w, h = H }
end

--------------------------------------------------------------------------

local drawing = false
local last_x, last_y

while win.running do
  -- Both buffers need every stroke, so the dirty box is repaired in this
  -- one and kept until the other has had it too.
  local pending = dirty
  local damage = present(win:surface())

  if not win:commit(damage) then break end

  --
  -- Kept for one more frame, which is the other buffer.
  --
  -- Written as an `if`, because `a and b and nil or c` is not a
  -- conditional: `... and nil` is nil whatever came before it, so the `or`
  -- always won and a rectangle that had already been repeated was repeated
  -- again, with `again` set once more. The damage never retired - every
  -- stroke was re-composited for the life of the program.
  --
  if not pending or pending.again then
    dirty = nil
  else
    dirty = { x = pending.x, y = pending.y,
              w = pending.w, h = pending.h, again = true }
  end

  local reply = fs.send("/dev/wm", { type = "poll", window = win.handle,
                                     wait = 1 })

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "key" then
      if ev.code == 3 then
        win:close()
      elseif ev.code == string.byte("a") or ev.code == string.byte("A") then
        smooth = not smooth
      elseif ev.code == string.byte("c") or ev.code == string.byte("C") then
        canvas:fill(0, 0, CANVAS_W, CANVAS_H, 0xffffffff)
        full = 2
      end
    elseif ev.type == "mouse" then
      local x, y = ev.x, ev.y

      if ev.action == "press" then
        if x < PAL_W then
          -- The palette.
          local col = (x - 8) // 28
          local row = (y - 8) // 28
          local i = row * 3 + col + 1

          if x >= 8 and col < 3 and COLOURS[i] and y >= 8 and row < 4 then
            colour = COLOURS[i]
          end

          local top = 8 + 4 * 28 + 12

          for n, r in ipairs(SIZES) do
            local ty = top + (n - 1) * 34

            if y >= ty and y < ty + 30 then size = r end
          end

          local foot = top + 3 * 34 + 8

          if y >= foot and y < foot + 22 then smooth = not smooth end
        elseif y < CANVAS_H then
          drawing = true
          last_x, last_y = x - PAL_W, y
          stroke(last_x, last_y, last_x, last_y)
        end
      elseif ev.action == "release" then
        drawing = false
      elseif ev.action == "move" and drawing then
        local cx, cy = x - PAL_W, y

        if cx >= 0 and cy >= 0 and cx < CANVAS_W and cy < CANVAS_H then
          stroke(last_x, last_y, cx, cy)
          last_x, last_y = cx, cy
        end
      end
    end
  end
end
