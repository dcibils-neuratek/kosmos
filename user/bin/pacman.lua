-- kosmos: application
-- Pac-Man.
--
--   wm pacman
--
--   arrows or WASD    turn
--   Control-C         leave
--
-- A game, which this system had none of, and a fair test of the graphics
-- path: it redraws every pixel of its window thirty times a second, so it
-- takes the direct-rendering road that `plasma` and `cube3d` take -
-- `gfx.md` 19.4, two buffers in memory both this process and the
-- compositor can see, and a `commit` that swaps them. Sending drawing
-- commands for a maze thirty times a second would be the wrong shape.
--
-- What is in Lua and what is in C is the usual division and worth naming
-- because a game is where people reach for C by reflex: Lua decides where
-- five entities are and which way they should turn, which is a few dozen
-- decisions a frame. Every pixel is written by `fill`, `span` or
-- `triangle`, which are C. There is no pixel loop in this file.

local ui  = use("/lib/ui.lua")

-- Double what it was. At sixteen the maze was legible and the game was not
-- playable: a corridor two pixels wider than Pac-Man leaves no room to see
-- a junction coming.
local TILE = 32

-- The maze. `#` wall, `.` dot, `o` power pellet, `-` the ghost door.
--
-- Hand-drawn rather than generated: a maze is level design, and the thing
-- that makes Pac-Man's maze good is that every corridor leads somewhere,
-- which no generator gives you for free.
local MAZE = {
  "###################",
  "#........#........#",
  "#o##.###.#.###.##o#",
  "#.................#",
  "#.##.#.#####.#.##.#",
  "#....#...#...#....#",
  "####.###.#.###.####",
  "   #.#.......#.#   ",
  "####.#.##-##.#.####",
  "    ...#GGG#...    ",
  "####.#.#####.#.####",
  "   #.#.......#.#   ",
  "####.#.#####.#.####",
  "#........#........#",
  "#.##.###.#.###.##.#",
  "#o.#.....P.....#.o#",
  "##.#.#.#####.#.#.##",
  "#....#...#...#....#",
  "#.######.#.######.#",
  "#.................#",
  "###################",
}

local COLS, ROWS = #MAZE[1], #MAZE
local BAR = 28
local W, H = COLS * TILE, ROWS * TILE + BAR

local win, err = ui.window{ title = "Pac-Man", w = W, h = H,
                            x = 150, y = 80, direct = true }

if not win then
  print("pacman: " .. tostring(err))
  return
end

if not win:surface() then
  print("pacman: this window did not get a shared surface")
  return
end

--------------------------------------------------------------------------
-- The board.
--
-- Walls never change, so they are a lookup rather than a string scan per
-- frame. Dots do change, and live in the same grid.
--------------------------------------------------------------------------

local wall, dot, pellet = {}, {}, {}
local dots_left = 0
local start_pac, ghost_home = nil, {}

