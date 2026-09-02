-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- PDF, in the part of it a reader needs: finding the objects.
--
-- This is the layer underneath a viewer. It knows how a PDF is put
-- together - the cross-reference table, indirect objects, dictionaries,
-- the page tree - and it knows nothing about drawing, fonts or pixels.
-- What comes out is data: a page's media box, its resources, and where its
-- content stream lives on the disk.
--
-- **It never holds the file.** A process has a 2 MB heap by design
-- (`design.md` 5.2) and the document this was written against is 1.6 MB, so
-- reading it into a string is not a tight fit, it is an impossibility with
-- nothing left over to parse it with. Everything here goes through
-- `source.read(offset, length)`, which is `kfs.read_range` inside the
-- machine and a seek and a read outside it. A PDF is the file type that
-- *wants* this: the cross-reference table exists precisely so a reader can
-- jump to object 431 without walking the 430 in front of it.
--
-- **The source is injected, the way kfs takes disk_read.** That is what
-- makes this testable on the host in a fraction of a second, against the
-- real document rather than a fixture, without booting anything. The same
-- argument `test_kfs.lua` makes, and the reason `build/host/lua` exists.
--
-- What it does not do, and where that will show:
--
--   * **Cross-reference streams and object streams** (`/Type /XRef`,
--     `/Type /ObjStm`), which is how PDF 1.5 and later pack objects. The
--     document this was built against is 1.4 and has neither. A 1.5 file
--     will fail at `open` with a clear message rather than half-work.
--   * **Encryption.** `/Encrypt` is refused rather than ignored, because
--     ignoring it produces streams that inflate into noise and an error
--     three layers away from the cause.
--   * **Decoding streams.** `stream_range` says where the bytes are and
--     what filter is on them; turning Flate into text is the next layer's
--     job, because it is a loop over bytes and `design.md` 6 puts those
--     in C.

local pdf = {}

-- How much is read at once. One window is live at a time and it is replaced,
-- never grown, so this is the memory cost of parsing, however large the
-- document is.
--
-- 256 because it was measured rather than chosen. Reading the 1127 objects
-- of a 1.6 MB document, against window size:
--
--      128   741 calls      96,649 bytes    5.7% of the file
--      256   383 calls      99,593 bytes    5.9%
--      512   310 calls     159,753 bytes    9.4%
--     2048   274 calls     559,113 bytes   32.9%
--     8192   265 calls   2,156,553 bytes  126.9%
--
-- The call count is set by how many objects are parsed and not by the
-- window, so it barely moves while the bytes grow with it - which is how
-- 8192 comes to read the whole file over again just to look at its index.
-- Below 256 the bytes stop falling and the calls double, and inside the
-- machine a call is an IPC round trip to the filesystem while a byte is a
-- memcpy, so that is the wrong side of the trade.
--
-- A value larger than this is still read whole: `peek` asks for what it
-- needs. The window is the floor, not the ceiling.
local WINDOW = 256

-- The two character classes the syntax is built out of. As byte values
-- rather than patterns, because this is asked once per character and a Lua
-- pattern match per byte is the kind of thing that makes a parser feel slow
-- for no reason anybody can point at.
local SPACE = { [0] = true, [9] = true, [10] = true, [12] = true,
                [13] = true, [32] = true }

local DELIM = { [40] = true, [41] = true, [60] = true, [62] = true,
                [91] = true, [93] = true, [123] = true, [125] = true,
                [47] = true, [37] = true }

--------------------------------------------------------------------------
-- A cursor over the file.
--
-- Holds one window and the position it starts at. Reading past the window
-- replaces it rather than extending it, so memory is constant however big
-- the document is.
--------------------------------------------------------------------------

local Cursor = {}
Cursor.__index = Cursor

local function cursor(source, pos)
  return setmetatable({ source = source, pos = pos or 0,
                        buf = "", base = 0 }, Cursor)
end

-- The next `n` bytes, as a string. Shorter than `n` at the end of the file,
-- which is how the callers detect it.
function Cursor:peek(n)
  n = n or 1

  if self.pos < self.base or self.pos + n > self.base + #self.buf then
    self.base = self.pos
    self.buf  = self.source.read(self.base, n > WINDOW and n or WINDOW) or ""
  end

  local i = self.pos - self.base + 1
  return self.buf:sub(i, i + n - 1)
end

-- The byte value at the cursor, or nil at the end.
function Cursor:at()
  local c = self:peek(1)
  if c == "" then return nil end
  return c:byte()
