-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Calculator
-- A calculator.
--
--   wm calc
--
--   click the keys, or type: 0-9 . + - * / = Enter, c to clear,
--   Backspace to rub out a digit
--
-- Every operating system has one, and it is a better test of a widget kit
-- than it looks: sixteen buttons in a grid that has to line up, a display
-- that is right-aligned, and a state machine that is wrong in every naive
-- version.
--
-- **The keypad is content, not an action bar.** `ui.md` 16.10 puts action
-- buttons across the top of a window, and these are not action buttons -
-- they are the thing the window is for, the way a list is what Tracker is
-- for. The rule is about verbs that act on what the window shows.

local ui    = use("/lib/ui.lua")
local theme = ui.theme

--
-- **As `docs/apps.html` draws it** (`roadmap.md` 5zp): no header at all -
-- a calculator is a canvas, and the display is its top - then the keys in
-- a grid 14 in from the edges and 7 apart, 44 tall with a radius of 8. The
-- operators on the sidebar's grey, `=` in the accent, 0 two keys wide.
--
-- It was 60 by 38 keys 6 apart under a display that was a label of the
-- interface's size, padded with spaces to push the number right.
--
local DISPLAY = 92                 -- 18 + a line of 18 + the number + 14
local PAD, GAP, KEY_H = 14, 7, 44
local W = 300
local KEY_W = (W - 2 * PAD - 3 * GAP) // 4
local H = DISPLAY + PAD + 5 * KEY_H + 4 * GAP + PAD

