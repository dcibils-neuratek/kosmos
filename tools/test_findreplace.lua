-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Find and replace, and the editing keys R1 brought, on the host
-- (`user/lib/textedit.lua`, `docs/office.md` O2, `docs/write.md` R1): what
-- `ed:find` finds in paragraphs and cells, with case and whole words and in
-- a selection; one match replaced and all of them as one step to undo; a
-- word at a time with Control and the arrows, and deleted with Control and
-- Backspace or Delete; the second press the word and the third the
-- paragraph; the words counted; Control-Shift-I italic. And the kept copy's
-- name and note (`user/lib/docfile.lua`, O4), which are pure.
--
--   build/host/lua tools/test_findreplace.lua

tabletext = dofile("user/init/tabletext.lua")

local loaded = {}

loaded["wmproto.lua"] = { copy = function() return true end, paste = function() return nil end }

-- `docfile` reads and writes through `zip`, whose deflate is a kit in C;
-- the parts tested here are pure and touch neither.
loaded["zip.lua"] = {}

function use(path)
  local name = path:match("^/Kosmos/Libraries/(.+)$")
  assert(name, "no such library here: " .. path)

  if not loaded[name] then loaded[name] = dofile("user/lib/" .. name) end

  return loaded[name]
end

local richtext = use("/Kosmos/Libraries/richtext.lua")
local writedoc = use("/Kosmos/Libraries/writedoc.lua")
local pageset  = use("/Kosmos/Libraries/pageset.lua")
local keys     = use("/Kosmos/Libraries/keys.lua")
local textedit = use("/Kosmos/Libraries/textedit.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then
    checks = checks + 1
  else
    fails = fails + 1
    print("  " .. what)
  end
end

local measure = {
  width = function(look, text) return utf8.len(text) * 0.5 * look.size_pt end,
  line = function(look) return 0.8 * look.size_pt, 0.2 * look.size_pt, 0 end,
}

-- A document of these paragraphs, and an editor over it.
local function editor(lines)
  local doc = writedoc.new()
  local by_name = {}
  for _, s in ipairs(doc.styles) do by_name[s.name] = s end

  doc.body = {}
  for i, t in ipairs(lines) do
    doc.body[i] = richtext.paragraph({ runs = { { text = t } } }, by_name, writedoc.BODY)
  end

  local cache = pageset.cache()
  local state = { set = pageset.set(doc, measure, cache, {}), edits = 0 }

  state.doc = doc
  state.ed = textedit.new{
    doc = doc, by_name = by_name, measure = measure,
    set = function() return state.set end,
    body_style = writedoc.BODY,
    keep = function() return { paper = doc.paper } end,
    put_back = function(entry) doc.paper = entry.paper end,
    on = {
      edited = function()
        state.edits = state.edits + 1
        state.set = pageset.set(doc, measure, cache, {})
      end,
      undone = function() state.set = pageset.set(doc, measure, cache, {}) end,
    },
  }

  return state
end

local function plain(doc, n) return richtext.plain(doc.body[n]) end

local function at(m) return ("%d:%d"):format(m.para, m.at) end

--
-- Finding: every match in order, case folded or not, whole words, within.
--
do
  local st = editor{ "The river and the River", "riverbank, a river." }
  local ed = st.ed

  local found = ed:find("river")
  check(#found == 4, "river was found " .. #found .. " times, not 4")
  check(at(found[1].from) == "1:5" and at(found[1].to) == "1:10",
        "the first match: " .. at(found[1].from) .. "-" .. at(found[1].to))
  check(at(found[3].from) == "2:1", "the third match: " .. at(found[3].from))

  check(#ed:find("river", { case = true }) == 3, "with case, River is not river")
  check(#ed:find("river", { words = true }) == 3, "whole words: riverbank is not river")
  check(#ed:find("") == 0 and #ed:find(nil) == 0, "nothing is looked for when nothing is typed")
  check(#ed:find("sea") == 0, "a word not there was found")

  local within = ed:find("river", { within = { { para = 2, at = 1 }, { para = 2, at = 30 } } })
  check(#within == 2, "in the second paragraph alone: " .. #within)
end

--
-- In a table's cells, as a caret has them.
--
do
  local st = editor{ "Above" }
  local ed, doc = st.ed, st.doc

  local t = richtext.new_table(2, 2, writedoc.BODY)
  t.table.rows[2][1] = richtext.paragraph({ runs = { { text = "the river" } } }, {}, writedoc.BODY)
  doc.body[2] = t

  local found = ed:find("river")
  check(#found == 1, "in a cell: " .. #found)
  check(found[1] and found[1].from.row == 2 and found[1].from.col == 1 and found[1].from.at == 5,
        "the cell's match is not where it is")
end

--
-- Replacing: one, then all, as a single step to undo.
--
do
  local st = editor{ "one cat, two cats", "cat" }
  local ed, doc = st.ed, st.doc

  local found = ed:find("cat", { words = true })
  check(#found == 2, "whole word cat: " .. #found)

  ed:replace(found[1], "dog")
  check(plain(doc, 1) == "one dog, two cats", "one replaced: " .. plain(doc, 1))

  local all = ed:find("cat")
  local n = ed:replace_all(all, "bird")
  check(n == 2, "all replaced said " .. tostring(n))
  check(plain(doc, 1) == "one dog, two birds" and plain(doc, 2) == "bird",
        "all replaced: " .. plain(doc, 1) .. " / " .. plain(doc, 2))

  ed:undo()
  check(plain(doc, 1) == "one dog, two cats" and plain(doc, 2) == "cat",
        "all replaced was not one step to undo: " .. plain(doc, 1))

  -- Replaced with nothing: the matches taken out.
  ed:replace_all(ed:find("cat"), "")
  check(plain(doc, 1) == "one dog, two s" and plain(doc, 2) == "", "replaced with nothing")
end

--
-- A word at a time: Control-Left and Control-Right, Control-Backspace and
-- Control-Delete.
--
do
  local st = editor{ "alpha beta  gamma", "next" }
  local ed, doc = st.ed, st.doc

  ed:move({ para = 1, at = 1 })
  ed:key(keys.with(keys.RIGHT, keys.CTRL))
  check(ed.caret.at == 6, "Control-Right to the end of alpha: " .. ed.caret.at)
  ed:key(keys.with(keys.RIGHT, keys.CTRL))
  check(ed.caret.at == 11, "Control-Right to the end of beta: " .. ed.caret.at)
  ed:key(keys.with(keys.LEFT, keys.CTRL))
  check(ed.caret.at == 7, "Control-Left to the start of beta: " .. ed.caret.at)

  ed:key(keys.with(keys.RIGHT, keys.CTRL | keys.SHIFT))
  check(ed:selected() and ed.anchor.at == 7 and ed.caret.at == 11,
        "Control-Shift-Right selects a word")

  -- From the paragraph's end, one step across, as an arrow goes.
  ed:move({ para = 1, at = #"alpha beta  gamma" + 1 })
  ed:key(keys.with(keys.RIGHT, keys.CTRL))
  check(ed.caret.para == 2 and ed.caret.at == 1, "Control-Right across a paragraph's end")

  -- Control-Backspace: the word before.
  ed:move({ para = 1, at = #"alpha beta  gamma" + 1 })
  ed:key(keys.with(8, keys.CTRL))
  check(plain(doc, 1) == "alpha beta  ", "Control-Backspace: " .. plain(doc, 1))

  -- Control-Delete: the word after.
  ed:move({ para = 1, at = 1 })
  ed:key(keys.with(keys.DELETE, keys.CTRL))
  check(plain(doc, 1) == " beta  ", "Control-Delete: " .. plain(doc, 1))

  -- At a paragraph's start, Control-Backspace is Backspace: the two joined.
  ed:move({ para = 2, at = 1 })
  ed:key(keys.with(8, keys.CTRL))
  check(#doc.body == 1 and plain(doc, 1) == " beta  next", "Control-Backspace joining")
end

--
-- The second press is the word, the third the paragraph.
--
do
  local st = editor{ "the quick fox", "second" }
  local ed = st.ed

  check(not ed:press({ para = 1, at = 7 }, 1), "one press selected something")
  check(ed:press({ para = 1, at = 7 }, 2), "two presses did not say to draw")
  check(ed.anchor.at == 5 and ed.caret.at == 10, "two presses: " .. ed.anchor.at .. "-" .. ed.caret.at)

  -- Just past a word is still that word.
  ed:press({ para = 1, at = 10 }, 2)
  check(ed.anchor.at == 5 and ed.caret.at == 10, "two presses just past quick")

  ed:press({ para = 1, at = 7 }, 3)
  check(ed.anchor.at == 1 and ed.caret.at == 14 and ed.caret.para == 1,
        "three presses: " .. ed.anchor.at .. "-" .. ed.caret.at)
end

--
-- The words counted: all of them, a selection's, and all with `all`.
--
do
  local st = editor{ "Three words here", "  and   two  ", "" }
  local ed = st.ed

  local w, c = ed:count()
  check(w == 5, "words: " .. w)
  check(c == #"Three words here" + #"  and   two  ", "characters: " .. c)

  ed:move({ para = 1, at = 1 })
  ed:move({ para = 1, at = 12 }, true)
  check(select(1, ed:count()) == 2, "the selection's words: " .. select(1, ed:count()))
  check(select(1, ed:count(true)) == 5, "all of them, with a selection")

  -- Counted again after an edit, not from what was kept.
  ed:move({ para = 2, at = 1 })
  for _, b in ipairs{ ("one "):byte(1, -1) } do ed:key(b) end
  check(select(1, ed:count()) == 6, "after typing a word: " .. select(1, ed:count()))
end

--
-- Control-Shift-I is italic: Control-I is Tab's byte.
--
do
  local st = editor{ "lean" }
  local ed, doc = st.ed, st.doc

  ed:move({ para = 1, at = 1 })
  ed:move({ para = 1, at = 5 }, true)
  check(ed:key(keys.with(string.byte("i"), keys.CTRL | keys.SHIFT)), "Control-Shift-I was not taken")
  check(doc.body[1].runs[1].italic == true, "Control-Shift-I did not make it italic")
  ed:key(keys.with(string.byte("I"), keys.CTRL | keys.SHIFT))
  check(not doc.body[1].runs[1].italic, "Control-Shift-I again did not take italic off")
end

--
-- The kept copy's name and note (`docfile`, O4).
--
do
  local docfile = use("/Kosmos/Libraries/docfile.lua")

  check(docfile.kept_name(nil) == "Untitled", "a document never saved")
  check(docfile.kept_name("/Home/Letters/To Ana.write") == "_Home_Letters_To_Ana.write",
        "a path's name: " .. docfile.kept_name("/Home/Letters/To Ana.write"))

  local note = docfile.kept_note(tabletext.encode{ path = "/Home/a.write", name = "a.write",
                                                   epoch = 1800000000, boot = 1799990000 })
  check(note and note.path == "/Home/a.write" and note.epoch == 1800000000
        and note.boot == 1799990000, "a note read back")

  local never = docfile.kept_note(tabletext.encode{ path = false, name = "Untitled",
                                                    epoch = 5, boot = 1 })
  check(never and never.path == nil and never.name == "Untitled", "a never-saved note")
  check(docfile.kept_note("not a note") == nil and docfile.kept_note(nil) == nil,
        "what is not a note")
end

print(("findreplace: %d checks, %d failed"):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