end

function Cursor:skip(n) self.pos = self.pos + (n or 1) end

-- Whitespace and comments. A comment runs to the end of the line, and `%`
-- is why it cannot simply be skipped as a delimiter.
function Cursor:space()
  while true do
    local b = self:at()

    if b == nil then return end

    if SPACE[b] then
      self.pos = self.pos + 1
    elseif b == 37 then                        -- '%'
      while true do
        local c = self:at()
        if c == nil or c == 10 or c == 13 then break end
        self.pos = self.pos + 1
      end
    else
      return
    end
  end
end

-- A run of ordinary characters: a number, a keyword, or the body of a name.
function Cursor:regular()
  local out = {}

  while true do
    local b = self:at()
    if b == nil or SPACE[b] or DELIM[b] then break end
    out[#out + 1] = string.char(b)
    self.pos = self.pos + 1
  end

  return table.concat(out)
end

--------------------------------------------------------------------------
-- Objects.
--
-- The eight kinds a PDF is made of. Represented as the Lua that is closest
-- to each: a number is a number, a name is a string, an array is a
-- sequence, a dictionary is a table. The two that have no Lua equivalent
-- get a tag - a reference and a stream - and `pdf.isref` is how a caller
-- tells the second kind of table from the first.
--------------------------------------------------------------------------

local REF = {}     -- the metatable that marks an indirect reference

function pdf.isref(v)
  return type(v) == "table" and getmetatable(v) == REF
end

local function ref(num, gen)
  return setmetatable({ num = num, gen = gen }, REF)
end

local parse_value    -- forward: the grammar is mutually recursive

-- A name: /Type, /F7, with #xx escapes for the awkward characters.
local function parse_name(cur)
  cur:skip(1)                                  -- the '/'
  local raw = cur:regular()

  if raw:find("#", 1, true) then
    raw = raw:gsub("#(%x%x)", function (h)
      return string.char(tonumber(h, 16))
    end)
  end

  return raw
end

-- A literal string: (like this), with backslash escapes and balanced
-- parentheses inside. The nesting is the part that catches people out - an
-- unescaped '(' inside is legal and has to be counted.
local function parse_literal(cur)
  cur:skip(1)                                  -- the '('

  local out, depth = {}, 1

  while true do
    local b = cur:at()
    if b == nil then error("pdf: unterminated string") end
    cur:skip(1)

    if b == 92 then                            -- backslash
      local e = cur:at()
      if e == nil then error("pdf: unterminated escape") end
      cur:skip(1)

      if     e == 110 then out[#out + 1] = "\n"
      elseif e == 114 then out[#out + 1] = "\r"
      elseif e == 116 then out[#out + 1] = "\t"
      elseif e == 98  then out[#out + 1] = "\b"
      elseif e == 102 then out[#out + 1] = "\f"
      elseif e >= 48 and e <= 55 then          -- \ooo, one to three octal
        local digits = string.char(e)
        for _ = 1, 2 do
          local d = cur:at()
          if d and d >= 48 and d <= 55 then
            digits = digits .. string.char(d)
            cur:skip(1)
          else
            break
          end
        end
        out[#out + 1] = string.char(tonumber(digits, 8) % 256)
      elseif e == 10 then                      -- a line continuation
      elseif e == 13 then
        if cur:at() == 10 then cur:skip(1) end
      else
        out[#out + 1] = string.char(e)         -- \( \) \\ and anything else
      end
    elseif b == 40 then                        -- '('
      depth = depth + 1
      out[#out + 1] = "("
    elseif b == 41 then                        -- ')'
      depth = depth - 1
      if depth == 0 then break end
      out[#out + 1] = ")"
    else
      out[#out + 1] = string.char(b)
    end
  end

  return table.concat(out)
end

-- A hex string: <48656C6C6F>. An odd number of digits pads with a zero,
-- which the specification asks for and which real files do rely on.
local function parse_hex(cur)
  cur:skip(1)                                  -- the '<'

  local digits = {}

  while true do
    local b = cur:at()
    if b == nil then error("pdf: unterminated hex string") end
    cur:skip(1)
    if b == 62 then break end                  -- '>'
    local c = string.char(b)
    if c:match("%x") then digits[#digits + 1] = c end
  end

  local hex = table.concat(digits)
  if #hex % 2 == 1 then hex = hex .. "0" end

  return (hex:gsub("%x%x", function (pair)
    return string.char(tonumber(pair, 16))
  end))
end

local function parse_dict(cur)
  cur:skip(2)                                  -- the '<<'

  local dict = {}

  while true do
    cur:space()

    local b = cur:at()
    if b == nil then error("pdf: unterminated dictionary") end

    if b == 62 then                            -- '>>'
      cur:skip(2)
      return dict
    end

    if b ~= 47 then                            -- '/'
      error("pdf: a dictionary key must be a name, at " .. cur.pos)
    end

    local key = parse_name(cur)
    dict[key] = parse_value(cur)
  end
end

local function parse_array(cur)
  cur:skip(1)                                  -- the '['

  local out = {}

  while true do
    cur:space()

    local b = cur:at()
    if b == nil then error("pdf: unterminated array") end

    if b == 93 then                            -- ']'
      cur:skip(1)
      return out
    end

    out[#out + 1] = parse_value(cur)
  end
end

-- A number, and the lookahead that turns two of them plus `R` into a
-- reference. There is no other way to tell: `1 0 R` and the three separate
-- values `1`, `0`, `R` are the same characters, and only the `R` decides.
local function parse_number(cur)
  local text = cur:regular()
  local n = tonumber(text)

  if n == nil then error("pdf: not a number: " .. text .. " at " .. cur.pos) end

  -- Only an integer can begin a reference, so a real number stops here.
  if not text:find("[.eE]") then
    local save = cur.pos

    cur:space()
    local second = cur:regular()

    if second:match("^%d+$") then
      cur:space()
      if cur:at() == 82 then                   -- 'R'
        local after = cur.pos + 1
        cur.pos = after
        -- `R` has to stand alone: `RG` is an operator, not a reference.
        local b = cur:at()
        if b == nil or SPACE[b] or DELIM[b] then
          return ref(math.tointeger(n) or n, tonumber(second))
        end
        cur.pos = after - 1
      end
    end

    cur.pos = save
  end

  return n
end

parse_value = function (cur)
  cur:space()

  local b = cur:at()
  if b == nil then error("pdf: unexpected end of file") end

  if b == 47 then return parse_name(cur) end   -- '/'
  if b == 40 then return parse_literal(cur) end
  if b == 91 then return parse_array(cur) end

  if b == 60 then                              -- '<' or '<<'
    if cur:peek(2) == "<<" then return parse_dict(cur) end
    return parse_hex(cur)
  end

  if (b >= 48 and b <= 57) or b == 43 or b == 45 or b == 46 then
    return parse_number(cur)
  end

  local word = cur:regular()

  if word == "true"  then return true end
  if word == "false" then return false end
  if word == "null"  then return nil end
  if word == ""      then error("pdf: cannot parse at " .. cur.pos) end

  -- `endobj`, `stream` and friends reach the caller as a keyword, which is
  -- how object parsing knows it has run off the end of the value.
  return { keyword = word }
end

pdf.parse_value = parse_value

-- A source over a string, for bytes that are already in hand: an inflated
-- content stream, a CMap. The same `read`/`size` pair a file gives, so the
-- cursor and the parser cannot tell the difference and nothing needs a
-- second implementation to walk them.
function pdf.from_string(str)
  return {
    size = #str,
    read = function (offset, length)
      return str:sub(offset + 1, offset + length)
    end,
  }
end

-- A cursor a caller can drive itself, which is what a content stream needs:
-- its operators are values with a keyword after them, not a fixed grammar.
function pdf.cursor(source, pos)
  return cursor(source, pos)
end

--------------------------------------------------------------------------
-- The document.
--------------------------------------------------------------------------

local Doc = {}
Doc.__index = Doc

-- The four attributes a page inherits from the tree above it rather than
-- carrying itself.
local INHERITED = { "Resources", "MediaBox", "CropBox", "Rotate" }

-- One indirect object, parsed at the offset the table gave for it.
--
-- The offset is checked rather than trusted: a table that points at the
-- wrong place is a corrupt file, and a parser that follows it produces
-- something that looks like an object and is not.
function Doc:get(num, gen)
  local hit = self.cache[num]
  if hit ~= nil then return hit end

  local offset = self.xref[num]
  if offset == nil then return nil end

  local cur = cursor(self.source, offset)

  cur:space()
  local got_num = tonumber(cur:regular())
  cur:space()
  local got_gen = tonumber(cur:regular())
  cur:space()
  local word = cur:regular()

  if word ~= "obj" or got_num ~= num then
    error(("pdf: object %d is not at %d (found %s %s %s)")
          :format(num, offset, tostring(got_num), tostring(got_gen), word))
  end

  local value = parse_value(cur)

  -- A stream is a dictionary followed by the keyword and then raw bytes.
  -- Where they are is recorded; what they mean is not this layer's business.
  if type(value) == "table" and not pdf.isref(value) then
    local save = cur.pos
    cur:space()

    if cur:peek(6) == "stream" then
      cur:skip(6)
      if cur:peek(2) == "\r\n" then cur:skip(2)
      elseif cur:at() == 10 or cur:at() == 13 then cur:skip(1) end

      local length = self:resolve(value.Length)
      if type(length) ~= "number" then
        error("pdf: stream " .. num .. " has no usable /Length")
      end

      value = { dict = value, offset = cur.pos, length = length,
                stream = true }
    else
      cur.pos = save
    end
  end

  self.cache[num] = value
  return value
end

-- One page, resolved when it is asked for.
--
-- What comes back is `dict` - the page's own dictionary - together with the
-- attributes it inherits from the tree above it, which were worked out once
-- when the document was opened.
function Doc:page(index)
  local at = self.pages[index]

  if at == nil then return nil end

  local dict = type(at) == "number" and self:get(at) or at

  if type(dict) ~= "table" then return nil end

  local page = { dict = dict }

  -- Up the tree, and the nearest definition wins: an attribute on the page
  -- itself overrides the one on its parent, which is what "inherited"
  -- means here.
  local node, depth = dict, 0

  while type(node) == "table" and depth < 64 do
    for _, key in ipairs(INHERITED) do
      if page[key] == nil and node[key] ~= nil then page[key] = node[key] end
    end

    node = self:resolve(node.Parent)
    depth = depth + 1
  end

  return page
end

-- Everything parsed so far, dropped.
--
-- The cache exists so that resolving the same object twice costs one parse,
-- and on a 2 MB heap it must also be possible to say "not any more". A
-- reader that has turned forty pages has no use for the thirty-nine it left
-- behind, and nothing here is expensive enough to re-read to be worth the
-- room.
function Doc:forget()
  self.cache = {}
end

-- A value, with a reference followed if that is what it is. Everything that
-- reads a dictionary goes through this, because almost any value in a PDF
-- is allowed to be indirect and most of them are in some file somewhere.
function Doc:resolve(v)
  if pdf.isref(v) then return self:get(v.num, v.gen) end
  return v
end

-- Where a stream's bytes are, and what is on top of them. The caller reads
-- and decodes; this only says where to look.
function Doc:stream_range(obj)
  obj = self:resolve(obj)

  if type(obj) ~= "table" or not obj.stream then return nil end

  local filter = self:resolve(obj.dict.Filter)

  -- One filter is a name, several are an array. Callers should not have to
  -- care which, so it always comes back as a list.
  if type(filter) == "string" then filter = { filter }
  elseif filter == nil then filter = {} end

  return obj.offset, obj.length, filter
end

--------------------------------------------------------------------------
-- The cross-reference table.
--
-- Read backwards from the end, which is the one thing a PDF guarantees:
-- `startxref` near the end gives the offset of a table, that table's
-- trailer may name a previous one with /Prev, and following the chain
-- gives every object. Earlier tables lose to later ones, so a later
-- entry is never overwritten by the older file it was built on.
--------------------------------------------------------------------------

local function find_startxref(source, size)
  -- The tail, because `startxref` is within a few dozen bytes of the end
  -- and scanning 1.6 MB backwards to find it would defeat the purpose.
  local want = 2048
  local from = size > want and size - want or 0
  local tail = source.read(from, size - from) or ""

  local at = nil
  local pos = 1

  while true do
    local found = tail:find("startxref", pos, true)
    if not found then break end
    at = found
    pos = found + 1
  end

  if not at then error("pdf: no startxref; this is not a PDF") end

  local offset = tail:match("startxref%s+(%d+)", at)
  if not offset then error("pdf: startxref names no offset") end

  return tonumber(offset)
end

local function read_xref_section(doc, offset, seen)
  if seen[offset] then return nil end          -- a /Prev loop
  seen[offset] = true

  local cur = cursor(doc.source, offset)
  cur:space()

  if cur:peek(4) ~= "xref" then
    -- PDF 1.5's cross-reference stream. Saying so plainly beats failing
    -- later with a parse error that names a byte offset and nothing else.
    error("pdf: cross-reference streams are not supported "
          .. "(this file is PDF 1.5 or later)")
  end

  cur:skip(4)

  -- Subsections: a first object number, a count, then that many 20-byte
  -- records. `n` is in use and `f` is free.
  while true do
    cur:space()

    if cur:peek(7) == "trailer" then
      cur:skip(7)
      return parse_value(cur)
    end

    local first = tonumber(cur:regular())
    if first == nil then error("pdf: broken xref subsection at " .. cur.pos) end

    cur:space()
    local count = tonumber(cur:regular())
    if count == nil then error("pdf: xref subsection has no count") end

    for i = 0, count - 1 do
      cur:space()
      local at   = tonumber(cur:regular())
      cur:space()
      local _gen = cur:regular()
      cur:space()
      local kind = cur:regular()

      local num = first + i

      -- First writer wins: the newest table is read first, and an older
      -- one must not undo it.
      if kind == "n" and doc.xref[num] == nil then
        doc.xref[num] = at
      end
    end
  end
end

--------------------------------------------------------------------------
-- The page tree.
--
-- /Root -> /Pages -> /Kids, and /Kids may hold more /Pages nodes. Four
-- attributes are inherited down that tree rather than repeated on every
-- page, which is why this carries them along instead of reading each page
-- on its own.
--------------------------------------------------------------------------


local function walk(doc, ref, out, depth)
  if depth > 64 then error("pdf: the page tree is too deep to be real") end

  local node = doc:resolve(ref)
  if type(node) ~= "table" then return end

  local kids = doc:resolve(node.Kids)

  if node.Type == "Pages" or (kids ~= nil and node.Type ~= "Page") then
    for _, kid in ipairs(kids or {}) do
      walk(doc, kid, out, depth + 1)
    end
    return
  end

  --
  -- The object number, and nothing else.
  --
  -- A page entry used to be a table holding a reference and the four
  -- inherited attributes - two tables per page, five hundred for a book,
  -- and about 310 KB of a 2 MB heap still held after opening. A windowed
  -- program has the UI kit in it as well and could not then read a page at
  -- all, while a console program with the same document could.
  --
  -- What is stored now is an integer. The inherited attributes are not
  -- lost: every page dictionary has a `/Parent`, so `Doc:page` walks up
  -- when it is asked, and the nodes it passes through are few and cached.
  --
  out[#out + 1] = pdf.isref(ref) and ref.num or node
end

--------------------------------------------------------------------------
-- Opening one.
--------------------------------------------------------------------------

-- `source` is a table with `read(offset, length) -> string` and `size`.
-- Nothing else: whether that is a file on this Mac or a capability into
-- the disk inside the machine is not this layer's concern.
function pdf.open(source)
  if type(source) ~= "table" or type(source.read) ~= "function" then
    error("pdf.open: a source needs read(offset, length)")
  end

  local size = source.size
  if type(size) ~= "number" then error("pdf.open: a source needs size") end

  local head = source.read(0, 9) or ""
  local version = head:match("^%%PDF%-(%d+%.%d+)")
  if not version then error("pdf: no %PDF header; this is not a PDF") end

  local doc = setmetatable({
    source = source, size = size, version = version,
    xref = {}, cache = {},
  }, Doc)

  local offset  = find_startxref(source, size)
  local seen    = {}
  local trailer = read_xref_section(doc, offset, seen)

  doc.trailer = trailer or {}

  -- Older tables, oldest last. Each may add objects the newest one does
  -- not mention, and none of them may replace what it does.
  local prev = doc.trailer.Prev
  while type(prev) == "number" do
    local older = read_xref_section(doc, prev, seen)
    if older == nil then break end
    for key, value in pairs(older) do
      if doc.trailer[key] == nil then doc.trailer[key] = value end
    end
    prev = older.Prev
  end

  if doc.trailer.Encrypt ~= nil then
    error("pdf: this document is encrypted, which is not supported")
  end

  local root = doc:resolve(doc.trailer.Root)
  if type(root) ~= "table" then error("pdf: the trailer names no catalog") end
  doc.catalog = root

  local pages = {}
  walk(doc, root.Pages, pages, 0)
  doc.pages = pages

  -- Everything the walk resolved on its way through the tree, let go of.
  -- The page dictionaries are the bulk of it and nothing holds them now;
  -- the trailer and the catalog are fields on the document rather than
  -- cache entries, so they survive.
  doc:forget()

  return doc
end

return pdf
