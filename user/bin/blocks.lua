-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: section demos
-- Falling Blocks.
--
--   wm blocks
--
--   left / right      move
--   up                rotate
--   down              drop one row faster
--   space             hard drop
--   p                 pause
--   r                 again, after it ends
--
-- **This one is not a direct-rendering window, and that is the interesting
-- part.** `plasma` and `cube3d` both draw every pixel every frame,
-- so they share memory with the compositor and swap buffers. The game changes
-- a handful of cells once or twice a second - so it sends drawing commands
-- like every other window, and the compositor owns its pixels.
--
-- That is the rule `gfx.md` 19.4 states and this is the side of it that
-- rarely gets demonstrated: direct rendering is not the fast road, it is
-- the road for things that redraw wholesale. For a board that mostly sits
-- still, commands are *fewer bytes* than a buffer swap - and the window
-- survives this program hanging, which a direct one does not.

local ui    = use("/lib/ui.lua")
local theme = ui.theme

local COLS, ROWS = 10, 20
local CELL = 22

local BOARD_W, BOARD_H = COLS * CELL, ROWS * CELL
local W = BOARD_W + 150
local H = BOARD_H + 76

local win, err = ui.window{ title = "Falling Blocks", w = W, h = H, x = 200, y = 60 }

if not win then
  print("blocks: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The pieces.
--
-- Each is a list of rotations, each rotation a list of {x, y} cells in a
-- 4x4 box. Written out rather than rotated arithmetically: the classic
-- rotations are not what a naive matrix rotation gives - the I and the S
-- are off by a cell - and the version everybody recognises is the one
-- written down.
--------------------------------------------------------------------------

local PIECES = {
  { colour = 0xff00f0f0, cells = {          -- I
      { {0,1},{1,1},{2,1},{3,1} },
      { {2,0},{2,1},{2,2},{2,3} },
  } },
  { colour = 0xfff0f000, cells = {          -- O
      { {1,0},{2,0},{1,1},{2,1} },
  } },
  { colour = 0xffa000f0, cells = {          -- T
      { {1,0},{0,1},{1,1},{2,1} },
      { {1,0},{1,1},{2,1},{1,2} },
      { {0,1},{1,1},{2,1},{1,2} },
      { {1,0},{0,1},{1,1},{1,2} },
  } },
  { colour = 0xff00f000, cells = {          -- S
      { {1,0},{2,0},{0,1},{1,1} },
      { {1,0},{1,1},{2,1},{2,2} },
  } },
  { colour = 0xfff00000, cells = {          -- Z
      { {0,0},{1,0},{1,1},{2,1} },
      { {2,0},{1,1},{2,1},{1,2} },
  } },
  { colour = 0xff0000f0, cells = {          -- J
      { {0,0},{0,1},{1,1},{2,1} },
      { {1,0},{2,0},{1,1},{1,2} },
      { {0,1},{1,1},{2,1},{2,2} },
      { {1,0},{1,1},{0,2},{1,2} },
  } },
  { colour = 0xfff0a000, cells = {          -- L
      { {2,0},{0,1},{1,1},{2,1} },
      { {1,0},{1,1},{1,2},{2,2} },
      { {0,1},{1,1},{2,1},{0,2} },
      { {0,0},{1,0},{1,1},{1,2} },
  } },
}

--------------------------------------------------------------------------
-- The state.
--------------------------------------------------------------------------

local board = {}                  -- board[r][c] = colour or nil

for r = 1, ROWS do
  board[r] = {}
end

local piece, spin, px, py, next_piece
local score, lines, level = 0, 0, 1
local over, paused = false, false

-- A shuffled bag rather than an independent random pick each time.
--
-- Seven pieces, shuffled, dealt one at a time - so you never wait eleven
-- turns for an I. It is what every version since about 2001 does, and the
-- reason is that pure random *feels* broken even when it is not.
local bag = {}

local seed = 0

local function rand(n)
  -- `math.random` with no seed is the same sequence every run, and there is
  -- no clock to seed from that is worth trusting. The counter is stirred by
  -- how long the player took, which is as good a source as this needs.
  seed = (seed * 1103515245 + 12345) % 2147483648
  return (seed // 65536) % n + 1
end

local function take()
  if #bag == 0 then
    for i = 1, #PIECES do bag[i] = i end

    for i = #bag, 2, -1 do
      local j = rand(i)
      bag[i], bag[j] = bag[j], bag[i]
    end
  end

  return table.remove(bag)
end

local function cells_of(p, s, ox, oy)
  local shape = PIECES[p].cells
  local rot = shape[((s - 1) % #shape) + 1]
  local out = {}

  for i, c in ipairs(rot) do
    out[i] = { c[1] + ox, c[2] + oy }
  end

  return out
end

local function fits(p, s, ox, oy)
  for _, c in ipairs(cells_of(p, s, ox, oy)) do
    local x, y = c[1], c[2]

    if x < 0 or x >= COLS or y >= ROWS then return false end
    if y >= 0 and board[y + 1][x + 1] then return false end
  end

  return true
end

local function spawn()
  piece = next_piece or take()
  next_piece = take()
  spin, px, py = 1, 3, -1

  if not fits(piece, spin, px, py) then over = true end
end

local function lock()
  for _, c in ipairs(cells_of(piece, spin, px, py)) do
    if c[2] >= 0 then
      board[c[2] + 1][c[1] + 1] = PIECES[piece].colour
    end
  end

  -- Full rows out, from the bottom up so that removing one does not move
  -- the row being examined.
  local cleared = 0
  local r = ROWS

  while r >= 1 do
    local full = true

    for c = 1, COLS do
      if not board[r][c] then full = false break end
    end

    if full then
      table.remove(board, r)
      table.insert(board, 1, {})
      cleared = cleared + 1
    else
      r = r - 1
    end
  end

  if cleared > 0 then
    -- The classic scoring: four at once is worth far more than four in a
    -- row, which is the whole reason anybody builds a well.
    score = score + ({ 100, 300, 500, 800 })[cleared] * level
    lines = lines + cleared
    level = 1 + lines // 10
  end

  spawn()
end

local function drop()
  if fits(piece, spin, px, py + 1) then
    py = py + 1
    return true
  end

  lock()
  return false
end

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

local view = ui.view{ x = 12, y = 34, w = BOARD_W + 2, h = BOARD_H + 2 }

local function cell(g, x, y, colour)
  g:fill(1 + x * CELL, 1 + y * CELL, CELL - 1, CELL - 1, colour)
end

function view:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:frame(0, 0, self.w, self.h, theme.line)

  for r = 1, ROWS do
    for c = 1, COLS do
      if board[r][c] then cell(g, c - 1, r - 1, board[r][c]) end
    end
  end

  if piece and not over then
    for _, c in ipairs(cells_of(piece, spin, px, py)) do
      if c[2] >= 0 then cell(g, c[1], c[2], PIECES[piece].colour) end
    end
  end
end

local side = ui.view{ x = BOARD_W + 22, y = 34, w = 116, h = BOARD_H }

function side:draw(g)
  g:text(0, 0, "next", theme.text_dim, theme.window)

  if next_piece then
    for _, c in ipairs(cells_of(next_piece, 1, 0, 0)) do
      g:fill(c[1] * 18, 20 + c[2] * 18, 17, 17, PIECES[next_piece].colour)
    end
  end

  g:text(0, 108, ("score %d"):format(score), theme.text, theme.window)
  g:text(0, 128, ("lines %d"):format(lines), theme.text, theme.window)
  g:text(0, 148, ("level %d"):format(level), theme.text, theme.window)

  if over then
    g:text(0, 184, "game over", theme.bad, theme.window)
    g:text(0, 204, "r to play", theme.text_dim, theme.window)
  elseif paused then
    g:text(0, 184, "paused", theme.text_dim, theme.window)
  end
end

win:add(ui.label{ x = 12, y = 10, w = W - 24,
                  text = "arrows move and rotate, space drops, p pauses",
                  color = "text_dim" })
win:add(view)
win:add(side)

--------------------------------------------------------------------------
-- Falling, and the keyboard.
--------------------------------------------------------------------------

local hz = fs.read("/dev/cpu").counter_hz
local last = sys.ticks()

local function interval()
  -- Faster with each level, and never faster than four rows a second,
  -- which is where the board stops being readable.
  local n = hz // (2 + level)

  return math.max(n, hz // 8)
end

function win:on_frame()
  if over or paused then return false end

  local now = sys.ticks()

  if now - last < interval() then return false end

  last = now
  drop()
  return true
end

local function restart()
  for r = 1, ROWS do board[r] = {} end

  bag = {}
  score, lines, level = 0, 0, 1
  over, paused = false, false
  next_piece = nil
  spawn()
  last = sys.ticks()
end

function win:on_key(c)
  -- The counter is stirred by every key, so the bag's shuffle depends on
  -- how the game was played rather than only on how long it ran.
  seed = seed + c + sys.ticks() % 1024

  if over then
    if c == string.byte("r") then restart() return true end
    return false
  end

  if c == string.byte("p") then paused = not paused return true end
  if paused then return false end

  if c == -4 then                                   -- left
    if fits(piece, spin, px - 1, py) then px = px - 1 end
  elseif c == -3 then                               -- right
    if fits(piece, spin, px + 1, py) then px = px + 1 end
  elseif c == -1 then                               -- up: rotate
    local n = spin + 1
    if fits(piece, n, px, py) then spin = n
    elseif fits(piece, n, px - 1, py) then spin, px = n, px - 1
    elseif fits(piece, n, px + 1, py) then spin, px = n, px + 1 end
  elseif c == -2 then                               -- down
    drop()
  elseif c == 32 then                               -- space: hard drop
    while drop() do end
  else
    return false
  end

  return true
end

restart()
win:run()
