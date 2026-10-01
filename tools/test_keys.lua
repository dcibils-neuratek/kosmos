-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The kit's keys, on this machine: every sequence the board makes read back
-- as the key and the modifiers it was made from, nothing typed that was not
-- a character, and a key's number taken apart the way it was put together.
--
--   build/host/lua tools/test_keys.lua
--
-- The sequences are the ones `tests/tests.c`'s "a key with its modifiers is
-- one sequence" holds the board to making, so the two halves of the key
-- path are checked against the same strings.

local keys = assert(loadfile("user/lib/keys.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- Every key a run of bytes comes out as, through one decoder.
local function decode(bytes)
  local d = keys.decoder()
  local out = {}

  for i = 1, #bytes do
    local a, b = d(bytes:byte(i))

    if a then out[#out + 1] = a end
    if b then out[#out + 1] = b end
  end

  return out
end

local function show(list)
  local parts = {}

  for _, c in ipairs(list) do parts[#parts + 1] = tostring(c) end

  return "{" .. table.concat(parts, ", ") .. "}"
end

local function same(a, b)
  if #a ~= #b then return false end

  for i = 1, #a do
    if a[i] ~= b[i] then return false end
  end

  return true
end

local S, A, C = keys.SHIFT, keys.ALT, keys.CTRL
local F = keys.F

-- What the board sends for each key (`hal/keys.c`), and what it must read as.
local SEQUENCES = {
  { "\27[A",      keys.UP },
  { "\27[1;2A",   keys.with(keys.UP, S) },
  { "\27[1;5D",   keys.with(keys.LEFT, C) },
  { "\27[1;6C",   keys.with(keys.RIGHT, S | C) },
  { "\27[1;3B",   keys.with(keys.DOWN, A) },
  { "\27[H",      keys.HOME },
  { "\27[1;2F",   keys.with(keys.END, S) },
  { "\27[5~",     keys.PAGEUP },
  { "\27[6;2~",   keys.with(keys.PAGEDOWN, S) },
  { "\27[3;5~",   keys.with(keys.DELETE, C) },
  { "\27[2~",     keys.INSERT },
  { "\27OP",      F[1] },
  { "\27[1;2S",   keys.with(F[4], S) },
  { "\27[15~",    F[5] },
  { "\27[15;2~",  keys.with(F[5], S) },
  { "\27[17~",    F[6] },
  { "\27[18~",    F[7] },
  { "\27[21~",    F[10] },
  { "\27[23~",    F[11] },
  { "\27[24;6~",  keys.with(F[12], S | C) },
  { "\27[Z",      keys.with(keys.TAB, S) },
  { "\27[9;5u",   keys.with(keys.TAB, C) },
  { "\27[32;5u",  keys.with(keys.SPACE, C) },
  { "\27[13;5u",  keys.with(keys.ENTER, C) },
  { "\27[47;5u",  keys.with(47, C) },
  { "\27[96;6u",  keys.with(96, S | C) },
  -- Other terminals' Home and End, and the application-mode arrows.
  { "\27[1~",     keys.HOME },
  { "\27[4~",     keys.END },
  { "\27OA",      keys.UP },
}

for _, t in ipairs(SEQUENCES) do
  local got = decode(t[1])

  check(same(got, { t[2] }),
        ("%q read as %s, not %s"):format(t[1], show(got), show({ t[2] })))
end

-- **Nothing typed that was not typed.** Page Up read as three bytes lost
-- its 5 and typed its ~; Shift+Up typed ;2A. Between two letters, each is
-- exactly the key it is.
check(same(decode("a\27[5~b"), { 97, keys.PAGEUP, 98 }),
      "Page Up between two letters typed something: " .. show(decode("a\27[5~b")))
check(same(decode("a\27[1;2Ab"), { 97, keys.with(keys.UP, S), 98 }),
      "Shift+Up between two letters typed something: "
      .. show(decode("a\27[1;2Ab")))

-- A sequence this does not know is dropped, whole.
check(same(decode("a\27[99;5zb"), { 97, 98 }),
      "an unknown sequence was not dropped whole: " .. show(decode("a\27[99;5zb")))
check(same(decode("a\27[99~b"), { 97, 98 }),
      "an unknown number was not dropped whole: " .. show(decode("a\27[99~b")))

-- Super's, which the window manager hands on when it has no binding: the
-- character held with Super, never the character - even with a digit as its
-- last byte, and with Shift already in it (`roadmap.md` 6zz d2). Super
-- tapped alone is the manager's, and reaches nobody.
local SUPER = keys.SUPER

check(same(decode("a\27[1;9tb"), { 97, keys.with(116, SUPER), 98 }),
      "Super+T did not reach a window as Super+T: " .. show(decode("a\27[1;9tb")))
check(same(decode("a\27[1;91b"), { 97, keys.with(49, SUPER), 98 }),
      "Super+1 did not reach a window, or ate the key after it: "
      .. show(decode("a\27[1;91b")))
check(same(decode("a\27[1;9}b"), { 97, keys.with(125, SUPER), 98 }),
      "Super+Shift+] is not Super+}: " .. show(decode("a\27[1;9}b")))
check(same(decode("a\27[1;9~b"), { 97, 98 }),
      "Super tapped alone reached a window: " .. show(decode("a\27[1;9~b")))
local super_t = decode("\27[1;9t")[1]

check(super_t and super_t < 0 and select(2, keys.parts(super_t)) == SUPER,
      "Super+T is a number a widget could type, or not Super when taken apart")

-- Escape, alone and doubled, and an Escape that starts nothing: the key
-- itself and the byte after it, both, in order.
check(same(decode("\27x"), { 27, 120 }), "Escape then x: " .. show(decode("\27x")))
check(same(decode("\27\27[A"), { 27, keys.UP }),
      "Escape then Up: " .. show(decode("\27\27[A")))
check(same(decode("\27[\27[B"), { keys.DOWN }),
      "a sequence cut short by another: " .. show(decode("\27[\27[B")))

-- **A modified key is never a character.** Every one is negative, so a
-- widget that types what it is given types none of them.
for _, t in ipairs(SEQUENCES) do
  local c = t[2]
  local key, mods = keys.parts(c)

  if mods > 0 then
    check(c < 0, ("%q is %d, which a widget could type"):format(t[1], c))
  end
end

-- **Taken apart as it was put together**, for every key and every
-- modifier, and an unmodified key is the number it always was.
local round = true

for key = -64, 255 do
  for mods = 0, 15 do
    local k, m = keys.parts(keys.with(key, mods))

    if k ~= key or m ~= mods then round = false end
  end
end

check(round, "keys.parts did not undo keys.with for every key and modifier")
check(keys.with(keys.UP, 0) == -1 and keys.UP == -1 and keys.LEFT == -4,
      "an unmodified arrow is not the number it has always been")

if failed > 0 then
  print(("FAIL: %d of %d checks on the keys"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the keys (%d sequences the board makes, each read "
       .. "back as its key and modifiers; nothing typed that was not; unknown "
       .. "dropped whole; Super's held with Super, its tap the manager's; every "
       .. "key's number taken apart as it was made)"):format(passed, #SEQUENCES))