for r = 1, ROWS do
  wall[r], dot[r], pellet[r] = {}, {}, {}

  for c = 1, COLS do
    local ch = MAZE[r]:sub(c, c)

    wall[r][c]   = (ch == "#")
    dot[r][c]    = (ch == ".")
    pellet[r][c] = (ch == "o")

    if ch == "." or ch == "o" then dots_left = dots_left + 1 end
    if ch == "P" then start_pac = { r = r, c = c } end
    if ch == "G" then ghost_home[#ghost_home + 1] = { r = r, c = c } end
  end
end

local BG      = 0xff000000
local WALL    = 0xff2121de
local DOT     = 0xffdedeb0
local PAC     = 0xffffff00
local TEXT    = 0xffffffff
local GHOSTS  = { 0xffff0000, 0xffffb8ff, 0xff00ffff, 0xffffb851 }

-- The centre of a tile, in pixels.
local function centre(r, c)
  return (c - 1) * TILE + TILE // 2, (r - 1) * TILE + TILE // 2
end

--------------------------------------------------------------------------
-- Entities.
--
-- Position in pixels and direction in tiles. Turning is only allowed at a
-- tile centre, which is the whole of what makes movement in this game feel
-- like it does: a turn taken early slides you into a wall, and a turn taken
-- late is the one you asked for arriving when it can.
--------------------------------------------------------------------------

local DIRS = {
  up    = {  0, -1 },
  down  = {  0,  1 },
  left  = { -1,  0 },
  right = {  1,  0 },
}

local function tile_of(e)
  return (e.y // TILE) + 1, (e.x // TILE) + 1
end

local function aligned(e)
  return (e.x - TILE // 2) % TILE == 0 and (e.y - TILE // 2) % TILE == 0
end

local function open(r, c)
  if r < 1 or r > ROWS or c < 1 or c > COLS then return false end

  return not wall[r][c]
end

local function can_go(e, dir)
  local d = DIRS[dir]
  local r, c = tile_of(e)

  return open(r + d[2], c + d[1])
end

local function make(r, c, colour, speed)
  local x, y = centre(r, c)

  return { x = x, y = y, dir = "left", want = "left",
           colour = colour, speed = speed }
end

local pac = make(start_pac.r, start_pac.c, PAC, 4)
local ghosts = {}

for i, home in ipairs(ghost_home) do
  ghosts[i] = make(home.r, home.c, GHOSTS[i] or GHOSTS[1], 4)
  ghosts[i].dir, ghosts[i].want = "up", "up"
end

local score, lives, over, won = 0, 3, false, false

-- Frames of fright left after a pellet, and what eating a ghost is worth.
--
-- A counter in frames rather than a deadline in `sys.ticks`: the game
-- advances one step per frame and everything else it measures is in frames,
-- so a second clock would be a second thing to keep in step.
local FRIGHT_FRAMES = 300
local fright = 0
local chain  = 0          -- ghosts eaten since this pellet: 200, 400, 800...
local frame  = 0

local FRIGHT_BLUE  = 0xff2121de
local FRIGHT_WHITE = 0xffe0e0e0

local function step(e)
  -- The wanted turn is taken the moment it becomes possible, which is at a
  -- tile centre and only there.
  if aligned(e) then
    if e.want ~= e.dir and can_go(e, e.want) then e.dir = e.want end
    if not can_go(e, e.dir) then return end
  end

  local d = DIRS[e.dir]
  e.x = e.x + d[1] * e.speed
  e.y = e.y + d[2] * e.speed

  -- The tunnel: walking off one side arrives at the other.
  if e.x < 0 then e.x = W - 1 elseif e.x >= W then e.x = 0 end
end

-- Where a ghost goes at a junction.
--
-- The classic rule and it is enough to be alarming: never reverse, and of
-- what is left take whichever direction ends nearer Pac-Man. Each ghost
-- gets a different tie-break so four of them do not walk in a column.
local OPPOSITE = { up = "down", down = "up", left = "right", right = "left" }

local function chase(g, n)
  if not aligned(g) then return end

  local pr, pc = tile_of(pac)
  local gr, gc = tile_of(g)
  local best, best_d

  for _, dir in ipairs({ "up", "left", "down", "right" }) do
    if dir ~= OPPOSITE[g.dir] and can_go(g, dir) then
      local d = DIRS[dir]
      local dr, dc = gr + d[2] - pr, gc + d[1] - pc
      local far = dr * dr + dc * dc + (n * 3)      -- the tie-break

      -- Frightened ghosts want to be far away, which is the same
      -- comparison with the sign turned over. It is not clever pathfinding
      -- and it is what the original did: they are not trying to escape,
      -- they are trying not to approach.
      if fright > 0 then far = -far end

      if not best_d or far < best_d then best, best_d = dir, far end
    end
  end

  -- A dead end: reversing is the only move, and refusing it would freeze.
  g.want = best or OPPOSITE[g.dir]
  g.dir  = g.want
end

--------------------------------------------------------------------------
-- Drawing.
--
-- A disc is a span per row, which is sixteen decisions in Lua and sixteen
-- runs written in C. A per-pixel circle here would be the thing `gfx.md`
-- 19.1 forbids, and it would also be slower.
--------------------------------------------------------------------------

local function disc(s, cx, cy, radius, colour)
  for dy = -radius, radius do
    local dx = math.floor(math.sqrt(radius * radius - dy * dy))
    s:span(cx - dx, cy + dy, dx * 2 + 1, colour)
  end
end

--------------------------------------------------------------------------
-- The maze, drawn once.
--
-- It was redrawn tile by tile every frame: 399 `fill` calls to produce a
-- picture that had not changed, thirty times a second. Direct rendering
-- made the *transport* free and left that in place, which is the trap in
-- "just make it direct" - the pixels were never the bottleneck, the four
-- hundred decisions in front of them were.
--
-- So the maze lives in a surface of its own and each frame starts with one
-- `blit` of it. Eating a dot patches that surface as well as the grid, so
-- the cache stays true without being rebuilt.
--------------------------------------------------------------------------

local board = gfx.surface { w = W, h = H }

local function tile_at(s, r, c)
  local x, y = (c - 1) * TILE, (r - 1) * TILE

  s:fill(x, y, TILE, TILE, BG)

  if wall[r][c] then
    s:fill(x + 1, y + 1, TILE - 2, TILE - 2, WALL)
  elseif dot[r][c] then
    local m = TILE // 2
    s:fill(x + m - 2, y + m - 2, 4, 4, DOT)
  elseif pellet[r][c] then
    disc(s, x + TILE // 2, y + TILE // 2, TILE // 5, DOT)
  end
end

local function paint_board()
  board:fill(0, 0, W, H, BG)

  for r = 1, ROWS do
    for c = 1, COLS do
      tile_at(board, r, c)
    end
  end
end

-- One tile changed; patch the cache rather than repainting the maze.
local function forget(r, c)
  tile_at(board, r, c)
end

--------------------------------------------------------------------------
-- Only what moved.
--
-- The cached maze made the *decisions* cheap and left the pixels alone: one
-- blit of the whole board is 608 x 700, which is 1.7 MB copied per frame,
-- and the compositor then copies it again. That is where the time was
-- going, and it was going there whether anything had moved or not.
--
-- So each frame repairs the boxes the last frame drew in *this* buffer,
-- draws the entities, and commits the union. There are two buffers and they
-- alternate, so what has to be repaired is what was drawn two frames ago -
-- which is why the record is kept per surface rather than as one list.
--
-- Six boxes of 64 x 64 instead of a whole window: about twenty times fewer
-- pixels, and the compositor gets a smaller rectangle to blit as well.
--------------------------------------------------------------------------

local dirt = setmetatable({}, { __mode = "k" })   -- surface -> boxes drawn

-- Eaten dots change the board itself, so both buffers need the tile put
-- back. Applied to the next two frames, which is one of each.
local repairs = {}

local function box_of(e)
  -- Generous: the mouth's wedge reaches a whole tile past the centre.
  return { x = e.x - TILE, y = e.y - TILE, w = TILE * 2, h = TILE * 2 }
end

local function clip_box(b)
  local x0 = math.max(0, b.x)
  local y0 = math.max(0, b.y)
  local x1 = math.min(W, b.x + b.w)
  local y1 = math.min(H, b.y + b.h)

  if x1 <= x0 or y1 <= y0 then return nil end

  return { x = x0, y = y0, w = x1 - x0, h = y1 - y0 }
end

-- The first frame in each buffer has to be the whole thing.
--
-- Damage-based drawing repairs what *it* drew; a buffer nothing has ever
-- drawn into holds nothing to repair, and committing a small box leaves the
-- rest of the window as it was - which, the first time round, is black.
-- Two, because there are two buffers.
local full_frames = 2

local function draw(s)
  local drew = {}

  if full_frames > 0 then
    s:blit(board, 0, 0, W, H, 0, 0)
  end

  local function repair(b)
    b = clip_box(b)
    if b then s:blit(board, b.x, b.y, b.w, b.h, b.x, b.y) end
  end

  -- What this buffer had on it, and any tile whose dot has gone.
  if full_frames <= 0 then
    for _, b in ipairs(dirt[s] or {}) do repair(b) end
    for _, b in ipairs(repairs) do repair(b) end
  end

  -- Pac-Man: a disc with a wedge taken out of it, and the wedge is a
  -- triangle in the background colour. That primitive exists because of the
  -- 3D renderer, and this is the second thing to want it.
  disc(s, pac.x, pac.y, TILE // 2 - 1, PAC)

  local d = DIRS[pac.dir]
  local reach = TILE

  -- The mouth opens and closes. A triangle whose half-width follows a
  -- triangle wave, which is four lines and the whole of the animation
  -- everybody remembers.
  local phase = frame % 16
  local open_ = (phase < 8) and phase or (16 - phase)
  open_ = 2 + open_ * (TILE // 16)

  s:triangle(pac.x, pac.y,
             pac.x + d[1] * reach - d[2] * open_,
             pac.y + d[2] * reach - d[1] * open_,
             pac.x + d[1] * reach + d[2] * open_,
             pac.y + d[2] * reach + d[1] * open_,
             BG)

  for _, g in ipairs(ghosts) do
    local colour = g.colour

    if g.eaten then
      -- Eyes only, on their way home. Nothing to draw but them.
      colour = nil
    elseif fright > 0 then
      -- Blue, and blinking white for the last second and a half so that
      -- running out of time is something you can see coming rather than
      -- something that happens to you.
      colour = FRIGHT_BLUE

      if fright < 90 and (frame // 8) % 2 == 0 then
        colour = FRIGHT_WHITE
      end
    end

    if colour then
      disc(s, g.x, g.y - 2, TILE // 2 - 3, colour)
      s:fill(g.x - TILE // 2 + 3, g.y - 2, TILE - 6, TILE // 2 - 1, colour)
    end

    -- Eyes, looking the way it is going, and all that is left of an eaten
    -- one.
    local e = DIRS[g.dir]
    local eye = (fright > 0 and not g.eaten) and FRIGHT_WHITE or 0xffffffff
    local r = TILE // 8

    s:fill(g.x - r * 2 + e[1] * r, g.y - r * 2 + e[2] * r, r, r, eye)
    s:fill(g.x + r + e[1] * r,     g.y - r * 2 + e[2] * r, r, r, eye)
  end

  for _, e in ipairs({ pac, ghosts[1], ghosts[2], ghosts[3], ghosts[4] }) do
    if e then drew[#drew + 1] = box_of(e) end
  end

  dirt[s] = drew

  local bar = ROWS * TILE
  s:fill(0, bar, W, BAR, BG)
  s:text(8, bar + 6, ("score %d"):format(score), TEXT, BG)

  if fright > 0 then
    s:text(W // 2 - 40, bar + 6, ("fright %d"):format(fright // 30),
           FRIGHT_WHITE, BG)
  end

  s:text(W - 100, bar + 6, ("lives %d"):format(lives), TEXT, BG)

  if over then
    s:text(W // 2 - 36, bar // 2, won and "you win" or "game over", TEXT, BG)
  end
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

-- Painted once, before the first frame.
paint_board()

local hz = fs.read("/dev/cpu").counter_hz
local escape = 0

local function turn(dir) pac.want = dir end

while win.running do
  frame = frame + 1

  if not over then
    if fright > 0 then
      fright = fright - 1

      -- When it runs out, whatever was eyes becomes a ghost again where it
      -- stands, which is where it walked home to.
      if fright == 0 then
        for _, g in ipairs(ghosts) do g.eaten = false end
        chain = 0
      end
    end

    step(pac)

    local r, c = tile_of(pac)

    if dot[r][c] or pellet[r][c] then
      if pellet[r][c] then
        score = score + 50
        fright = FRIGHT_FRAMES
        chain  = 0

        -- Everyone turns round, which is what a pellet does: the moment of
        -- a power pellet is the whole board reversing.
        for _, g in ipairs(ghosts) do
          if not g.eaten then g.dir = OPPOSITE[g.dir] end
        end
      else
        score = score + 10
      end

      dot[r][c], pellet[r][c] = false, false
      forget(r, c)

      repairs[#repairs + 1] = { x = (c - 1) * TILE, y = (r - 1) * TILE,
                                w = TILE, h = TILE, left = 2 }
      dots_left = dots_left - 1

      if dots_left == 0 then over, won = true, true end
    end

    for n, g in ipairs(ghosts) do
      chase(g, n)
      step(g)

      local gr, gc = tile_of(g)

      if gr == r and gc == c and g.eaten then
        -- Already eyes; walking through them costs nothing.
      elseif gr == r and gc == c and fright > 0 then
        -- Eaten. Doubling, as it always did.
        chain = chain + 1
        score = score + 200 * (1 << (chain - 1))

        g.eaten = true
        g.x, g.y = centre(ghost_home[n].r, ghost_home[n].c)
        g.dir = "up"
      elseif gr == r and gc == c then
        lives = lives - 1

        if lives <= 0 then
          over = true
        else
          pac.x, pac.y = centre(start_pac.r, start_pac.c)
          pac.dir, pac.want = "left", "left"
          full_frames = 2

          fright, chain = 0, 0

          for i, home in ipairs(ghost_home) do
            ghosts[i].x, ghosts[i].y = centre(home.r, home.c)
            ghosts[i].dir = "up"
            ghosts[i].eaten = false
          end
        end

        break
      end
    end
  end

  draw(win:surface())

  -- The union of what was repaired and what was drawn, plus the score bar.
  -- One rectangle, because that is what a commit carries - so two entities
  -- at opposite corners cost the box between them, and that is still less
  -- than the window whenever they are not.
  local x0, y0, x1, y1 = W, ROWS * TILE, 0, H

  if full_frames > 0 then
    full_frames = full_frames - 1
    x0, y0, x1, y1 = 0, 0, W, H
  end

  for _, e in ipairs({ pac, ghosts[1], ghosts[2], ghosts[3], ghosts[4] }) do
    if e then
      x0 = math.min(x0, e.x - TILE * 2)
      y0 = math.min(y0, e.y - TILE * 2)
      x1 = math.max(x1, e.x + TILE * 2)
      y1 = math.max(y1, e.y + TILE * 2)
    end
  end

  x0 = math.max(0, x0)
  y0 = math.max(0, y0)
  x1 = math.min(W, x1)
  y1 = math.min(H, y1)

  if not win:commit{ x = x0, y = y0, w = x1 - x0, h = y1 - y0 } then
    break
  end

  -- A repair is needed once per buffer, so it survives exactly two frames.
  for i = #repairs, 1, -1 do
    repairs[i].left = repairs[i].left - 1
    if repairs[i].left <= 0 then table.remove(repairs, i) end
  end

  local reply = fs.send("/dev/wm", { type = "poll", window = win.handle,
                                     wait = 1 })

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "key" then
      local c = ev.code

      -- An arrow is three bytes over a serial line and one keycode on a
      -- keyboard; the console turns the second into the first, so this only
      -- has to understand escape, '[', and a letter.
      if escape == 1 then
        escape = (c == 91) and 2 or 0
      elseif escape == 2 then
        escape = 0
        if c == 65 then turn("up")
        elseif c == 66 then turn("down")
        elseif c == 67 then turn("right")
        elseif c == 68 then turn("left") end
      elseif c == 27 then
        escape = 1
      elseif c == 3 then
        win:close()
      else
        local ch = (c >= 32 and c < 127) and string.char(c):lower() or ""

        if ch == "w" then turn("up")
        elseif ch == "s" then turn("down")
        elseif ch == "a" then turn("left")
        elseif ch == "d" then turn("right")
        elseif ch == "r" and over then
          -- Again.
          score, lives, over, won = 0, 3, false, false
          fright, chain, dots_left = 0, 0, 0

          for rr = 1, ROWS do
            for cc = 1, COLS do
              local m = MAZE[rr]:sub(cc, cc)
              dot[rr][cc]    = (m == ".")
              pellet[rr][cc] = (m == "o")
              if m == "." or m == "o" then dots_left = dots_left + 1 end
            end
          end

          pac.x, pac.y = centre(start_pac.r, start_pac.c)
          paint_board()
          full_frames = 2

          for i, home in ipairs(ghost_home) do
            ghosts[i].x, ghosts[i].y = centre(home.r, home.c)
          end
        end
      end
    end
  end
end
