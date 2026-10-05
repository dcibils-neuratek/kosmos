-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Markdown, as Text Editor shows it while it is written (`roadmap.md` 6zs,
-- `docs/texteditor.html`).
--
--   local md = use("/Kosmos/Libraries/mdstyle.lua")
--   local info, in_code = md.line("## Sound", false)
--   -- info.kind == "h2", info.hang == 3, info.spans == { ... }
--
-- **What a line is, not what it becomes.** Nothing here changes the text:
-- a line is read and said to be a heading, an item, a quotation, and which
-- of its bytes are marks, bold, code or a link - and the page draws the
-- same bytes that way. What is on the disk is what is on the screen
-- (agreed on 28 September: the marks kept, faint).
--
-- `info` is:
--
--   kind     "h1" "h2" "h3" "para" "item" "number" "check" "quote"
--            "code" "fence" "rule" "blank"
--   hang     how many bytes at the start are the line's mark - `## `,
--            `- `, `- [ ] `, `> ` - drawn in the margin; 0 when none
--   level    how deep a list item is, from its indent, 0 first
--   checked  a checklist item that is ticked
--   box      the byte of the ` ` or `x` inside a checklist item's brackets
--   spans    `{ from, to, style }` over the bytes after the hang, in
--            order, every byte in one: style nil (plain), "mark", "bold",
--            "italic", "bolditalic", "code", "link" or "url"
--
-- `in_code` is whether a fenced block is open after the line, which the
-- next line is read with.
--
-- The Markdown people write in notes and no more: tables and images are
-- left as text (`docs/texteditor.html`, "What it understands"). Pure, and
-- `tools/test_mdstyle.lua` holds it on the Mac.

local md = {}

--------------------------------------------------------------------------
-- Inside a line: code, links, bold and italic.
--------------------------------------------------------------------------

