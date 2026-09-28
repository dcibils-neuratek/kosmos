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

function U.target(surface) S = surface end

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
-- `clip` is cut to it, which is what LÖVE's scissor did for a clip's name.
function U.text(s, x, y, c, f, w, align, clip)
  f = f or U.fM
  if clip then
    while #s > 1 and f:getWidth(s) > clip do s = s:sub(1, -2) end
  end
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
  elseif kind == "rec" then U.circle(cx, cy, s) end
end
U.icon = icon

function U.button(x, y, w, h, label, o)
  o = o or {}
  local hot = U.hit(x, y, w, h) and not U.active
  local bg = o.on and (o.color or C.accent) or (hot and C.panel3 or (o.bg or C.panel2))
  U.rect(x, y, w, h, bg, o.r or 3)
  local tc = o.on and C.dark or (o.tc or C.text)
  if o.glyph then U.col(o.on and C.dark or (o.ic or C.text)); icon(o.glyph, x + w / 2, y + h / 2, o.is or 5)
  elseif label then
    local f = o.font or U.fS
    U.text(label, x, y + (h - f:getHeight()) / 2, tc, f, w, "center")
  end
  if hot and U.pressed then U.pressed = false; return true end
  return false
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
