-- kosmos: application
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

local KEY_W, KEY_H, GAP = 60, 38, 6
local W = GAP + 4 * (KEY_W + GAP)
local H = 96 + 5 * (KEY_H + GAP)

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

local display = ui.label{ x = GAP, y = 12, w = W - GAP * 2, text = "0" }

local function refresh()
  -- Right-aligned by padding, because the kit has no alignment and a
  -- calculator that left-aligns its number is one nobody trusts.
  local room = (W - GAP * 2) // gfx.font.w
  display.text = (" "):rep(math.max(0, room - #shown)) .. shown
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

local LAYOUT = {
  { "7", "8", "9", "/" },
  { "4", "5", "6", "*" },
  { "1", "2", "3", "-" },
  { "0", ".", "=", "+" },
  { "C" },
}

local function press(label)
  if label:match("^[0-9.]$") then digit(label)
  elseif label == "C" then clear()
  elseif label == "=" then apply()
  else operator(label) end

  refresh()
end

for row, keys in ipairs(LAYOUT) do
  for col, label in ipairs(keys) do
    win:add(ui.button{
      x = GAP + (col - 1) * (KEY_W + GAP),
      y = 52 + (row - 1) * (KEY_H + GAP),
      w = (label == "C") and (KEY_W * 2 + GAP) or KEY_W,
      h = KEY_H,
      text = label,
      on_click = function() press(label) end,
    })
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
