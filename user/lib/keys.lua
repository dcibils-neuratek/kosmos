-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Keys, from the bytes a window is sent.
--
-- The board turns a key that is not a character into the escape sequence a
-- terminal would have sent (`hal/keys.c`), with Shift and Control inside it
-- where they were held - xterm's shapes, `ESC [ 1 ; 2 A` for Shift+Up. The
-- window manager forwards the bytes; this turns them back into one number
-- per key, which is what a widget's `key` is handed.
--
-- **One number, and what it is made of.** A character is itself, 0 to 255.
-- A key that is not one is a small negative number: the arrows -1 to -4,
-- which is what they have always been, and Home, End, the page keys, Insert,
-- Delete and the function keys after them. A key held with Shift, Alt or
-- Control is that number less 1024 for each step of the modifiers - so it
-- is always negative, and no widget that types what it is given can take
-- Ctrl+/ for a slash. `keys.parts` takes one apart and `keys.with` puts one
-- together; nothing else should do the arithmetic.
--
-- **Every sequence is read whole**, parameters and all, and one this does
-- not know is dropped rather than typed. The reader this replaced took a
-- sequence to be three bytes, so `ESC [ 5 ~` - Page Up - lost its `5` and
-- typed its `~` into whatever had the keyboard (`roadmap.md` 6n, step 0).
--
-- Pure: no `sys`, no `gfx`, so `tools/test_keys.lua` runs it on the Mac.

local keys = {}

keys.UP, keys.DOWN, keys.RIGHT, keys.LEFT = -1, -2, -3, -4
keys.HOME, keys.END = -5, -6
keys.PAGEUP, keys.PAGEDOWN = -7, -8
keys.INSERT, keys.DELETE = -9, -10

-- F1 is -11 and F12 is -22: `keys.F[5]` is F5.
keys.F = {}
for n = 1, 12 do keys.F[n] = -10 - n end

-- xterm's modifier bits, the number in a sequence less one - and Super's,
-- which is 9 in `ESC [ 1 ; 9 x` and so 8 here.
keys.SHIFT, keys.ALT, keys.CTRL, keys.SUPER = 1, 2, 4, 8

keys.TAB, keys.ENTER, keys.ESCAPE, keys.SPACE = 9, 13, 27, 32
keys.BACKSPACE = 8

local STEP = 1024

-- A key and its modifiers as one number. Modifiers of 0 leave it as it is.
function keys.with(key, mods)
  return key - STEP * (mods or 0)
end

-- A number back into its key and its modifiers.
function keys.parts(c)
  local mods = (255 - c) // STEP

  return c + STEP * mods, mods
end

--
-- The keys a sequence names by its final byte, with or without a first
-- parameter of 1: `ESC [ A` and `ESC [ 1 ; 2 A` are both Up.
--
local BY_FINAL = {
  [65] = keys.UP, [66] = keys.DOWN, [67] = keys.RIGHT, [68] = keys.LEFT,
  [72] = keys.HOME, [70] = keys.END,
  [80] = keys.F[1], [81] = keys.F[2], [82] = keys.F[3], [83] = keys.F[4],
}

-- The keys `ESC [ n ~` names by its number. 16 and 22 are gaps xterm kept
-- from the VT220; 1 and 4, 7 and 8 are other terminals' Home and End.
local BY_NUMBER = {
  [1] = keys.HOME, [2] = keys.INSERT, [3] = keys.DELETE, [4] = keys.END,
  [5] = keys.PAGEUP, [6] = keys.PAGEDOWN, [7] = keys.HOME, [8] = keys.END,
  [11] = keys.F[1], [12] = keys.F[2], [13] = keys.F[3], [14] = keys.F[4],
  [15] = keys.F[5], [17] = keys.F[6], [18] = keys.F[7], [19] = keys.F[8],
  [20] = keys.F[9], [21] = keys.F[10], [23] = keys.F[11], [24] = keys.F[12],
}

