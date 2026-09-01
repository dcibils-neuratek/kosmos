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

local TILE = 16

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
local BAR = 24
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

local pac = make(start_pac.r, start_pac.c, PAC, 2)
local ghosts = {}

for i, home in ipairs(ghost_home) do
  ghosts[i] = make(home.r, home.c, GHOSTS[i] or GHOSTS[1], 2)
  ghosts[i].dir, ghosts[i].want = "up", "up"
end

local score, lives, over, won = 0, 3, false, false

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

local function draw(s)
  s:fill(0, 0, W, H, BG)

  for r = 1, ROWS do
    for c = 1, COLS do
      local x, y = (c - 1) * TILE, (r - 1) * TILE

      if wall[r][c] then
        s:fill(x + 1, y + 1, TILE - 2, TILE - 2, WALL)
      elseif dot[r][c] then
        s:fill(x + TILE // 2 - 1, y + TILE // 2 - 1, 3, 3, DOT)
      elseif pellet[r][c] then
        disc(s, x + TILE // 2, y + TILE // 2, 4, DOT)
      end
    end
  end

  -- Pac-Man: a disc with a wedge taken out of it, and the wedge is a
  -- triangle in the background colour. That primitive exists because of the
  -- 3D renderer, and this is the second thing to want it.
  disc(s, pac.x, pac.y, TILE // 2 - 1, PAC)

  local d = DIRS[pac.dir]
  local reach = TILE
  local open_ = 5

  s:triangle(pac.x, pac.y,
             pac.x + d[1] * reach - d[2] * open_,
             pac.y + d[2] * reach - d[1] * open_,
             pac.x + d[1] * reach + d[2] * open_,
             pac.y + d[2] * reach + d[1] * open_,
             BG)

  for _, g in ipairs(ghosts) do
    disc(s, g.x, g.y - 1, TILE // 2 - 2, g.colour)
    s:fill(g.x - TILE // 2 + 2, g.y - 1, TILE - 4, TILE // 2 - 1, g.colour)

    -- Eyes, looking the way it is going.
    local e = DIRS[g.dir]
    s:fill(g.x - 4 + e[1], g.y - 3 + e[2], 3, 3, 0xffffffff)
    s:fill(g.x + 1 + e[1], g.y - 3 + e[2], 3, 3, 0xffffffff)
  end

  local bar = ROWS * TILE
  s:fill(0, bar, W, BAR, BG)
  s:text(6, bar + 4, ("score %d"):format(score), TEXT, BG)
  s:text(W - 90, bar + 4, ("lives %d"):format(lives), TEXT, BG)

  if over then
    s:text(W // 2 - 36, bar // 2, won and "you win" or "game over", TEXT, BG)
  end
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

local hz = fs.read("/dev/cpu").counter_hz
local escape = 0

local function turn(dir) pac.want = dir end

while win.running do
  if not over then
    step(pac)

    local r, c = tile_of(pac)

    if dot[r][c] or pellet[r][c] then
      score = score + (pellet[r][c] and 50 or 10)
      dot[r][c], pellet[r][c] = false, false
      dots_left = dots_left - 1

      if dots_left == 0 then over, won = true, true end
    end

    for n, g in ipairs(ghosts) do
      chase(g, n)
      step(g)

      local gr, gc = tile_of(g)

      if gr == r and gc == c then
        lives = lives - 1

        if lives <= 0 then
          over = true
        else
          pac.x, pac.y = centre(start_pac.r, start_pac.c)
          pac.dir, pac.want = "left", "left"

          for i, home in ipairs(ghost_home) do
            ghosts[i].x, ghosts[i].y = centre(home.r, home.c)
            ghosts[i].dir = "up"
          end
        end

        break
      end
    end
  end

  draw(win:surface())

  if not win:commit{ x = 0, y = 0, w = W, h = H } then break end

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
          dots_left = 0

          for rr = 1, ROWS do
            for cc = 1, COLS do
              local m = MAZE[rr]:sub(cc, cc)
              dot[rr][cc]    = (m == ".")
              pellet[rr][cc] = (m == "o")
              if m == "." or m == "o" then dots_left = dots_left + 1 end
            end
          end

          pac.x, pac.y = centre(start_pac.r, start_pac.c)

          for i, home in ipairs(ghost_home) do
            ghosts[i].x, ghosts[i].y = centre(home.r, home.c)
          end
        end
      end
    end
  end
end
