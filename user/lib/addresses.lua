-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The addresses a person has written to and heard from, for completing one
-- as it is typed (`docs/mail.md` M6).
--
--   local addresses = use("/Kosmos/Libraries/addresses.lua")
--   local book = addresses.gather()            every account's kept mail
--   addresses.match(book, "pri", 6)            -> { { name =, address =, count = }, ... }
--
-- **Read from the mail itself**, not kept beside it: each message `maild`
-- keeps carries who sent it (`from`, `from_address`) and whom it went to
-- (`to`, a line each, "name<TAB>address") as attributes, so the book is
-- the mail's and cannot disagree with it. A message is one `getattr`; the
-- book is made when a composer opens, and a mailbox of a few thousand is
-- a few thousand small reads.
--
-- A library rather than Mail's own, because it is not about Mail: whatever
-- wants "who does this person write to" - a share's people, a calendar's
-- guests - asks the same question of the same mail.

local addresses = {}

local HOME = "/Home/Mail"

local function add(book, name, address, weight)
  address = tostring(address or ""):lower()

  if not address:match("^[^%s@]+@[^%s@]+$") then return end

  local e = book.by[address]

  if not e then
    e = { name = "", address = address, count = 0 }
    book.by[address] = e
    book.list[#book.list + 1] = e
  end

  e.count = e.count + weight

  -- The longest name it was given: "Priya Nair" over "Priya".
  name = tostring(name or "")
  if name ~= address and #name > #e.name then e.name = name end
end

-- Every message's attributes in a folder, and the folders inside it.
local function walk(book, folder, depth)
  if depth > 4 then return end

  for _, name in ipairs(fs.list(folder) or {}) do
    local path = folder .. "/" .. name

    if name:match("%.eml$") then
      local f = fs.getattr(path)

      if type(f) == "table" and f.type == "mail" then
        add(book, f.from ~= f.from_address and f.from or "", f.from_address, 1)

        -- And whom it went to: in Sent, the people this person wrote to;
        -- elsewhere, the others a message was shared with.
        for line in tostring(f.to or ""):gmatch("[^\n]+") do
          local n, a = line:match("^(.-)\t(.+)$")

          add(book, n, a, 1)
        end
      end
    elseif type(fs.list(path)) == "table" then
      walk(book, path, depth + 1)       -- a mailbox inside: Gmail's [Gmail]/Sent Mail
    end
  end
end

--
-- Every account's mail, gathered: the addresses, each with the longest
-- name it came with and how often it was seen. The account's own address
-- is left out - nobody completes their own name.
--
function addresses.gather(own)
  local book = { by = {}, list = {} }

  for _, account in ipairs(fs.list(HOME) or {}) do
    if fs.getattr(HOME .. "/" .. account .. "/account") then
      walk(book, HOME .. "/" .. account, 0)
    end
  end

  for _, o in ipairs(own or {}) do
    local e = book.by[tostring(o):lower()]

    if e then
      e.count = -1
      book.by[e.address] = nil
    end
  end

  return book
end

--
-- **What the typing could be**: addresses whose address begins with it,
-- or one of whose name's words does - "pri" finds Priya Nair and
-- priya.nair@ - most written to first, at most `most`.
--
function addresses.match(book, typed, most)
  typed = tostring(typed or ""):lower():gsub("^%s+", "")

  if typed == "" then return {} end

  local out = {}

  for _, e in ipairs(book.list) do
    if e.count >= 0 then
      local hit = e.address:sub(1, #typed) == typed

      if not hit then
        for word in e.name:lower():gmatch("[^%s,\"']+") do
          if word:sub(1, #typed) == typed then hit = true break end
        end

        hit = hit or e.name:lower():sub(1, #typed) == typed
      end

      if hit then out[#out + 1] = e end
    end
  end

  table.sort(out, function(x, y)
    if x.count ~= y.count then return x.count > y.count end
    return x.address < y.address
  end)

  for i = #out, (most or 6) + 1, -1 do out[i] = nil end

  return out
end

return addresses
