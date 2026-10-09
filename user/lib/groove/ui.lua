-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Groove's widgets (`roadmap.md` 6zh): PulseMusic's tiny immediate-mode kit
-- - buttons, knobs, faders, meters, number boxes - drawn on a surface.
--
-- **The same widgets, called the same way.** Each frame the window draws
-- everything, and a widget both draws itself and answers whether it was
-- used, from `U.mx`, `U.pressed` and the rest, which the window sets from
-- its events before the frame. That is PulseMusic's shape, and keeping it is
-- what lets `app.lua` stay PulseMusic's `app.lua`.
--
-- **What changed is only where the pixels go**: LÖVE's `love.graphics`
-- becomes the gfx kit's surface - a rectangle with rounded corners is
-- `fill_round`, a knob's ring is `arc`, its pointer is `line`, a colour is
-- a number rather than a current colour - and PulseMusic's three fonts are
-- the theme's face at its three sizes.

local ui = use("/Kosmos/Libraries/ui.lua")

local U = { mx = 0, my = 0, down = false, pressed = false, rpressed = false, released = false, wheel = 0,
  active = nil, dbl = false, shift = false, alt = false, ctrl = false }
local log10 = math.log10 or function(x) return math.log(x) / math.log(10) end
local floor, min, max, pi, cos, sin = math.floor, math.min, math.max, math.pi, math.cos, math.sin

U.C = {
  bg = { 0.085, 0.09, 0.105 }, panel = { 0.13, 0.135, 0.155 }, panel2 = { 0.18, 0.185, 0.21 }, panel3 = { 0.25, 0.255, 0.29 },
  line = { 0.27, 0.275, 0.31 }, text = { 0.88, 0.89, 0.92 }, dim = { 0.5, 0.51, 0.56 }, dark = { 0.06, 0.06, 0.07 },
  accent = { 1.0, 0.56, 0.2 }, play = { 0.35, 0.92, 0.55 }, rec = { 1.0, 0.3, 0.32 }, blue = { 0.3, 0.75, 1.0 },
}
U.trackColors = { { 1, .42, .42 }, { 1, .62, .26 }, { .98, .82, .3 }, { .5, .85, .4 }, { .3, .82, .78 }, { .35, .62, 1 },
  { .66, .52, 1 }, { .95, .5, .8 } }
local C = U.C

-- The surface this frame is drawn into, set by the window each frame: a
-- direct window's buffers flip, so it is never kept from one to the next.
local S

---------------------------------------------------------------- what changed
-- **Only what changed is drawn** (`roadmap.md` 6zh). Diego on the M700:
-- "the app feels laggy", "Are we redrawing the entire ui every frame for
-- groove?", "Or just redraw what's dirty?" PulseMusic's UI is immediate
-- mode - every frame draws everything - and under QEMU a frame was 72 ms of
-- drawing at 1920x1080 and 150 at 3432x1406: the pixels.
--
-- So PulseMusic's code is left as it is and the surface it draws on is not
-- the window's: `S` is a recorder. Each call - eight kinds, the Graphics
-- Kit's `fill` to `arc` - is kept as numbers with the rectangle it can
-- touch, and at the end of the frame (`U.flush`) the calls are compared,
-- one by one in order, with those last drawn into the same buffer. Where
-- one differs, its old and new rectangles are dirty; the dirty places are
-- gathered into tiles and the tiles into rectangles, and every call that
-- touches one is drawn again into a *view* of it - the kit's surface over
-- that rectangle, which clips by being no bigger. A playing song changes
-- its playheads, meters and counter, so that is what is drawn.
--
-- **The same buffer, not the last frame**: a direct window has two, and
-- the one drawn into now holds the frame before last. So each buffer keeps
-- the calls last drawn into it, and the first frame into each is drawn
-- whole. A call's rectangle is generous - text by its measured width and
-- the face's height, lines and arcs by their width - because a rectangle
-- that is too small leaves yesterday's pixels behind, and one too large
-- only draws a little more.
local REAL                                   -- the buffer this frame is for
local STRIDE = 10                            -- numbers kept a call
local TILE = 32
local lastIn = setmetatable({}, { __mode = "k" })    -- buffer -> its calls
local spare = nil                            -- a list no buffer holds
local L                                      -- this frame's calls
local heights = setmetatable({}, { __mode = "k" })   -- face -> height

local function newList()
  return { n = 0, op = {}, a = {}, s = {}, f = {}, x0 = {}, y0 = {}, x1 = {}, y1 = {} }
