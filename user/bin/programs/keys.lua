-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs desktop
-- keys: text typed into the window keys go to, as if at the keyboard.
--
--   keys TEXT                    typed now, every key at once
--   keys --to TITLE TEXT         when a window called TITLE is the one keys
--                                go to - waited for, a minute at most
--   keys --after MS TEXT         and that many milliseconds after it
--
-- Each character becomes its key's press and release, Shift held round a
-- capital or a shifted mark (a US keyboard's), `\n` is Return - sent
-- through the window manager's remote door, `/Running/wm/remote`, which is
-- how `vncd` hands it a viewer's keys and why this needs the desktop: it is
-- lent only to a program the desktop starts (`wm a,keys:...`).
--
-- All of them at once is the point as much as the convenience: it is a
-- burst the way a VNC viewer's typing or a paste is, and `testing.md`
-- 18.655 is what such a burst found.

local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local words = use("/Kosmos/Libraries/files.lua").words(args, 99)

local to, after, text = nil, 0, {}
local i = 1

while i <= #words do
  if words[i] == "--to" and words[i + 1] then
    to, i = words[i + 1], i + 2
  elseif words[i] == "--after" and words[i + 1] then
    after, i = math.tointeger(tonumber(words[i + 1])) or 0, i + 2
  else
    text[#text + 1] = words[i]
    i = i + 1
  end
end

text = table.concat(text, " "):gsub("\\n", "\n")

if text == "" then
  print("keys: keys [--to TITLE] [--after MS] TEXT")
  return
end

if not fs.getattr(wmproto.REMOTE) then
  print("keys: not started by the desktop, so it has no keyboard to lend - wm ...,keys:TEXT")
  return
end

-- Linux's key codes, which are the keyboard's own here, for a US layout:
-- the key, and whether Shift is held for the character.
local KEY = {}
local function row(chars, shifted, codes)
  for n = 1, #chars do
    KEY[chars:sub(n, n)] = { codes[n], false }
    if shifted then KEY[shifted:sub(n, n)] = { codes[n], true } end
  end
end

row("1234567890-=", "!@#$%^&*()_+", { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 })
row("qwertyuiop[]", "QWERTYUIOP{}", { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 })
row("asdfghjkl;'`", "ASDFGHJKL:\"~", { 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41 })
row("\\zxcvbnm,./", "|ZXCVBNM<>?", { 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53 })
KEY[" "], KEY["\n"], KEY["\t"] = { 57, false }, { 28, false }, { 15, false }

local SHIFT = 42

-- Waits are in the scheduler's ticks: milliseconds turned into them here.
local tick_hz = (fs.read("/Devices/kernel") or {}).tick_hz or 250
local function wait_ms(ms) sys.sleep(math.max(1, ms * tick_hz // 1000)) end

if to then
  local found = false

  for _ = 1, 1200 do
    local open = wmproto.windows()

    for _, w in ipairs(open and open.windows or {}) do
      if w.title == to and w.focused then found = true end
    end

    if found then break end
    wait_ms(50)
  end

  if not found then
    print(("keys: no window called %s took the keys in a minute"):format(to))
    return
  end
end

if after > 0 then wait_ms(after) end

-- The character with the press, as `vncd` sends a viewer's: a Kosmos
-- window types from it, Linux's window from the key alone.
local function send(code, down, ch)
  fs.send(wmproto.REMOTE, { type = "key", code = code, down = down, chars = ch or "" })
end

local typed, skipped = 0, 0

for ch in text:gmatch(utf8.charpattern) do
  local k = KEY[ch]

  if k then
    if k[2] then send(SHIFT, true) end
    send(k[1], true, ch ~= "\n" and ch ~= "\t" and ch or "")
    send(k[1], false)
    if k[2] then send(SHIFT, false) end
    typed = typed + 1
  else
    skipped = skipped + 1
  end
end

print(("keys: %d typed%s"):format(typed, skipped > 0 and (", %d with no key here"):format(skipped) or ""))
