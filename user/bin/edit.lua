-- A screen editor, so the machine can write its own Lua.
--
--   edit /data/hello.lua       open it, or start it empty
--
--   arrows          move            Ctrl-S   save
--   Home / End      line ends       Ctrl-Q   quit
--   Backspace       delete back     Ctrl-C   quit
--   Enter           split the line
--
-- Then `run /data/hello.lua` from the shell.
--
--------------------------------------------------------------------------
-- Why this exists this early.
--
-- Everything above the kernel here is Lua, and Lua is text, and until now
-- the only way to change any of it was to edit a file on a build machine
-- and make a new image. A system whose whole design rests on being able to
-- replace a running server cannot be one you have to reboot to type into.
--
-- It is a plain array of lines and a cursor. No undo, no selection, no
-- syntax colouring yet - each of those is worth having and none of them is
-- worth delaying the thing that makes the machine self-sufficient.
--------------------------------------------------------------------------

local path = tostring(args or ""):match("^%s*(%S+)")

if not path then
  print("usage: edit <path>")
  return
end

local screen = gfx.screen()

if not screen then
  print("edit: this process was not given the screen")
  return
end

local W, H = screen:size()
local GW, GH = gfx.font.w, gfx.font.h

local BG      = 0xff0d1117
local FG      = 0xffc9d1d9
local STATUS  = 0xff1f6feb
local STATUS2 = 0xff21262d
local GUTTER  = 0xff484f58
local CURSOR  = 0xff58a6ff