end

-- A call: its kind, its numbers, a string and a face for text, and the
-- rectangle it may touch.
local function rec(op, x0, y0, x1, y1, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, str, face)
  local i = L.n + 1
  L.n = i
  L.op[i] = op
  local k, A = (i - 1) * STRIDE, L.a
  A[k + 1], A[k + 2], A[k + 3], A[k + 4], A[k + 5] = a1, a2, a3, a4, a5
  A[k + 6], A[k + 7], A[k + 8], A[k + 9], A[k + 10] = a6, a7, a8, a9, a10
  L.s[i], L.f[i] = str, face
  L.x0[i], L.y0[i], L.x1[i], L.y1[i] = floor(x0) - 1, floor(y0) - 1, floor(x1) + 2, floor(y1) + 2
end

local function bool(v) return v and 1 or 0 end

-- The recorder, with the kit's surface's methods and its arguments.
local R = {}

function R:fill(x, y, w, h, c) rec(1, x, y, x + w, y + h, x, y, w, h, c) end
function R:fill_round(x, y, w, h, c, r) rec(2, x, y, x + w, y + h, x, y, w, h, c, r) end
function R:frame_round(x, y, w, h, c, r) rec(3, x, y, x + w, y + h, x, y, w, h, c, r) end

function R:line(x0, y0, x1, y1, width, c)
  local e = (width or 1) / 2 + 1
  rec(4, min(x0, x1) - e, min(y0, y1) - e, max(x0, x1) + e, max(y0, y1) + e,
      x0, y0, x1, y1, width, c)
end

function R:disc(x, y, r, c, filled)
  rec(5, x - r - 1, y - r - 1, x + r + 1, y + r + 1, x, y, r, c, bool(filled))
end

function R:text(x, y, s, c, _, face)
  local h = heights[face]
  if not h then h = gfx.height(face); heights[face] = h end
  rec(6, x - 2, y - 2, x + gfx.measure(s, face) + 2, y + h + 2, x, y, c, nil, nil,
      nil, nil, nil, nil, nil, s, face)
end

function R:triangle(ax, ay, bx, by, cx, cy, c)
  rec(7, min(ax, bx, cx), min(ay, by, cy), max(ax, bx, cx), max(ay, by, cy),
      ax, ay, bx, by, cx, cy, c)
end

function R:arc(x, y, r, width, ax, ay, bx, by, big, c)
  local e = r + width + 1
  rec(8, x - e, y - e, x + e, y + e, x, y, r, width, ax, ay, bx, by, bool(big), c)
end

-- Call `i` of list `C` drawn on `t`, moved by (dx, dy) - into a view whose
-- corner is where (-dx, -dy) was.
local function play(t, C, i, dx, dy)
  local o, k, A = C.op[i], (i - 1) * STRIDE, C.a
  if o == 1 then t:fill(A[k + 1] + dx, A[k + 2] + dy, A[k + 3], A[k + 4], A[k + 5])
  elseif o == 2 then t:fill_round(A[k + 1] + dx, A[k + 2] + dy, A[k + 3], A[k + 4], A[k + 5], A[k + 6])
  elseif o == 3 then t:frame_round(A[k + 1] + dx, A[k + 2] + dy, A[k + 3], A[k + 4], A[k + 5], A[k + 6])
  elseif o == 4 then t:line(A[k + 1] + dx, A[k + 2] + dy, A[k + 3] + dx, A[k + 4] + dy, A[k + 5], A[k + 6])
  elseif o == 5 then t:disc(A[k + 1] + dx, A[k + 2] + dy, A[k + 3], A[k + 4], A[k + 5] == 1)
  elseif o == 6 then t:text(A[k + 1] + dx, A[k + 2] + dy, C.s[i], A[k + 3], nil, C.f[i])
  elseif o == 7 then
    t:triangle(A[k + 1] + dx, A[k + 2] + dy, A[k + 3] + dx, A[k + 4] + dy, A[k + 5] + dx, A[k + 6] + dy, A[k + 7])
  elseif o == 8 then
    t:arc(A[k + 1] + dx, A[k + 2] + dy, A[k + 3], A[k + 4], A[k + 5], A[k + 6], A[k + 7], A[k + 8],
          A[k + 9] == 1, A[k + 10])
  end
end