-- The number at the drawing's 34, which is 44 in this rasterizer's pixels
-- (`theme.lua`: sizes here are 1.30 times CSS's). The key faces at its 16.
local NUMBER_PX, KEY_PX = 44, 21

local win, err = ui.window{ title = "Calculator", w = W, h = H,
                            x = 240, y = 140 }

if not win then
  print("calc: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The state machine.
--
-- Three pieces and no more: what is on the display, the number waiting for
-- an operator, and which operator is pending. Everything a calculator does
-- wrong comes from confusing "the display holds a number I am typing" with
-- "the display holds a result", so that distinction is a variable rather
-- than something inferred.
--------------------------------------------------------------------------

local shown    = "0"
local pending  = nil      -- the operator waiting for a right-hand side
local left     = nil      -- what it is waiting to be applied to
local typing   = false    -- is `shown` something being entered?

local display                      -- the view, made once `present` exists

local function refresh()
  win.dirty = true
end

local function as_number(s)
  return tonumber(s) or 0
end

-- Trailing zeroes off, and an integer shown as one.
local function present(v)
  if v ~= v then return "not a number" end          -- 0/0
  if v == math.huge or v == -math.huge then return "infinity" end

  if math.type(v) == "float" and v == math.floor(v)
     and math.abs(v) < 1e15 then
    return ("%d"):format(v)
  end

  return (("%.10g"):format(v))
end

local function apply()
  if not pending or not left then return end

  local right = as_number(shown)
  local v

  if pending == "+" then v = left + right
  elseif pending == "-" then v = left - right
  elseif pending == "*" then v = left * right
  elseif pending == "/" then
    -- Division by zero answers rather than raising. Lua gives infinity for
    -- a float divide and this says so, which is more useful than an error
    -- dialog and is what the machine actually computed.
    v = right == 0 and (left == 0 and (0/0) or (left > 0 and math.huge
                                                or -math.huge))
        or left / right
  end

  shown = present(v)
  left, pending, typing = nil, nil, false
end

local function digit(d)
  if not typing then
    shown, typing = "", true
  end

  if d == "." and shown:find("%.") then return end
  if shown == "0" and d ~= "." then shown = "" end

  shown = shown .. d
  if shown == "." then shown = "0." end
end

local function operator(op)
  if pending and typing then apply() end

  left    = as_number(shown)
  pending = op
  typing  = false
end

local function clear()
  shown, pending, left, typing = "0", nil, nil, false
end

local function rub()
  if not typing then return end

  shown = shown:sub(1, -2)
  if shown == "" or shown == "-" then shown, typing = "0", false end
end

--------------------------------------------------------------------------
-- The keypad.
--------------------------------------------------------------------------

--
-- The display: what is waiting, dim, above the number - "12 ×" while the
-- right-hand side is typed - and the number right-aligned under it, as
-- large as the drawing makes it. Measured, where it was padded with spaces
-- because the kit had no alignment.
--
local SIGN = { ["+"] = "+", ["-"] = "−", ["*"] = "×", ["/"] = "÷" }

display = ui.view{ x = 0, y = 0, w = W, h = DISPLAY,
                   follow = { "left", "right", "top" } }

function display:draw(g)
  g:fill(0, 0, self.w, self.h - 1, theme.sunken)
  g:fill(0, self.h - 1, self.w, 1, theme.line_soft)

  if pending and left then
    local was = present(left) .. " " .. (SIGN[pending] or pending)

    g:text(self.w - 20 - gfx.measure(was), 18, was, theme.text_dim)
  end

  local face = ui.sized("ui", NUMBER_PX)

  g:text(self.w - 20 - gfx.measure(shown, face), 36, shown, theme.text,
         nil, "ui", NUMBER_PX)
end

--
-- The keys, in the drawing's order. The first row is C, which clears, the
-- rub-out that takes back the last digit, and divide - three keys that all
-- do something. The drawing had brackets there, which this calculator does
-- not have, and a key that does nothing is the half-built feeling in its
-- smallest form.
--
local LAYOUT = {
  { { "C", 2 }, { "⌫" }, { "/" } },
  { { "7" }, { "8" }, { "9" }, { "*" } },
  { { "4" }, { "5" }, { "6" }, { "-" } },
  { { "1" }, { "2" }, { "3" }, { "+" } },
  { { "0", 2 }, { "." }, { "=" } },
}

local function press(label)
  if label:match("^[0-9.]$") then digit(label)
  elseif label == "C" then clear()
  elseif label == "⌫" then rub()
  elseif label == "=" then apply()
  else operator(label) end

  refresh()
end

--
-- One key: the kit's button would be the interface's size and shape, and a
-- key is neither - it is a square of the grid with its face at the size the
-- drawing gives a key. Pressed, a shade darker.
--
local function key(x, y, w, label)
  local k = ui.view{ x = x, y = y, w = w, h = KEY_H }
  local op = SIGN[label] or label == "⌫" or label == "C"
  local eq = (label == "=")

  function k:draw(g)
    local fill = eq and theme.accent
                 or op and theme.mix(theme.window, theme.line_soft, 330)
                 or theme.sunken

    if self.pressed then fill = theme.lift(fill, -16) end

    g:fill_round(0, 0, self.w, self.h, fill, 8)

    if not eq then
      g:frame_round(0, 0, self.w, self.h, theme.line_soft, 8)
    end

    local word = SIGN[label] or label
    local face = ui.sized("ui", KEY_PX)

    g:text((self.w - gfx.measure(word, face)) // 2,
           (self.h - gfx.height(face)) // 2, word,
           eq and theme.text_on or theme.text, nil, "ui", KEY_PX)
  end

  function k:mouse(action, mx, my)
    local inside = mx >= 0 and mx < self.w and my >= 0 and my < self.h

    if action == "press" then
      self.pressed = true
    elseif action == "move" then
      self.pressed = inside
    elseif action == "release" then
      if self.pressed and inside then press(label) end
      self.pressed = false
    end

    return true
  end

  return k
end

for row, keys in ipairs(LAYOUT) do
  local x = PAD

  for _, entry in ipairs(keys) do
    local span = entry[2] or 1
    local w = span * KEY_W + (span - 1) * GAP

    win:add(key(x, DISPLAY + PAD + (row - 1) * (KEY_H + GAP), w, entry[1]))
    x = x + w + GAP
  end
end

win:add(display)

-- The keyboard, at the window rather than at a widget: no key here belongs
-- to one button, and making the buttons focusable so that Tab moved between
-- them would be a calculator you operate with Tab, which nobody wants.
function win:on_key(c)
  local ch = (c >= 32 and c < 127) and string.char(c) or nil

  if ch and ch:match("^[0-9.]$") then press(ch)
  elseif ch and ch:match("^[-+*/]$") then press(ch)
  elseif ch == "=" or c == 13 or c == 10 then press("=")
  elseif ch == "c" or ch == "C" then press("C")
  elseif c == 8 or c == 127 then rub(); refresh()
  else return false end

  return true
end

refresh()
win:run()
