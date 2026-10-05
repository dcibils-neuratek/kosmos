-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Markdown, in the part of it that a manual actually uses.
--
-- Headings, paragraphs, bullet lists, code blocks, block quotes, horizontal
-- rules, and `code` inside a line. Not tables, not links, not images, not
-- nested lists - `roadmap.md` asks for "manuals and tutorials in the system
-- itself", and every one of those is made of the seven things above.
--
-- **Line-based on purpose.** A real markdown parser builds a tree because
-- markdown nests, and the nesting is where the specification gets long and
-- disagreeable. This walks lines and emits a flat list of blocks, which is
-- a hundred lines instead of a thousand and renders every document in
-- `docs/` correctly. When something needs a nested list, that is the moment
-- to find out whether a tree is worth it.
--
-- What comes out is data, not pixels: a list of
-- `{ kind, text, level }`, which the viewer lays out. Keeping the parser
-- free of any idea of a font or a width is what lets the same output be
-- re-wrapped when a window is resized.
--
-- **What a line is, is `mdstyle.lua`'s to say** - the one reader of
-- Markdown, which Text Editor styles a line with as it is written. This
-- had a reader of its own, patterns that agreed with that one on most
-- lines and not on all: it took emphasis out of a code span, and
-- `2 * 3 * 4` lost its asterisks. Now it asks, and keeps only what Reader
-- needs on top: lines joined into paragraphs, items and quotations, and
-- the marks left out.

local md = use("/Kosmos/Libraries/mdstyle.lua")

local markdown = {}

--
-- The words of `s` from byte `from`, as Reader shows them: the marks that
-- make bold and italic taken out, since this font has one weight and
-- **bold** can only be a lie about what is on screen - the asterisks going
-- away is the honest version. A code span keeps its backticks and a link
-- its brackets, which say something the face cannot.
--
local function words(s, from)
  local out = {}

  for _, span in ipairs(md.inline(s, from)) do
    local text = s:sub(span[1], span[2])

    if span[3] == "mark" then text = text:gsub("[*_]", "") end

    out[#out + 1] = text
  end

  return table.concat(out)
end

-- The block each kind of line starts; a list's three kinds are one here.
local ITEM = { item = true, number = true, check = true }

--
-- **A paragraph, an item or a quotation goes on until a line says
-- otherwise**: a plain line under an item is more of the item, and quoted
-- lines one after another are one quotation - CommonMark's lazy
-- continuation. Each line was a block of its own, so an item's second line
-- was a paragraph after a gap, and bold that began on one line and ended on
-- the next was two stray marks once the marks were read rather than
-- guessed at.
--
function markdown.parse(source)
  local blocks = {}
  local open = nil          -- { kind, parts }: the block still being written

  local function flush()
    if open then
      blocks[#blocks + 1] = { kind = open.kind,
                              text = words(table.concat(open.parts, " "), 1) }
      open = nil
    end
  end

  local function start(kind, text)
    flush()
    open = { kind = kind, parts = { text } }
  end

  local in_code = false
  local code = {}

  for line in (tostring(source) .. "\n"):gmatch("([^\n]*)\n") do
    local info, after = md.line(line, in_code)
    local kind = info.kind

    if kind == "fence" then
      if in_code then
        blocks[#blocks + 1] = { kind = "code", text = table.concat(code, "\n") }
        code = {}
      else
        flush()
      end
    elseif kind == "code" then
      code[#code + 1] = line
    elseif kind == "blank" then
      flush()
    elseif kind == "rule" then
      flush()
      blocks[#blocks + 1] = { kind = "rule" }
    elseif kind:match("^h%d$") then
      flush()
      blocks[#blocks + 1] = { kind = "heading", level = #line:match("^#+"),
                              text = words(line, info.hang + 1) }
    elseif ITEM[kind] then
      -- A checklist's box stays in its words: `[ ] swing`, `[x] done`.
      start("item", line:sub(kind == "check" and info.box - 1 or info.hang + 1))
    elseif kind == "quote" and open and open.kind == "quote" then
      open.parts[#open.parts + 1] = line:sub(info.hang + 1)
    elseif kind == "quote" then
      start("quote", line:sub(info.hang + 1))
    elseif open then
      open.parts[#open.parts + 1] = line:match("^%s*(.-)%s*$")
    else
      start("para", line:match("^%s*(.-)%s*$"))
    end

    in_code = after
  end

  flush()

  if in_code and #code > 0 then
    blocks[#blocks + 1] = { kind = "code", text = table.concat(code, "\n") }
  end

  return blocks
end

-- Blocks to lines that fit a width, in characters.
--
-- Separate from parsing because a window can be resized and the document
-- has not changed: re-wrapping is cheap and re-parsing is not, and mixing
-- them would mean doing both every time.
function markdown.wrap(blocks, columns)
  local out = {}

  local function push(kind, text, level)
    out[#out + 1] = { kind = kind, text = text, level = level }
  end

  local function wrapped(text, width, indent)
    local line = ""

    for word in tostring(text):gmatch("%S+") do
      if line == "" then
        line = word
      elseif #line + 1 + #word <= width then
        line = line .. " " .. word
      else
        return line, text:sub(#line + 2)
      end
    end

    return line, nil
  end

  for _, b in ipairs(blocks) do
    if b.kind == "code" then
      if #out > 0 then push("blank") end

      for line in (b.text .. "\n"):gmatch("([^\n]*)\n") do
        push("code", line)
      end

      push("blank")
    elseif b.kind == "rule" then
      push("rule")
    elseif b.kind == "blank" then
      push("blank")
    else
      -- A blank between blocks of different kinds, so a paragraph that
      -- follows a list is not read as one more item. Between two items of
      -- the same kind there is none, because a list with a gap between
      -- every entry is a list that has stopped looking like one.
      local last = out[#out]

      if #out > 0 and last and last.kind ~= "blank"
         and last.kind ~= b.kind then
        push("blank")
      end

      local indent = (b.kind == "item") and 2
                     or (b.kind == "quote") and 2 or 0
      local width  = columns - indent
      local rest   = b.text
      local first  = true

      while rest and rest ~= "" do
        local line
        line, rest = wrapped(rest, width)

        if line == "" then break end

        local prefix = ""

        if b.kind == "item" then
          prefix = first and "- " or "  "
        elseif b.kind == "quote" then
          prefix = first and "| " or "| "
        end

        push(b.kind, prefix .. line, b.level)
        first = false
      end

      if b.kind == "heading" then push("blank") end
    end
  end

  return out
end

return markdown