local function same(C, P, i)
  if C.op[i] ~= P.op[i] or C.s[i] ~= P.s[i] or C.f[i] ~= P.f[i] then return false end
  local k, A, B = (i - 1) * STRIDE, C.a, P.a
  for j = k + 1, k + STRIDE do
    if A[j] ~= B[j] then return false end
  end
  return true
end

function U.target(surface)
  REAL = surface
  L = spare or newList()
  spare = nil
  L.n = 0
  S = R
end

-- What the last flush did, for `--report` and the check below.
U.drawn = { frames = 0, whole = 0, px = 0, window = 0 }

-- Every dirty tile, and the rectangles they make: runs along a row of tiles,
-- a run carried down while the rows below repeat it.
local tiles = {}

local function dirty(tw, th, x0, y0, x1, y1)
  local c0, c1 = max(0, x0 // TILE), min(tw - 1, (x1 - 1) // TILE)
  local r0, r1 = max(0, y0 // TILE), min(th - 1, (y1 - 1) // TILE)
  for r = r0, r1 do
    local base = r * tw
    for c = c0, c1 do tiles[base + c] = true end
  end
end

local function rectangles(tw, th, W, H)
  local out, open = {}, {}
  for r = 0, th - 1 do
    local runs, c = {}, 0
    while c < tw do
      if tiles[r * tw + c] then
        local c0 = c
        while c < tw and tiles[r * tw + c] do tiles[r * tw + c] = nil; c = c + 1 end
        runs[#runs + 1] = c0 * 65536 + c
      else
        c = c + 1
      end
    end
    local still = {}
    for _, key in ipairs(runs) do
      local o = open[key]
      if o then o.h = o.h + TILE; still[key] = o
      else
        o = { x = (key // 65536) * TILE, y = r * TILE, w = (key % 65536 - key // 65536) * TILE, h = TILE }
        out[#out + 1] = o
        still[key] = o
      end
    end
    open = still
  end
  for _, o in ipairs(out) do
    o.w = min(o.w, W - o.x)
    o.h = min(o.h, H - o.y)
  end
  return out
end

-- **The frame, onto the window**: drawn whole into a buffer the first time,
-- and after that only where its calls differ from those last drawn into the
-- same buffer. Answers the rectangle to hand over, or nil when nothing
-- changed at all.
function U.flush()
  local W, H = REAL:size()
  local P = lastIn[REAL]
  local d = U.drawn
  local damage

  d.frames, d.window = d.frames + 1, W * H

  if not P or P.w ~= W or P.h ~= H then
    for i = 1, L.n do play(REAL, L, i, 0, 0) end
    d.whole, d.px = d.whole + 1, d.px + W * H
    damage = { x = 0, y = 0, w = W, h = H }
  else
    local tw, th = (W + TILE - 1) // TILE, (H + TILE - 1) // TILE
    local any = false

    for i = 1, max(L.n, P.n) do
      if i > L.n or i > P.n or not same(L, P, i) then
        any = true
        if i <= L.n then dirty(tw, th, L.x0[i], L.y0[i], L.x1[i], L.y1[i]) end
        if i <= P.n then dirty(tw, th, P.x0[i], P.y0[i], P.x1[i], P.y1[i]) end
      end
    end

    if any then
      local x0, y0, x1, y1 = W, H, 0, 0
      for _, r in ipairs(rectangles(tw, th, W, H)) do
        local v = REAL:view(r.x, r.y, r.w, r.h)
        if v then
          for i = 1, L.n do
            if L.x1[i] > r.x and L.x0[i] < r.x + r.w and L.y1[i] > r.y and L.y0[i] < r.y + r.h then
              play(v, L, i, -r.x, -r.y)
            end
          end
        end
        d.px = d.px + r.w * r.h
        x0, y0 = min(x0, r.x), min(y0, r.y)
        x1, y1 = max(x1, r.x + r.w), max(y1, r.y + r.h)
      end
      damage = { x = x0, y = y0, w = x1 - x0, h = y1 - y0 }
    end
  end

  L.w, L.h = W, H
  spare = P
  lastIn[REAL] = L

  -- `--redraw-check`: the frame drawn whole as well, into a surface of its
  -- own, and the pixels that differ counted - which has to be none.
  if U.checking then
    local ref = U.checkSurface
    if not ref or select(1, ref:size()) ~= W or select(2, ref:size()) ~= H then
      ref = gfx.surface{ w = W, h = H }
      U.checkSurface = ref
    end
    for i = 1, L.n do play(ref, L, i, 0, 0) end
    local wrong = REAL:differs(ref) or -1
    U.checked = (U.checked or 0) + 1
    U.wrong = (U.wrong or 0) + wrong
  end

  return damage
end

---------------------------------------------------------------- colour
-- PulseMusic's colours are LÖVE's, three numbers from 0 to 1; a surface
-- takes one number, ARGB.
local function byte(v)
  v = floor(v * 255 + 0.5)
  return v < 0 and 0 or (v > 255 and 255 or v)
end

local function argb(c, a)
  return (byte(a or 1) << 24) | (byte(c[1]) << 16) | (byte(c[2]) << 8) | byte(c[3])
end

U.argb = argb

-- LÖVE's current colour, for what draws with it: an icon, a dot, a line.
local cur = 0xffffffff

function U.col(c, a) cur = argb(c, a) end

---------------------------------------------------------------- fonts
-- A font as PulseMusic uses one: a height, and a width for a string.
local function font(px)
  local face = ui.sized("ui", px)
  return { face = face, px = px, h = gfx.height(face),
           getHeight = function(self) return self.h end,
           getWidth = function(self, s) return gfx.measure(s, self.face) end }
end

function U.load()
  U.fS, U.fM, U.fL = font(11), font(13), font(20)
end

---------------------------------------------------------------- drawing
function U.hit(x, y, w, h) return U.mx >= x and U.mx < x + w and U.my >= y and U.my < y + h end

-- LÖVE's rectangles fall between pixels; these are rounded to them, edge
-- by edge, so cells laid side by side meet without a gap or an overlap.
function U.rect(x, y, w, h, c, r, a)
  local x0, y0 = floor(x + 0.5), floor(y + 0.5)
  local ww, hh = floor(x + w + 0.5) - x0, floor(y + h + 0.5) - y0
  if ww <= 0 or hh <= 0 then return end
  r, a = r or 3, a or 1
  if r == 0 and a >= 1 then S:fill(x0, y0, ww, hh, argb(c))
  else S:fill_round(x0, y0, ww, hh, argb(c, a), r) end
end

-- An outline, one pixel, in the current colour.
function U.frame(x, y, w, h, r)
  S:frame_round(floor(x + 0.5), floor(y + 0.5), floor(w + 0.5), floor(h + 0.5), cur, r or 0)
end

function U.line(x0, y0, x1, y1, width)
  S:line(floor(x0 + 0.5), floor(y0 + 0.5), floor(x1 + 0.5), floor(y1 + 0.5), width or 1, cur)
end

-- A straight line across or down, one pixel, in a colour with its alpha:
-- the grid lines, which are most of the lines there are.
function U.vline(x, y0, y1, c, a) U.rect(x, y0, 1, y1 - y0, c, 0, a) end
function U.hline(x0, x1, y, c, a) U.rect(x0, y, x1 - x0, 1, c, 0, a) end

function U.circle(x, y, r) S:disc(floor(x + 0.5), floor(y + 0.5), floor(r + 0.5), cur, true) end

-- `printf`'s `w` and `align` place one line in a box; a line longer than
-- `clip` is fitted to it (`ui.fitted`, with "..."), where LÖVE's scissor
-- cut a clip's name - and this cut it a byte at a time, inside a character.
function U.text(s, x, y, c, f, w, align, clip)
  f = f or U.fM
  if clip then s = ui.fitted(s, clip, f.face) end
  if w then
    local tw = f:getWidth(s)
    if align == "right" then x = x + w - tw
    elseif align ~= "left" then x = x + (w - tw) / 2 end
  end
  S:text(floor(x), floor(y), s, argb(c or C.text), nil, f.face)
end

function U.begin()
end

function U.finish()
  U.pressed, U.rpressed, U.released, U.wheel, U.dbl = false, false, false, 0, false
  if not U.down then U.active = nil end
end

local function icon(kind, cx, cy, s)
  if kind == "play" then S:triangle(cx - s * 0.7, cy - s, cx - s * 0.7, cy + s, cx + s, cy, cur)
  elseif kind == "stop" then
    local d = floor(s * 1.6 + 0.5)
    S:fill(floor(cx - d / 2 + 0.5), floor(cy - d / 2 + 0.5), d, d, cur)
  elseif kind == "rec" then U.circle(cx, cy, s)
  elseif kind == "more" then          -- three dots, stood up: the window's menu
    for i = -1, 1 do U.circle(cx, cy + i * s * 1.3, s * 0.34) end
  end
end
U.icon = icon

-- **A click is a press and a release over the same thing** (Diego, 9 October
-- 2026), as Dear ImGui has it: the press makes the thing it was on the active
-- one - "button", and where it is, since a button here has no id but its
-- place - and the release answers true only over the thing that was pressed.
-- Let go anywhere else and nothing happens. A knob, a fader and a number are
-- drags and keep the press; so does a drum pad, which sounds when struck.
local function held(x, y, w, h)
  return U.active == "button" and U.bx == x and U.by == y and U.bw == w and U.bh == h
end

-- The press, taken and held on (x, y, w, h).
function U.hold(x, y, w, h)
  U.active, U.bx, U.by, U.bw, U.bh = "button", x, y, w, h
  U.pressed = false
end

-- The release over (x, y, w, h), when that is where the press was held.
function U.clicked(x, y, w, h)
  if U.released and U.hit(x, y, w, h) and held(x, y, w, h) then U.bx = nil; return true end
  return false
end

-- Both: a press there is held, and true on the release over it.
function U.click(x, y, w, h)
  if U.pressed and not U.active and U.hit(x, y, w, h) then U.hold(x, y, w, h) end
  return U.clicked(x, y, w, h)
end

-- `o.press` for the few that act on the press: a pad that sounds, a menu that opens.
function U.button(x, y, w, h, label, o)
  o = o or {}
  local over, fired = U.hit(x, y, w, h), false
  if o.press then
    fired = over and U.pressed and not U.active
    if fired then U.pressed = false end
  else
    fired = U.click(x, y, w, h)
  end
  local down = over and held(x, y, w, h)      -- pressed, and the pointer still on it
  local hot = over and (not U.active or down)
  local bg = o.on and (o.color or C.accent) or (down and C.panel or (hot and C.panel3 or (o.bg or C.panel2)))
  U.rect(x, y, w, h, bg, o.r or 3, (o.on and down) and 0.75 or nil)
  local tc = o.on and C.dark or (o.tc or C.text)
  if o.glyph then U.col(o.on and C.dark or (o.ic or C.text)); icon(o.glyph, x + w / 2, y + h / 2, o.is or 5)
  elseif label then
    local f = o.font or U.fS
    U.text(label, x, y + (h - f:getHeight()) / 2, tc, f, w, "center")
  end
  return fired
end

---------------------------------------------------------------- value mapping
function U.toNorm(s, v)
  if s.exp then return math.log(v / s.min) / math.log(s.max / s.min) end
  return (v - s.min) / (s.max - s.min)
end

function U.fromNorm(s, n)
  n = max(0, min(1, n))
  local v
  if s.exp then v = s.min * (s.max / s.min) ^ n else v = s.min + n * (s.max - s.min) end
  if s.choices or s.int then v = floor(v + 0.5) end
  return v
end

function U.format(s, v)
  if s.choices then return s.choices[v] or "?" end
  local f = s.fmt
  if s.int then return string.format("%+d", floor(v))
  elseif f == "hz" then return v >= 1000 and string.format("%.1fk", v / 1000) or string.format("%.0f", v)
  elseif f == "hzf" then return string.format("%.2f", v)
  elseif f == "s" then return v < 1 and string.format("%.0fms", v * 1000) or string.format("%.1fs", v)
  elseif f == "st" then return string.format("%+.0f", v)
  elseif f == "ct" then return string.format("%.0fct", v)
  elseif f == "x" then return string.format("x%.2f", v)
  elseif f == "pan" then
    if math.abs(v) < 0.02 then return "C" end
    return (v < 0 and "L" or "R") .. floor(math.abs(v) * 50 + 0.5)
  end
  return string.format("%.0f", v * 100)
end

---------------------------------------------------------------- knob
-- An arc from angle `a` to angle `b`, clockwise on the screen as LÖVE's are
-- with y downward: the kit is handed the two directions and whether the arc
-- is more than half a turn.
local function arc(cx, cy, r, width, a, b)
  if b < a then a, b = b, a end
  S:arc(floor(cx + 0.5), floor(cy + 0.5), floor(r + 0.5), width,
        floor(cos(a) * 4096), floor(sin(a) * 4096), floor(cos(b) * 4096), floor(sin(b) * 4096),
        b - a > pi, cur)
end

-- returns new value when changed
function U.knob(id, x, y, w, h, s, v, color)
  local cx, r = x + w / 2, max(6, min(16, min(w / 2 - 4, (h - 29) / 2)))
  local cy = y + 13 + r
  local hot = U.hit(x, y, w, h) and not U.active
  local n = U.toNorm(s, v)
  local out
  if hot and U.dbl then out = s.def; U.dbl = false; U.pressed = false
  elseif hot and U.pressed then
    U.active, U.dragY, U.dragN = id, U.my, n; U.pressed = false
  elseif hot and U.wheel ~= 0 then
    local stepN = (s.choices or s.int) and (1 / (s.max - s.min)) or 0.025
    out = U.fromNorm(s, n + U.wheel * stepN); U.wheel = 0
  end
  if U.active == id then
    local nn = U.dragN + (U.dragY - U.my) / (U.shift and 900 or 160)
    out = U.fromNorm(s, nn)
  end
  if out then n = U.toNorm(s, out) end
  local a0, a1 = pi * 0.75, pi * 2.25
  U.text(s.l, x, y, (hot or U.active == id) and C.text or C.dim, U.fS, w, "center")
  U.col(C.line); arc(cx, cy, r, 3, a0, a1)
  local c = color or C.accent
  if s.min < 0 and not s.int then
    local mid = (a0 + a1) / 2
    U.col(c); arc(cx, cy, r, 3, mid, a0 + n * (a1 - a0))
  else
    U.col(c); arc(cx, cy, r, 3, a0, a0 + max(0.001, n) * (a1 - a0))
  end
  local a = a0 + n * (a1 - a0)
  U.col(C.text)
  U.line(cx + cos(a) * r * 0.25, cy + sin(a) * r * 0.25, cx + cos(a) * r * 0.8, cy + sin(a) * r * 0.8, 2)
  U.text(U.format(s, out or v), x, cy + r + 1, C.text, U.fS, w, "center")
  return out
end

---------------------------------------------------------------- draggable number box
function U.dragNum(id, x, y, w, h, v, lo, hi, sens, fmt, o)
  o = o or {}
  local hot = U.hit(x, y, w, h) and not U.active
  local out
  if hot and U.pressed then U.active, U.dragY, U.dragN = id, U.my, v; U.pressed = false
  elseif hot and U.wheel ~= 0 then out = max(lo, min(hi, v + U.wheel * (o.wheelStep or 1))); U.wheel = 0 end
  if U.active == id then
    out = max(lo, min(hi, U.dragN + (U.dragY - U.my) * (U.shift and sens * 0.1 or sens)))
    if o.int then out = floor(out + 0.5) end
  end
  U.rect(x, y, w, h, (hot or U.active == id) and C.panel3 or (o.bg or C.dark), 3)
  local f = o.font or U.fM
  local label = type(fmt) == "function" and fmt(out or v) or string.format(fmt, out or v)
  U.text(label, x, y + (h - f:getHeight()) / 2, o.tc or C.text, f, w, "center")
  return out
end

---------------------------------------------------------------- fader + meter
local function meterN(p)
  if p < 0.00001 then return 0 end
  return max(0, min(1, (20 * log10(p) + 54) / 60))
end

function U.meter(x, y, w, h, p)
  U.rect(x, y, w, h, C.dark, 2)
  local n = meterN(p)
  if n > 0 then
    local c = p > 1 and C.rec or (p > 0.7 and C.accent or C.play)
    U.rect(x, y + h * (1 - n), w, h * n, c, 2)
  end
end

local HANDLE = { 0.75, 0.76, 0.8 }

function U.fader(id, x, y, w, h, v, def, color)
  local hot = U.hit(x - 4, y - 4, w + 8, h + 8) and not U.active
  local out
  if hot and U.dbl then out = def; U.dbl = false; U.pressed = false
  elseif hot and U.pressed then U.active, U.dragY, U.dragN = id, U.my, v; U.pressed = false
  elseif hot and U.wheel ~= 0 then out = max(0, min(1, v + U.wheel * 0.02)); U.wheel = 0 end
  if U.active == id then out = max(0, min(1, U.dragN + (U.dragY - U.my) / (U.shift and h * 6 or h))) end
  local n = out or v
  U.rect(x + w / 2 - 2, y, 4, h, C.dark, 2)
  U.rect(x + w / 2 - 2, y + h * (1 - n), 4, h * n, color or C.accent, 2, 0.7)
  local hy = y + h * (1 - n)
  U.rect(x, hy - 5, w, 10, (hot or U.active == id) and C.text or HANDLE, 2)
  U.rect(x + 2, hy - 1, w - 4, 2, C.dark, 0)
  return out
end

return U
