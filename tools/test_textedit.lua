-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Text Edit kit on the host (`user/lib/textedit.lua`, `docs/office.md`
-- O1): keys typed, deleted, formatted and undone over a `richtext` body set
-- by `pageset`, against the half-an-em measure `test_pageset.lua` uses - so
-- what each key does to the body can be said here exactly, without a
-- window.
--
--   build/host/lua tools/test_textedit.lua
--
-- And, at the end, **a keystroke in a 100-page document**, timed on this
-- Mac by stage - the edit, and the pages set again - and what each
-- allocates. It is a number for the record and not a check: the Mac is not
-- the machine, and the window's own part of a keystroke - drawing the
-- pages again - is not here (`office.md`, and the main session's QEMU run).

tabletext = dofile("user/init/tabletext.lua")

local loaded = {}

-- The clipboard, kept here: what `wmproto` would hand the window manager.
local clipboard = nil

loaded["wmproto.lua"] = {
  copy = function(text) clipboard = text return true end,
  paste = function() return clipboard end,
}

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

-- A document and an editor over it, the hooks counted.
local function editor(body)
  local doc = writedoc.new()
  if body then doc.body = body end

  local by_name = {}
  for _, s in ipairs(doc.styles) do by_name[s.name] = s end

  local cache = pageset.cache()
  local state = { set = pageset.set(doc, measure, cache, {}), heard = {} }

  local function heard(what)
    state.heard[what] = (state.heard[what] or 0) + 1
  end

  state.doc = doc
  state.ed = textedit.new{
    doc = doc, by_name = by_name, measure = measure,
    set = function() return state.set end,
    body_style = writedoc.BODY,
    keep = function() return { paper = doc.paper } end,
    put_back = function(entry) doc.paper = entry.paper end,
    on = {
      edited = function()
        heard("edited")
        state.set = pageset.set(doc, measure, cache, {})
      end,
      undone = function()
        heard("undone")
        state.set = pageset.set(doc, measure, cache, {})
      end,
      moved = function(redraw) heard(redraw and "moved, drawn" or "moved") end,
      looked = function() heard("looked") end,
      all = function() heard("all") end,
    },
  }

  return state
end

local function typed(ed, text)
  for i = 1, #text do ed:key(text:byte(i)) end
end

local function plain(doc, n) return richtext.plain(doc.body[n]) end

--
-- Typing, and a run of it one step to undo.
--
do
  local st = editor()
  local ed, doc = st.ed, st.doc

  typed(ed, "Hello")
  check(plain(doc, 1) == "Hello", "typed: " .. plain(doc, 1))
  check(ed.caret.para == 1 and ed.caret.at == 6, "the caret after typing: " .. ed.caret.at)
  check(#ed.undo_steps == 1, "a run of typing was " .. #ed.undo_steps .. " steps, not one")
  check(ed.dirty, "typing did not make the document edited")
  check(st.heard.edited == 5, "edited was heard " .. tostring(st.heard.edited) .. " times")

  check(ed:key(13), "Return was not the text's")
  check(#doc.body == 2 and plain(doc, 2) == "", "Return did not make a paragraph")
  check(#ed.undo_steps == 2, "Return was not a step of its own")

  typed(ed, "World")
  check(plain(doc, 2) == "World" and #ed.undo_steps == 3, "the second run")

  ed:undo()
  check(#doc.body == 2 and plain(doc, 2) == "", "undo took back more or less than a run")
  check(st.heard.undone == 1, "undo was not heard")
  ed:undo()
  check(#doc.body == 1 and plain(doc, 1) == "Hello", "the second undo: " .. #doc.body)
  ed:redo()
  check(#doc.body == 2, "redo did not put Return back")
  ed:redo()
  check(plain(doc, 2) == "World", "redo did not put the run back")
  check(ed.caret.para == 2 and ed.caret.at == 6, "the caret after a redo")

  -- Control-Z and Control-Y are the same two.
  ed:key(26)
  check(plain(doc, 2) == "", "Control-Z")
  ed:key(25)
  check(plain(doc, 2) == "World", "Control-Y")

  -- Backspace, and Backspace at a paragraph's start joining two.
  ed:key(8)
  check(plain(doc, 2) == "Worl", "Backspace: " .. plain(doc, 2))
  ed:move({ para = 2, at = 1 })
  ed:key(8)
  check(#doc.body == 1 and plain(doc, 1) == "HelloWorl", "Backspace joining: " .. plain(doc, 1))

  -- Delete, forward.
  ed:move({ para = 1, at = 6 })
  ed:key(keys.DELETE)
  check(plain(doc, 1) == "Helloorl", "Delete: " .. plain(doc, 1))

  -- The arrows; with Shift, a selection.
  ed:move({ para = 1, at = 1 })
  ed:key(keys.RIGHT)
  check(ed.caret.at == 2 and not ed:selected(), "Right")
  ed:key(keys.with(keys.RIGHT, keys.SHIFT))
  check(ed:selected() and ed.anchor.at == 2 and ed.caret.at == 3, "Shift-Right")
  check(st.heard["moved, drawn"] and st.heard["moved, drawn"] >= 1,
        "a selection made was not said to need drawing")
  ed:key(keys.HOME)
  check(ed.caret.at == 1 and not ed:selected(), "Home")
  ed:key(keys.END)
  check(ed.caret.at == #"Helloorl" + 1, "End: " .. ed.caret.at)

  -- A key the text does not take.
  check(not ed:key(keys.with(string.byte("q"), keys.ALT)), "Alt-Q was taken")
end

--
-- Formatting: over a selection, and for what is typed next.
--
do
  local st = editor()
  local ed, doc = st.ed, st.doc

  typed(ed, "bold words")
  ed:move({ para = 1, at = 1 })
  ed:move({ para = 1, at = 5 }, true)
  ed:key(2)                                              -- Control-B
  check(ed:selected(), "a format did not keep the selection")
  check(doc.body[1].runs[1].weight == "Bold" and doc.body[1].runs[1].text == "bold",
        "Control-B over a selection")
  local look = ed:look()
  check(look.weight == "Bold", "the look over a bold selection: " .. tostring(look.weight))

  ed:key(21)                                             -- Control-U
  check(doc.body[1].runs[1].underline == true, "Control-U over a selection")

  -- Nothing selected: the look waits for what is typed.
  ed:move({ para = 1, at = #"bold words" + 1 })
  ed:format({ italic = true })
  check(st.heard.looked == 1, "a look for typing was not heard")
  check(ed:look().italic == true, "the pending look was not shown")
  typed(ed, "!")
  local last = doc.body[1].runs[#doc.body[1].runs]
  check(last.text == "!" and last.italic == true, "the pending look was not typed in")

  -- A paragraph's fields, and a style.
  ed:arrange({ align = "center" })
  check(doc.body[1].align == "center", "arrange")
  ed:restyle("Title")
  check(doc.body[1].style == "Title", "restyle: " .. tostring(doc.body[1].style))

  -- Everything, then Backspace.
  ed:key(1)                                              -- Control-A
  check(st.heard.all == 1 and ed:selected(), "Control-A")
  ed:key(8)
  check(#doc.body == 1 and plain(doc, 1) == "", "everything deleted: " .. plain(doc, 1))
end

--
-- Copy, cut and paste, through the clipboard.
--
do
  local st = editor()
  local ed, doc = st.ed, st.doc

  typed(ed, "one two")
  check(not ed:edit("copy"), "copy with nothing selected was taken")

  ed:move({ para = 1, at = 1 })
  ed:move({ para = 1, at = 4 }, true)
  check(ed:edit("copy") and clipboard == "one", "copy: " .. tostring(clipboard))
  check(ed:edit("cut") and plain(doc, 1) == " two", "cut: " .. plain(doc, 1))

  ed:move({ para = 1, at = 5 })
  check(ed:edit("paste") and plain(doc, 1) == " twoone", "paste: " .. plain(doc, 1))
  check(ed:edit("selectall") and ed:selected(), "select all")
  check(not ed:edit("frobnicate"), "an edit nobody knows was taken")
end

--
-- Return on an empty list item ends the list; Shift-Return breaks a line.
--
do
  local st = editor({ { style = writedoc.BODY, list = "bullet", runs = { { text = "item" } } } })
  local ed, doc = st.ed, st.doc

  ed:move({ para = 1, at = 5 })
  ed:key(13)
  check(#doc.body == 2 and doc.body[2].list == "bullet", "Return in a list did not go on with it")
  ed:key(13)
  check(#doc.body == 2 and (doc.body[2].list or "none") == "none",
        "Return on an empty item did not end the list")

  typed(ed, "ab")
  ed:move({ para = 2, at = 2 })
  ed:key(keys.with(13, keys.SHIFT))
  check(#doc.body == 2 and plain(doc, 2):find("\n", 1, true),
        "Shift-Return did not break the line in its paragraph: " .. plain(doc, 2))
end

--
-- A table's cells: Tab and Shift-Tab, a new row after the last cell, and
-- Return to the cell below.
--
do
  local t = richtext.new_table(2, 2, writedoc.BODY, true)
  local st = editor({ t, { style = writedoc.BODY, runs = {} } })
  local ed, doc = st.ed, st.doc

  ed:move({ para = 1, row = 1, col = 1, at = 1 })
  typed(ed, "a")
  check(richtext.plain(doc.body[1].table.rows[1][1]) == "a", "typing in a cell")

  ed:key(9)
  check(ed.caret.row == 1 and ed.caret.col == 2, "Tab to the next cell")
  ed:key(keys.with(9, keys.SHIFT))
  check(ed.caret.row == 1 and ed.caret.col == 1, "Shift-Tab to the cell before")
  ed:key(13)
  check(ed.caret.row == 2 and ed.caret.col == 1, "Return to the cell below")

  ed:move({ para = 1, row = 2, col = 2, at = 1 })
  ed:key(9)
  check(#doc.body[1].table.rows == 3 and ed.caret.row == 3 and ed.caret.col == 1,
        "Tab in the last cell did not make a row")

  -- A cell's start holds against Backspace.
  ed:move({ para = 1, row = 3, col = 1, at = 1 })
  local before = doc.body
  ed:key(8)
  check(doc.body == before, "Backspace at a cell's start changed the body")
end

--
-- What `keep` adds is put back by an undo, and a run of a field's words
-- is one step.
--
do
  local st = editor()
  local ed, doc = st.ed, st.doc
  local a4 = doc.paper

  ed:step("paper", false)
  doc.paper = { name = "Letter", width_mm = 215.9, height_mm = 279.4, landscape = false }
  ed:undo()
  check(doc.paper == a4, "undo did not put the paper back")
  ed:redo()
  check(doc.paper.name == "Letter", "redo did not put the Letter back")

  local n = #ed.undo_steps
  ed:step("header_text", true)
  ed:step("header_text", true)
  ed:step("header_text", true)
  check(#ed.undo_steps == n + 1, "a run of a field's words was " .. (#ed.undo_steps - n) .. " steps")
  ed:step("paper", false)
  ed:step("paper", false)
  check(#ed.undo_steps == n + 3, "two settings changed were not two steps")
end

--
-- A picture's paragraph: typed on, a paragraph is made after it; Backspace
-- on it takes it out.
--
do
  local pic = { style = writedoc.BODY, runs = {},
                picture = { name = "pictures/1.png", width_mm = 50, height_mm = 30 } }
  local st = editor({ { style = writedoc.BODY, runs = { { text = "x" } } }, pic })
  local ed, doc = st.ed, st.doc

  ed:move({ para = 2, at = 1 })
  typed(ed, "y")
  check(#doc.body == 3 and doc.body[2].picture and plain(doc, 3) == "y",
        "typing on a picture did not go after it")

  ed:move({ para = 2, at = 1 })
  ed:key(8)
  check(#doc.body == 2 and not doc.body[2].picture, "Backspace on a picture")

  -- A pointer pressed, and dragged.
  check(ed:press({ para = 1, at = 1 }) == false, "a press with nothing selected said it had")
  ed:drag({ para = 1, at = 2 })
  check(ed:selected(), "a drag did not select")
  check(ed:press({ para = 1, at = 1 }) == true, "a press over a selection did not say so")
end

--
-- **A keystroke in a 100-page document**, timed. One paragraph in the
-- middle typed into, as a person writing would: the edit, and the pages
-- set again from the cache, which sets only the paragraph that changed.
--
do
  local words = {}
  for i = 1, 60 do words[i] = ("word%d"):format(i % 17) end
  local text = table.concat(words, " ")

  local body = {}
  for i = 1, 930 do body[i] = { style = writedoc.BODY, runs = { { text = text } } } end

  local st = editor(body)
  local ed = st.ed
  local pages = #st.set.pages

  check(pages >= 100, "the long document is " .. pages .. " pages, not 100")

  local middle = #body // 2
  ed:move({ para = middle, at = 10 })

  local N = 200
  local set_s = 0

  -- The two stages apart: the hook times the setting, the rest is the edit.
  local cache = pageset.cache()
  st.set = pageset.set(st.doc, measure, cache, {})
  ed = textedit.new{
    doc = st.doc, by_name = (function()
      local b = {} for _, s in ipairs(st.doc.styles) do b[s.name] = s end return b
    end)(),
    measure = measure, set = function() return st.set end, body_style = writedoc.BODY,
    on = {
      edited = function()
        local t0 = os.clock()
        st.set = pageset.set(st.doc, measure, cache, {})
        set_s = set_s + (os.clock() - t0)
      end,
    },
  }
  ed.caret = { para = middle, at = 10 }

  collectgarbage("collect")
  collectgarbage("stop")
  local k0 = collectgarbage("count")
  local t0 = os.clock()

  for i = 1, N do ed:key(string.byte("a") + i % 26) end

  local total = os.clock() - t0
  local bytes = (collectgarbage("count") - k0) * 1024
  collectgarbage("restart")
  local edit_s = total - set_s

  check(richtext.plain(st.doc.body[middle]):len() == #text + N, "the long document's typing")

  print(("  a keystroke in %d pages, on this Mac: %.3f ms - the edit %.3f ms, the pages set "
         .. "again %.3f ms; %.1f KB allocated"):format(pages, total / N * 1000,
         edit_s / N * 1000, set_s / N * 1000, bytes / N / 1024))
end

print(("textedit: %d checks, %d failed"):format(checks, fails))
os.exit(fails == 0 and 0 or 1)