-- One row for the status line at the bottom, and the gutter is wide enough
-- for four digits of line number plus a space.
local GUTTER_COLS = 5
local ROWS = (H // GH) - 1
local COLS = (W // GW) - GUTTER_COLS

-- Composed here and blitted out, so a redraw is never seen half-done. The
-- same reason the window manager has one.
local back = gfx.surface{ w = W, h = H }

--------------------------------------------------------------------------
-- The text.
--------------------------------------------------------------------------

local lines = {}
local cy, cx = 1, 1          -- cursor, one-based, cx is "before this column"
local top = 1                -- first visible line
local dirty = false
local message = ""

do
  local text = fs.read(path)

  if type(text) == "string" then
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
      lines[#lines + 1] = line
    end
    -- The split above leaves one empty line for the trailing newline.
    if #lines > 1 and lines[#lines] == "" then lines[#lines] = nil end
    message = ("%d lines"):format(#lines)
  else
    message = "new file"
  end
end

if #lines == 0 then lines[1] = "" end

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

local function scroll_into_view()
  if cy < top then top = cy end
  if cy > top + ROWS - 1 then top = cy - ROWS + 1 end
  if top < 1 then top = 1 end
end

local function draw()
  scroll_into_view()
  back:fill(0, 0, W, H, BG)

  for row = 0, ROWS - 1 do
    local n = top + row
    local line = lines[n]

    if line then
      back:text(0, row * GH, ("%4d "):format(n), GUTTER, BG)

      -- Only what fits. Horizontal scrolling is not here yet, and a long
      -- line clipped is better than a long line wrapped into the next
      -- line's number.
      back:text(GUTTER_COLS * GW, row * GH, line:sub(1, COLS), FG, BG)
    end
  end

  -- The cursor as a block on the character it is on, which is what a
  -- terminal does and what makes the column obvious in indented code.
  if cy >= top and cy <= top + ROWS - 1 then
    local px = (GUTTER_COLS + math.min(cx, COLS + 1) - 1) * GW
    local py = (cy - top) * GH
    local under = lines[cy]:sub(cx, cx)

    back:fill(px, py, GW, GH, CURSOR)

    if under ~= "" then
      back:text(px, py, under, BG, CURSOR)
    end
  end

  -- The status line.
  local y = ROWS * GH
  back:fill(0, y, W, GH, STATUS2)

  local left = ("%s%s  %d,%d"):format(path, dirty and " *" or "", cy, cx)
  back:text(4, y, left, 0xffffffff, STATUS2)

  local right = "^S save  ^Q quit  " .. message
  local rx = W - (#right + 1) * GW

  if rx > (#left + 3) * GW then
    back:text(rx, y, right, 0xff8b949e, STATUS2)
  end

  screen:blit(back, 0, 0, W, H, 0, 0)
end

--------------------------------------------------------------------------
-- Editing.
--------------------------------------------------------------------------

local function insert(ch)
  local line = lines[cy]
  lines[cy] = line:sub(1, cx - 1) .. ch .. line:sub(cx)
  cx = cx + #ch
  dirty = true
end

local function split_line()
  local line = lines[cy]
  local rest = line:sub(cx)
  lines[cy] = line:sub(1, cx - 1)
  table.insert(lines, cy + 1, rest)
  cy = cy + 1
  cx = 1
  dirty = true
end

local function backspace()
  if cx > 1 then
    local line = lines[cy]
    lines[cy] = line:sub(1, cx - 2) .. line:sub(cx)
    cx = cx - 1
    dirty = true
  elseif cy > 1 then
    -- Joining onto the end of the line above, which is where the cursor
    -- has to land or the join is invisible.
    local above = lines[cy - 1]
    cx = #above + 1
    lines[cy - 1] = above .. lines[cy]
    table.remove(lines, cy)
    cy = cy - 1
    dirty = true
  end
end

local function clamp()
  if cy < 1 then cy = 1 end
  if cy > #lines then cy = #lines end
  if cx < 1 then cx = 1 end
  if cx > #lines[cy] + 1 then cx = #lines[cy] + 1 end
end

local function save()
  local ok, err = fs.write(path, table.concat(lines, "\n") .. "\n")

  if ok then
    dirty = false
    message = ("saved, %d lines"):format(#lines)
  else
    message = "could not save: " .. tostring(err)
  end
end

--------------------------------------------------------------------------
-- Keys.
--
-- Arrows arrive as escape, '[', then a letter. Collected across calls,
-- because the three bytes do not have to turn up in the same drain.
--------------------------------------------------------------------------

local escape = 0
local running = true

local function key(c)
  if escape == 1 then
    escape = (c == 91) and 2 or 0        -- '['
    return
  end

  if escape == 2 then
    escape = 0

    if c == 65 then cy = cy - 1                                    -- up
    elseif c == 66 then cy = cy + 1                                -- down
    elseif c == 67 then                                            -- right
      if cx > #lines[cy] then
        if cy < #lines then cy = cy + 1; cx = 1 end
      else
        cx = cx + 1
      end
    elseif c == 68 then                                            -- left
      if cx == 1 then
        if cy > 1 then cy = cy - 1; cx = #lines[cy] + 1 end
      else
        cx = cx - 1
      end
    elseif c == 72 then cx = 1                                     -- Home
    elseif c == 70 then cx = #lines[cy] + 1                        -- End
    end

    clamp()
    return
  end

  if c == 27 then escape = 1 return end
  if c == 3 or c == 17 then running = false return end             -- ^C, ^Q
  if c == 19 then save() return end                                -- ^S
  if c == 1 then cx = 1 return end                                 -- ^A
  if c == 5 then cx = #lines[cy] + 1 return end                    -- ^E
  if c == 10 or c == 13 then split_line() return end
  if c == 8 or c == 127 then backspace() return end
  if c == 9 then insert("  ") return end                           -- Tab: two

  if c >= 32 and c < 127 then insert(string.char(c)) end
end

--------------------------------------------------------------------------
-- The loop. Redraw only when something changed, so an idle editor costs a
-- yield and one call to the console rather than a full-screen compose.
--------------------------------------------------------------------------

draw()

while running do
  local keys = fs.keys("/dev/console") or {}

  if #keys > 0 then
    for _, c in ipairs(keys) do
      key(c)
      if not running then break end
    end

    clamp()
    draw()
  end

  sys.yield()
end

screen:fill(0, 0, W, H, BG)
back:free()

if dirty then
  print(("edit: %s was not saved"):format(path))
end
