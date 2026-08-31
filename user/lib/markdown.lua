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

local markdown = {}

local function inline(text)
  -- Backtick spans become a marker the renderer understands, and the two
  -- emphasis forms are stripped rather than rendered: this font has one
  -- weight, so **bold** can only be a lie about what is on screen. The
  -- asterisks going away is the honest version.
  text = text:gsub("%*%*(.-)%*%*", "%1")
  text = text:gsub("%_%_(.-)%_%_", "%1")
  text = text:gsub("%*(.-)%*", "%1")

  return text
end

function markdown.parse(source)
  local blocks = {}
  local paragraph = {}

  local function flush()
    if #paragraph > 0 then
      blocks[#blocks + 1] = { kind = "para",
                              text = inline(table.concat(paragraph, " ")) }
      paragraph = {}
    end
  end

  local in_code = false
  local code = {}

  for line in (tostring(source) .. "\n"):gmatch("([^\n]*)\n") do
    if in_code then
      if line:match("^%s*```") then
        blocks[#blocks + 1] = { kind = "code",
                                text = table.concat(code, "\n") }
        code, in_code = {}, false
      else
        code[#code + 1] = line
      end
    elseif line:match("^%s*```") then
      flush()
      in_code = true
    elseif line:match("^%s*$") then
      flush()
    elseif line:match("^%s*[-*_][-*_%s]*$") and #line:gsub("%s", "") >= 3 then
      flush()
      blocks[#blocks + 1] = { kind = "rule" }
    else
      local hashes, rest = line:match("^(#+)%s+(.*)$")

      if hashes then
        flush()
        blocks[#blocks + 1] = { kind = "heading", level = #hashes,
                                text = inline(rest) }
      else
        local bullet = line:match("^%s*[-*+]%s+(.*)$")
        local number = line:match("^%s*%d+%.%s+(.*)$")

        if bullet or number then
          flush()
          blocks[#blocks + 1] = { kind = "item", text = inline(bullet or number) }
        else
          local quote = line:match("^%s*>%s?(.*)$")

          if quote then
            flush()
            blocks[#blocks + 1] = { kind = "quote", text = inline(quote) }
          else
            paragraph[#paragraph + 1] = line:match("^%s*(.-)%s*$")
          end
        end
      end
    end
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
