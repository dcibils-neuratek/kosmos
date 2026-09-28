-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Markdown as Text Editor reads it while it is written (`roadmap.md` 6zs):
-- what each line is, which of its bytes are its mark, and the bold, the
-- code and the links inside it. On this machine, since it is pure.
--
--   build/host/lua tools/test_mdstyle.lua

local md = assert(loadfile(arg and arg[1] or "user/lib/mdstyle.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- A line's spans, written out: "plain|*mark*|bold:words".
local function shape(s, in_code)
  local info = md.line(s, in_code)
  local out = {}

  for _, sp in ipairs(info.spans) do
    local text = s:sub(sp[1], sp[2])

    out[#out + 1] = sp[3] and (sp[3] .. ":" .. text) or text
  end

  return info, table.concat(out, "|")
end

local function is(s, kind, hang, want, what)
  local info, got = shape(s)

  check(info.kind == kind and info.hang == hang and got == want,
        ("%s: %s %d %q, not %s %d %q"):format(what, info.kind, info.hang, got,
                                               kind, hang, want))
end

-- What a line is.
is("# Grooves", "h1", 2, "Grooves", "a heading, its mark the hash and the space")
is("## Sound", "h2", 3, "Sound", "a second heading")
is("#### Deep", "h3", 5, "Deep", "a fourth is shown as the third")
is("#hashtag", "para", 0, "#hashtag", "a hash with no space is not a heading")
is("- one", "item", 2, "one", "a bullet")
is("  * two", "item", 4, "two", "a bullet further in")
is("12. twelfth", "number", 4, "twelfth", "a numbered item")
is("- [ ] swing", "check", 6, "swing", "a checklist item")
is("> heard", "quote", 2, "heard", "a quotation")
is("---", "rule", 3, "", "a rule, all of it the mark")
is("   ", "blank", 0, "", "a blank line")
is("plain words", "para", 0, "plain words", "a paragraph")

local info = md.line("- [x] done", false)

check(info.checked and info.box == 4, "a ticked box, and where its x is")
check(not md.line("- [ ] open", false).checked, "an open box is not ticked")
check(md.line("    - deeper", false).level == 2, "a level from the indent")

-- Inside a line.
is("a **bold** b", "para", 0, "a |mark:**|bold:bold|mark:**| b", "bold between its marks")
is("an *it* here", "para", 0, "an |mark:*|italic:it|mark:*| here", "italic")
is("***both***", "para", 0, "mark:***|bolditalic:both|mark:***",
   "bold and italic at once")
is("run `x * y` now", "para", 0, "run |mark:`|code:x * y|mark:`| now",
   "a star inside code is code")
is("see [the server](audio.md).", "para", 0,
   "see |mark:[|link:the server|mark:](|url:audio.md|mark:)|.", "a link")
is("2 * 3 * 4", "para", 0, "2 * 3 * 4",
   "a star with a space after it is arithmetic, as CommonMark has it")
is("a * b", "para", 0, "a * b", "a star with nothing to close it is a star")
is("## A **big** one", "h2", 3, "A |mark:**|bold:big|mark:**| one", "bold in a heading")

-- Fenced code: the fence is a mark, what is inside is code and nothing else.
local fence, open = md.line("```lua", false)

check(fence.kind == "fence" and open, "a fence opens a block")

local inside, still = md.line("local a = **b**", true)

check(inside.kind == "code" and still and #inside.spans == 1
      and inside.spans[1][3] == "code", "inside a block, stars are code")

local closing, shut = md.line("```", true)

check(closing.kind == "fence" and not shut, "and a fence closes it")

-- What Return starts.
local lead, ends = md.continue("- [x] one")

check(lead == "- [ ] " and not ends, "after a ticked item, an open box")
lead, ends = md.continue("- [ ] ")
check(ends, "an empty item ends the list")
lead = md.continue("9. nine")
check(lead == "10. ", "after nine, ten")
lead = md.continue("  * nested")
check(lead == "  * ", "a bullet keeps its indent")
check(md.continue("words") == nil, "a paragraph starts nothing")

-- Every byte after the mark is in exactly one span, in order.
for _, s in ipairs({ "a **b** *c* `d` [e](f) g", "## x", "- [ ] **y** z",
                     "***", "`unclosed", "[not](a link" }) do
  local i = md.line(s, false)
  local at = i.hang + 1

  for _, sp in ipairs(i.spans) do
    if sp[1] ~= at then at = -1 break end
    at = sp[2] + 1
  end

  check(at == #s + 1 or (#i.spans == 0 and i.hang >= #s),
        ("the spans of %q are its bytes, each once"):format(s))
end

if failed > 0 then
  print(("FAIL: %d of %d checks on Markdown as it is written"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on Markdown as it is written (headings, bullets, numbers, "
       .. "checklists, quotations, rules and fences; bold, italic, code and links "
       .. "inside a line; what Return starts; every byte in one span)"):format(passed))