-- The numbers in a sequence's parameters, `"15;2"` as 15 and 2.
local function numbers(params)
  local out = {}

  for field in (params .. ";"):gmatch("([^;]*);") do
    out[#out + 1] = tonumber(field) or 0
  end

  return out
end

--
-- What a whole `ESC [ ... final` sequence means, or nil for one that means
-- nothing here - which is dropped, never typed.
--
local function csi(params, final)
  local p = numbers(params)
  local mods = math.max(0, (p[2] or 1) - 1)

  -- Super's own shape is read whole by the decoder below; anything else
  -- carrying it is nothing the board makes.
  if mods >= 8 then return nil end

  if final == 126 then                                        -- ~
    local key = BY_NUMBER[p[1] or 0]

    return key and keys.with(key, mods)
  end

  if final == 117 then                                        -- u
    local c = p[1] or 0

    if c == 10 then c = keys.ENTER end
    if c < 0 or c > 255 then return nil end

    return keys.with(c, mods)
  end

  if final == 90 then                                         -- Z
    return keys.with(keys.TAB, keys.SHIFT | mods)
  end

  local key = BY_FINAL[final]

  if key and (p[1] == nil or p[1] == 0 or p[1] == 1) then
    return keys.with(key, mods)
  end

  return nil
end

--
-- A decoder: give it one byte, get back nought, one or two keys.
--
-- Two, and that is the part worth being careful about. 27 is ambiguous
-- until the byte after it: if that byte starts no sequence then the 27 was a
-- real Escape *and* the byte is a key of its own, and both have to come out.
-- Returning one and remembering the other for next time sounds equivalent
-- and is not - the next call arrives with its own byte, and the remembered
-- one has to displace it. That was the first attempt, and it ate two bytes
-- out of every three: three presses of Down moved a Doom menu once.
--
-- Stateful, because several bytes make one key, so each reader needs its
-- own decoder.
--
-- **Given nil, the batch has ended** (`roadmap.md` 6zz l3): an Escape still
-- held started nothing, and is the key itself. The window manager posts a
-- key's bytes one after another into the window's queue, which the window
-- takes whole, so a sequence's bytes come in the batch its Escape came in -
-- and without this a lone Escape waited for the next key to be pressed: the
-- browser's Escape stopped nothing, and the address field's gave the page
-- its keys back only once another key came.
--
function keys.decoder()
  local state = nil          -- nil, "escape", "csi", "ss3" or "super"
  local params = ""

  local function begin(c)
    if c == 27 then
      state = "escape"
      return nil
    end

    return c
  end

  return function(c)
    if c == nil then
      if state == "escape" then
        state = nil
        return 27
      end

      return nil
    end

    if state == "escape" then
      if c == 91 then                                         -- [
        state, params = "csi", ""
        return nil
      end

      if c == 79 then                                         -- O
        state = "ss3"
        return nil
      end

      -- An Escape that started nothing: it was the key itself, and this
      -- byte is the next one - which may be another Escape.
      state = nil

      if c == 27 then
        state = "escape"
        return 27
      end

      return 27, c
    end

    if state == "ss3" then
      state = nil

      local key = BY_FINAL[c]

      return key
    end

    if state == "super" then
      --
      -- **The character after `ESC [ 1 ; 9`, whatever it is** - Super held
      -- with a digit makes a digit the last byte - held with Super. The
      -- window manager keeps the combinations it has a binding for and hands
      -- on the rest, and a window may answer one: the browser's Super T is
      -- a new tab (`roadmap.md` 6zz d2). It was dropped here, so a binding
      -- the manager did not have reached no window at all.
      --
      -- Shift is already in the character - Super Shift ] is `}` - so the
      -- modifier is Super alone. `~` is Super tapped by itself, the menu,
      -- which is the manager's always.
      --
      state = nil

      if c == 126 then return nil end

      return keys.with(c, keys.SUPER)
    end

    if state == "csi" then
      if c >= 0x30 and c <= 0x3f then                         -- a parameter
        params = params .. string.char(c)

        -- Super's is one shape of fixed length, and its last byte need not
        -- be one that ends a sequence.
        if params == "1;9" then state = "super" end

        -- Past any real sequence's length: nothing here means anything,
        -- and it is not allowed to swallow the keyboard.
        if #params > 16 then state = nil end

        return nil
      end

      if c >= 0x20 and c <= 0x2f then return nil end          -- intermediate

      state = nil

      if c >= 0x40 and c <= 0x7e then
        return csi(params, c)
      end

      -- A control byte where a sequence should end: the sequence was cut
      -- short, and the byte is a key of its own.
      return begin(c)
    end

    return begin(c)
  end
end

return keys
