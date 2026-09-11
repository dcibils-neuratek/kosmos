-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What Lite XL's host decides without asking anybody.
--
-- `/bin/litexl.lua` is the editor's launcher, and most of it is conversation
-- with servers: a window, a namespace, a clipboard. What is here is the part
-- it works out on its own - where a path points, what the installed tree
-- looks like, how a file reads, which key is which, what waits on the queue,
-- how much of the window changed - so that `tools/test_litexl_host.lua` can
-- check it on the build machine, in a second, without booting one.
--
-- **The queue is why that matters.** It consumed events by clearing each
-- slot, and once everything had been taken `#` answered nought while the
-- read position had moved on - so the count of waiting events went below
-- zero and the next events were written where they would never be read. A
-- Control release was among them, and the editor took `l` and `o` for
-- `ctrl+l` and `ctrl+o`. Nothing short of watching it misbehave in a window
-- said so - and a check here would have.

local host = {}

--------------------------------------------------------------------------
-- Paths.
--------------------------------------------------------------------------

-- `p` against the working directory `cwd`, with `.`, `..` and doubled
-- slashes resolved. `..` above the root stays at the root.
function host.absolute(cwd, p)
  p = tostring(p or "")

  if p:sub(1, 1) ~= "/" then p = tostring(cwd or "/") .. "/" .. p end

  local parts = {}

  for part in p:gmatch("[^/]+") do
    if part == ".." then
      parts[#parts] = nil
    elseif part ~= "." then
      parts[#parts + 1] = part
    end
  end

  return "/" .. table.concat(parts, "/")
end

--------------------------------------------------------------------------
-- The installed tree.
--
-- The build stores Lite XL's data flat, under keys like
-- `litexl/plugins/treeview.lua`, and the editor finds its plugins, colours
-- and languages by listing directories. So the directories are worked out
-- from the keys: a name is a file when a key is exactly that, and a
-- directory when keys continue past it.
--------------------------------------------------------------------------

function host.tree(names, prefix)
  local keys = {}

  for _, name in ipairs(names or {}) do
    if name:sub(1, #prefix) == prefix and #name > #prefix then
      keys[#keys + 1] = name:sub(#prefix + 1)
    end
  end

  local tree = {}

  function tree.info(sub)
    if sub == "" then return { type = "dir", size = 0, modified = 0 } end

    local below = sub .. "/"

    for _, k in ipairs(keys) do
      if k == sub then return { type = "file", size = 0, modified = 0 } end

      if k:sub(1, #below) == below then
        return { type = "dir", size = 0, modified = 0 }
      end
    end

    return nil, "no such file: " .. sub
  end

  function tree.list(sub)
    local below = (sub == "") and "" or (sub .. "/")
    local seen, out = {}, {}

    for _, k in ipairs(keys) do
      if k:sub(1, #below) == below then
        local child = k:sub(#below + 1):match("^[^/]+")

        if child and not seen[child] then
          seen[child] = true
          out[#out + 1] = child
        end
      end
    end

    if #out == 0 then return nil, "no such directory: " .. sub end

    table.sort(out)
    return out
  end

  return tree
end

--------------------------------------------------------------------------
-- Files, as Lite XL's `io.open` expects them.
--------------------------------------------------------------------------

-- A text as a file opened for reading, which is what a document is here:
-- read once, top to bottom. The formats are Lua's - `l`, `L`, `a` and a
-- count, with or without the `*` older code writes.
function host.reading(text)
  local at = 1
  local f = {}

  local function one(fmt)
    if type(fmt) == "number" then
      if at > #text then return nil end
      local s = text:sub(at, at + fmt - 1)
      at = at + #s
      return s
    end

    local kind = (tostring(fmt or "l"):gsub("^%*", "")):sub(1, 1)

    if kind == "a" then
      local s = text:sub(at)
      at = #text + 1
      return s
    end

    if at > #text then return nil end

    local e = text:find("\n", at, true)
    local line = text:sub(at, (e or (#text + 1)) - 1)

    if kind == "L" and e then line = line .. "\n" end

    at = (e or #text) + 1
    return line
  end

  function f.read(_, ...)
    local n = select("#", ...)
    if n == 0 then return one("l") end

    local out = {}
    for i = 1, n do out[i] = one((select(i, ...))) end
    return table.unpack(out, 1, n)
  end

  function f.lines(_, fmt)
    return function() return one(fmt or "l") end
  end

  function f.seek(_, whence, offset)
    offset = offset or 0

    if whence == "set" then
      at = offset + 1
    elseif whence == "end" then
      at = #text + 1 + offset
    else
      at = at + offset
    end

    return at - 1
  end

  function f.write() return nil, "opened for reading" end
  function f.close() return true end

  return f
end

-- A file opened for writing. What is written is collected and handed to
-- `store` once, on close, so a document is one write however many lines it
-- has; `initial` is what an append starts from. `store(text)` returns true,
-- or nil and why.
function host.writing(store, initial)
  local parts = { initial }
  local f = {}

  function f:write(...)
    for i = 1, select("#", ...) do
      parts[#parts + 1] = tostring((select(i, ...)))
    end
    return self
  end

  function f:flush() return self end
  function f.seek() return nil, "not seekable here" end
  function f.read() return nil, "opened for writing" end
  function f.lines() return function() return nil end end

  function f.close()
    local ok, err = store(table.concat(parts))
    if not ok then return nil, tostring(err) end
    return true
  end

  return f
end

--------------------------------------------------------------------------
-- The queue of events on their way to Lite XL.
--
-- **Consumed by moving past an entry, never by clearing it.** Cleared slots
-- leave `#` free to answer nought once they are all gone, while `head` has
-- moved on: `pending` goes below zero, a caller waiting for it to reach
-- nought never sees it, and the next push lands below `head`, where nothing
-- reads it. So nothing is ever cleared, and the storage starts again once
-- everything has been taken, which is also how it gives memory back.
--------------------------------------------------------------------------

function host.queue(on_push)
  local items, head = {}, 1
  local q = {}

  function q.push(...)
    local e = table.pack(...)
    items[#items + 1] = e
    if on_push then on_push(e) end
  end

  function q.pending()
    return #items - head + 1
  end

  -- The next event, packed with its count so trailing values survive, or nil.
  function q.pop()
    local e = items[head]
    if not e then return nil end

    head = head + 1

    if head > #items then items, head = {}, 1 end

    return e
  end

  return q
end

--------------------------------------------------------------------------
-- Keys.
--
-- Lite XL binds strokes by the names SDL gives keys, lower-cased - `return`,
-- `pageup`, `left ctrl` - and learns what is held from the presses and
-- releases themselves, so a table from keycode to name is the whole of the
-- translation. The codes are what the window manager passes up undecoded:
-- Linux's `input-event-codes.h`, which virtio-input speaks and which the
-- i8042 driver matches through the typing block and translates to for the
-- extended keys `hal/keys.h` names. The letter rows are laid out as
-- `hal/keys.c`'s `keymap_plain` has them.
--------------------------------------------------------------------------

host.KEY_NAMES = {
  [1] = "escape", [14] = "backspace", [15] = "tab", [28] = "return",
  [57] = "space", [58] = "capslock",
  [29] = "left ctrl", [97] = "right ctrl",
  [42] = "left shift", [54] = "right shift",
  [56] = "left alt", [100] = "right alt",
  [102] = "home", [103] = "up", [104] = "pageup", [105] = "left",
  [106] = "right", [107] = "end", [108] = "down", [109] = "pagedown",
  [110] = "insert", [111] = "delete", [96] = "keypad enter",
  [12] = "-", [13] = "=", [26] = "[", [27] = "]", [39] = ";", [40] = "'",
  [41] = "`", [43] = "\\", [51] = ",", [52] = ".", [53] = "/",
  [87] = "f11", [88] = "f12",
}

for first, row in pairs({ [2] = "1234567890", [16] = "qwertyuiop",
                          [30] = "asdfghjkl", [44] = "zxcvbnm" }) do
  for i = 1, #row do host.KEY_NAMES[first + i - 1] = row:sub(i, i) end
end

for i = 1, 10 do host.KEY_NAMES[58 + i] = "f" .. i end

-- Whether a character code is text. A key pressed with Control or Alt held
-- meant a command rather than a character, so it is not.
function host.is_text(c, held)
  return type(c) == "number" and c >= 32 and c < 127
         and not (held[29] or held[97] or held[56])
end

--------------------------------------------------------------------------
-- Damage.
--------------------------------------------------------------------------

-- The rectangle covering what changed, clipped to a `w` by `h` window, as
-- x0, y0, x1, y1 - or nil when nothing did. `damage` is `take_damage()`'s
-- answer: `true` for the whole window, or a flat list of x, y, w, h.
function host.damage_bounds(damage, w, h)
  if damage == true then return 0, 0, w, h end

  if #damage == 0 then return nil end

  local x0, y0, x1, y1 = w, h, 0, 0

  for i = 1, #damage, 4 do
    local x, y, dw, dh = damage[i], damage[i + 1], damage[i + 2], damage[i + 3]

    if x < x0 then x0 = x end
    if y < y0 then y0 = y end
    if x + dw > x1 then x1 = x + dw end
    if y + dh > y1 then y1 = y + dh end
  end

  if x0 < 0 then x0 = 0 end
  if y0 < 0 then y0 = 0 end
  if x1 > w then x1 = w end
  if y1 > h then y1 = h end

  if x1 <= x0 or y1 <= y0 then return nil end

  return x0, y0, x1, y1
end

return host