local function styled(s, from)
  local n = #s
  local style = {}        -- byte -> "mark", "code", "link", "url"
  local bold, italic = {}, {}
  local taken = {}        -- bytes code and links own, where nothing else goes

  local function claim(a, b, what)
    for i = a, b do style[i], taken[i] = what, true end
  end

  local function free(a, b)
    for i = a, b do
      if taken[i] then return false end
    end

    return true
  end

  -- Code, `between backticks`: nothing inside is anything else.
  local at = from

  while true do
    local a = s:find("`", at, true)

    if not a then break end

    local b = s:find("`", a + 1, true)

    if not b then break end

    if b > a + 1 then
      claim(a, a, "mark")
      claim(a + 1, b - 1, "code")
      claim(b, b, "mark")
    end

    at = b + 1
  end

  -- Links, [words](place).
  at = from

  while true do
    local a, e, words, place = s:find("%[([^%]]+)%]%(([^%)]*)%)", at)

    if not a then break end

    if free(a, e) then
      local b = a + #words + 1           -- the `]`

      claim(a, a, "mark")
      claim(a + 1, b - 1, "link")
      claim(b, b + 1, "mark")            -- `](`
      claim(b + 2, b + 1 + #place, "url")
      claim(e, e, "mark")
    end

    at = e + 1
  end

  -- **Bold**, then *italic*, their marks only on bytes nothing has claimed.
  -- What is between them may be code or a link - two stars round a code
  -- span, as the docs write a name they mean, are bold code, as one star
  -- round it was already italic code - and the closing pair is the next
  -- one outside them. Bold used to ask for every byte between to be free,
  -- and such a name read as a stray star and an italic one.
  local function pairs_of(open, flag)
    local len = #open
    local i = from

    while i <= n do
      local a = s:find(open, i, true)

      if not a then return end

      local b = s:find(open, a + len + 1, true)

      while b and not free(b, b + len - 1) do b = s:find(open, b + 1, true) end

      if not b then return end

      local inner_a, inner_b = a + len, b - 1
      local first = s:sub(inner_a, inner_a)

      if first ~= " " and free(a, a + len - 1) then
        for k = a, a + len - 1 do style[k] = "mark" end
        for k = b, b + len - 1 do style[k] = "mark" end
        for k = inner_a, inner_b do flag[k] = true end
        for k = a, b + len - 1 do taken[k] = taken[k] or (style[k] == "mark") end
        i = b + len
      else
        i = a + len
      end
    end
  end

  pairs_of("**", bold)
  pairs_of("__", bold)

  -- A single `*` is emphasis only when it is not half of a `**`.
  local i = from

  while i <= n do
    local a = s:find("*", i, true)

    if not a then break end

    if style[a] == "mark" or taken[a] then
      i = a + 1
    else
      local b = a + 1

      while b <= n and (s:sub(b, b) ~= "*" or style[b] == "mark" or taken[b]) do
        b = b + 1
      end

      if b <= n and b > a + 1 and s:sub(a + 1, a + 1) ~= " " then
        style[a], style[b] = "mark", "mark"
        taken[a], taken[b] = true, true

        for k = a + 1, b - 1 do italic[k] = true end

        i = b + 1
      else
        i = a + 1
      end
    end
  end

  -- Runs of one style.
  local spans = {}

  for k = from, n do
    local what = style[k]

    if what == nil then
      if bold[k] and italic[k] then what = "bolditalic"
      elseif bold[k] then what = "bold"
      elseif italic[k] then what = "italic" end
    end

    local last = spans[#spans]

    if last and last[3] == what and last[2] == k - 1 then
      last[2] = k
    else
      spans[#spans + 1] = { k, k, what }
    end
  end

  return spans
end

md.inline = styled

--------------------------------------------------------------------------
-- A line.
--------------------------------------------------------------------------

local function whole(s, from, what)
  if from > #s then return {} end

  return { { from, #s, what } }
end

function md.line(s, in_code)
  s = tostring(s or "")

  -- A fence opens or closes a block, and is a mark itself.
  if s:match("^%s*```") then
    return { kind = "fence", hang = 0, level = 0, spans = whole(s, 1, "mark") },
           not in_code
  end

  if in_code then
    return { kind = "code", hang = 0, level = 0, spans = whole(s, 1, "code") }, true
  end

  if s:match("^%s*$") then
    return { kind = "blank", hang = 0, level = 0, spans = {} }, false
  end

  if s:match("^%s*[-*_]%s*[-*_]%s*[-*_][-*_%s]*$") then
    return { kind = "rule", hang = #s, level = 0, spans = {} }, false
  end

  local hashes, gap = s:match("^(#+)(%s+)")

  if hashes and #hashes <= 6 then
    local hang = #hashes + #gap

    return { kind = "h" .. math.min(3, #hashes), hang = hang, level = 0,
             spans = styled(s, hang + 1) }, false
  end

  local indent, bullet, space, box = s:match("^(%s*)([-*+])(%s+)%[([ xX])%]%s")

  if indent then
    local hang = #indent + #bullet + #space + 4
    local mark_box = #indent + #bullet + #space + 2

    return { kind = "check", hang = hang, level = #indent // 2,
             checked = (box ~= " "), box = mark_box,
             spans = styled(s, hang + 1) }, false
  end

  indent, bullet, space = s:match("^(%s*)([-*+])(%s+)")

  if indent then
    local hang = #indent + #bullet + #space

    return { kind = "item", hang = hang, level = #indent // 2,
             spans = styled(s, hang + 1) }, false
  end

  local digits

  indent, digits, space = s:match("^(%s*)(%d+[.)])(%s+)")

  if indent then
    local hang = #indent + #digits + #space

    return { kind = "number", hang = hang, level = #indent // 2,
             spans = styled(s, hang + 1) }, false
  end

  local quote = s:match("^(%s*>%s?)")

  if quote then
    return { kind = "quote", hang = #quote, level = 0,
             spans = styled(s, #quote + 1) }, false
  end

  return { kind = "para", hang = 0, level = 0, spans = styled(s, 1) }, false
end

--------------------------------------------------------------------------
-- What Return starts: the next item of a list, or its end.
--------------------------------------------------------------------------

--
-- `lead` is what the new line begins with - the same bullet, the next
-- number, an unticked box - and `ends` is true when the item was empty, in
-- which case Return takes its mark away instead of starting another: the
-- way out of a list, as every editor has it.
--
function md.continue(s)
  s = tostring(s or "")

  local info = md.line(s, false)
  local empty = (info.hang >= #s)

  if info.kind == "check" then
    local indent, bullet = s:match("^(%s*)([-*+])")

    return indent .. bullet .. " [ ] ", empty
  end

  if info.kind == "item" then
    return s:sub(1, info.hang), empty
  end

  if info.kind == "number" then
    local indent, n, dot = s:match("^(%s*)(%d+)([.)])")

    return indent .. tostring(tonumber(n) + 1) .. dot .. " ", empty
  end

  if info.kind == "quote" then
    return s:sub(1, info.hang), empty
  end

  return nil, false
end

return md
