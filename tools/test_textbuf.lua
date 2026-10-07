-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The text an editor edits, on this machine: every edit and its undo, how
-- typing gathers into steps, the moves with and without Shift, and the
-- line-wise commands - indent, outdent, comment - each one step.
--
--   build/host/lua tools/test_textbuf.lua

local textbuf = assert(loadfile(arg and arg[1] or "user/lib/textbuf.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function is(b, want, what)
  local got = b:content()

  check(got == want, ("%s: %q, not %q"):format(what, got, want))
end

local function at(b, y, x, what)
  check(b.cy == y and b.cx == x,
        ("%s: the caret is at %d,%d, not %d,%d"):format(what, b.cy, b.cx, y, x))
end

-- Typing a string a character at a time, as keys do.
local function type_in(b, text)
  for ch in text:gmatch(".") do
    if ch == "\n" then b:newline(true) else b:insert(ch, "type") end
  end
end

--
-- The text in and out, and the file's last newline ending its last line.
--
do
  local b = textbuf.new("one\ntwo\n")

  check(#b.lines == 2, "a file ending in a newline has two lines, not "
        .. #b.lines)
  is(b, "one\ntwo\n", "a file comes back as it went in")
  check(not b.dirty, "a file just opened is changed")

  b = textbuf.new("")
  check(#b.lines == 1 and b.lines[1] == "", "an empty file is one empty line")
end

--
-- Typing, and undo taking it back a word at a time.
--
do
  local b = textbuf.new("")

  type_in(b, "local x = 1")
  is(b, "local x = 1\n", "typed")
  at(b, 1, 12, "after typing")
  check(b.dirty, "typing did not make the text changed")

  check(b:undo(), "nothing to undo after typing")
  is(b, "local x = \n", "undo took back the last word")
  b:undo()
  is(b, "local x \n", "undo took back the = with its space")
  b:undo() b:undo()
  is(b, "\n", "undo took the rest back")
  check(not b.dirty, "undone to the start, and still changed")
  check(not b:undo(), "undid more than was done")

  check(b:redo() and b:redo(), "no redo after undo")
  is(b, "local x \n", "redo put two words back")
  b:redo() b:redo()
  is(b, "local x = 1\n", "redo put it all back")
  at(b, 1, 12, "after redo")
  check(not b:redo(), "redid more than was undone")

  -- A new edit ends the redo trail.
  b:undo()
  b:insert("2", "type")
  check(not b:redo(), "redo survived a new edit")
  is(b, "local x = 2\n", "an edit after undo")
end

--
-- Enter keeps the indent, and one Enter is one step.
--
do
  local b = textbuf.new("  if x then")

  b:line_end()
  b:newline(true)
  is(b, "  if x then\n  \n", "Enter did not keep the line's indent")
  at(b, 2, 3, "after Enter")
  b:undo()
  is(b, "  if x then\n", "Enter was not one step")

  b:newline(false)
  is(b, "  if x then\n\n", "Enter without indent added one")
end

--
-- Backspace and Delete, across a line break, and gathered.
--
do
  local b = textbuf.new("ab\ncd")

  b:place(2, 1)
  b:backspace()
  is(b, "abcd\n", "Backspace at a line's start did not join it to the one above")
  at(b, 1, 3, "after joining")
  b:backspace() b:backspace()
  is(b, "cd\n", "two backspaces")
  b:undo()
  is(b, "ab\ncd\n", "a run of Backspace, across the join, was not one step")

  b:place(1, 3)
  b:delete_forward()
  is(b, "abcd\n", "Delete at a line's end did not join the next one")
  b:delete_forward() b:delete_forward()
  is(b, "ab\n", "two deletes")
  b:undo()
  is(b, "ab\ncd\n", "a run of Delete, across the join, was not one step")
  check(not textbuf.new("x"):backspace(), "Backspace at the very start did something")
end

--
-- A selection: made by moving with Shift, replaced by typing, taken by
-- Backspace, dropped by moving without Shift.
--
do
  local b = textbuf.new("hello world")

  b:place(1, 1)
  for _ = 1, 5 do b:right(true) end
  check(b:selected() == "hello", "Shift+Right five times did not select hello: "
        .. tostring(b:selected()))

  b:insert("HELLO")
  is(b, "HELLO world\n", "typing did not replace the selection")
  check(b:selected() == nil, "a selection survived being typed over")
  b:undo()
  is(b, "hello world\n", "replacing a selection was not one undo step")
  check(b:selected() == "hello", "undo did not bring the selection back")

  b:right()
  at(b, 1, 6, "Right on a selection goes to its end")
  check(b:selected() == nil, "Right without Shift kept the selection")

  b:place(1, 12)
  b:word_left(true)
  check(b:selected() == "world",
        "Ctrl+Shift+Left did not select a word: " .. tostring(b:selected()))
  b:word_left(true)
  check(b:selected() == "hello world",
        "a second Ctrl+Shift+Left did not reach the word before: "
        .. tostring(b:selected()))
  b:word_right(true)
  check(b:selected() == " world",
        "Ctrl+Shift+Right did not go to the end of hello: " .. tostring(b:selected()))

  b:backspace()
  is(b, "hello\n", "Backspace took other than the selection")

  -- Across lines, and the whole text.
  b = textbuf.new("one\ntwo\nthree")
  b:select_all()
  check(b:selected() == "one\ntwo\nthree", "select all")
  b:delete_selected()
  is(b, "\n", "deleting everything")
  b:undo()
  is(b, "one\ntwo\nthree\n", "undo of deleting everything")

  b:place(1, 2)
  b:down(true) b:down(true)
  check(b:selected() == "ne\ntwo\nt", "Shift+Down twice: " .. tostring(b:selected()))
end

--
-- Up and down keep the column they started from.
--
do
  local b = textbuf.new("a long line\nab\nanother long line")

  b:place(1, 8)
  b:down()
  at(b, 2, 3, "down onto a short line")
  b:down()
  at(b, 3, 8, "down again, back to the column it started in")
  b:up() b:up()
  at(b, 1, 8, "and up")
end

--
-- Home, the first thing on the line, then its start.
--
do
  local b = textbuf.new("    return x")

  b:line_end()
  b:home()
  at(b, 1, 5, "Home went to the first thing on the line")
  b:home()
  at(b, 1, 1, "Home again went to the line's start")
  b:home()
  at(b, 1, 5, "and back")
  b:text_end()
  at(b, 1, 13, "Ctrl+End")
end

--
-- Words: Ctrl+Left and Ctrl+Right stop at a word and at punctuation.
--
do
  local b = textbuf.new("local s = ui.slider{ x = 1 }")

  b:place(1, 1)
  b:word_right() at(b, 1, 6, "past local")
  b:word_right() at(b, 1, 8, "past s")
  b:word_right() at(b, 1, 10, "past =")
  b:word_right() at(b, 1, 13, "past ui")
  b:word_right() at(b, 1, 14, "past the dot")
  b:word_left()  at(b, 1, 13, "back before the dot")
  b:word_left()  at(b, 1, 11, "back to ui")
end

--
-- Indent and outdent, of a selection's lines, each one step, and the same
-- lines still selected so a second press does it again.
--
do
  local b = textbuf.new("a\n  b\nc\n")

  b:place(1, 1)
  b:place(3, 1, true)     -- lines 1 and 2, whole, ending at the start of 3
  b:indent()
  is(b, "  a\n    b\nc\n", "indent took a line the selection only reached the start of, "
     .. "or missed one")
  b:indent()
  is(b, "    a\n      b\nc\n", "a second indent")
  b:undo()
  is(b, "  a\n    b\nc\n", "an indent of two lines was not one step")
  b:outdent() b:outdent()
  is(b, "a\nb\nc\n", "outdent")
  b:outdent()
  is(b, "a\nb\nc\n", "outdent of nothing changed something")

  -- Tab with no selection: spaces to the next stop.
  b = textbuf.new("x")
  b:place(1, 2)
  b:tab()
  is(b, "x \n", "Tab after one character went to column 3")
  b:tab()
  is(b, "x   \n", "a second Tab went to column 5")

  -- Tab with lines selected indents them.
  b = textbuf.new("a\nb\n")
  b:select_all()
  b:tab()
  is(b, "  a\n  b\n", "Tab on selected lines did not indent them")
end

--
-- Comments on and off, at the least indent, and one step.
--
do
  local b = textbuf.new("if x then\n  y()\n\nend")

  b:select_all()
  b:toggle_comment()
  is(b, "-- if x then\n--   y()\n\n-- end\n",
     "commenting did not put -- at the least indent, and skip the blank line")
  b:toggle_comment()
  is(b, "if x then\n  y()\n\nend\n", "uncommenting did not take them off")
  b:undo()
  is(b, "-- if x then\n--   y()\n\n-- end\n", "uncommenting was not one step")

  -- One line, where the caret is, indented.
  b = textbuf.new("  print(1)")
  b:place(1, 5)
  b:toggle_comment()
  is(b, "  -- print(1)\n", "one line commented at its indent")
  at(b, 1, 8, "the caret moved with its text")

  -- A mix: some commented, some not - all get one.
  b = textbuf.new("-- a\nb")
  b:select_all()
  b:toggle_comment()
  is(b, "-- -- a\n-- b\n", "a mix of commented and not was uncommented")
end

--
-- Saved, and undone back to the saved text: not changed.
--
do
  local b = textbuf.new("x")

  b:line_end()
  type_in(b, " = 1")
  b:saved()
  check(not b.dirty, "saved and still changed")
  b:insert("!", "type")
  check(b.dirty, "an edit after saving is not a change")
  b:undo()
  check(not b.dirty, "undone back to the saved text and still changed")
  b:undo()
  check(b.dirty, "undone past the saved text and not changed")
end

--
-- Appended: an Output panel's text, put on the end and never undone.
--
do
  local b = textbuf.new("")

  b:append("one")
  b:append(" two\nthree\n")
  b:append("four")
  is(b, "one two\nthree\nfour\n", "appending did not put the text on the end")
  check(#b.undos == 0 and not b.dirty, "appending made something to undo")
  check(not b:undo(), "an append was undone")
end

--
-- `changed_from`: the first line an edit touched, which is where the
-- colourer starts again.
--
do
  local b = textbuf.new("a\nb\nc\nd")

  b.changed_from = math.huge
  b:place(3, 1)
  b:insert("x", "type")
  check(b.changed_from == 3, "an edit on line 3 said line " .. b.changed_from)
  b.changed_from = math.huge
  b:undo()
  check(b.changed_from == 3, "an undo on line 3 said line " .. b.changed_from)
end

-- Finding and replacing (7 October): forwards and back round the ends, case
-- or not, every match counted, and Replace All one step to take back.
do
  local b = textbuf.new("Plasma plasma\nno match\nPLASMA here")

  local y, x1, x2 = b:find("plasma", 1, 1)
  check(y == 1 and x1 == 1 and x2 == 7, "the first match from the start")
  y, x1 = b:find("plasma", 1, 2)
  check(y == 1 and x1 == 8, "the next one along the line")
  y, x1 = b:find("plasma", 1, 9)
  check(y == 3 and x1 == 1, "the next line that has one, any case")
  y, x1 = b:find("plasma", 3, 2)
  check(y == 1 and x1 == 1, "round the end to the start")
  y, x1 = b:find("plasma", 1, 1, { back = true })
  check(y == 3 and x1 == 1, "backwards, round the start to the end")
  y, x1 = b:find("plasma", 1, 1, { case = true })
  check(y == 1 and x1 == 8, "with case, only the one written so")
  check(b:find("nothing", 1, 1) == nil, "a word that is not there")
  check(#b:matches("plasma") == 3 and #b:matches("plasma", { case = true }) == 1,
        "every match counted, and with case")

  b:select(1, 8, 1, 14)
  check(b:selected() == "plasma", "a match selected: " .. tostring(b:selected()))

  check(b:replace_all("plasma", "fire") == 3, "Replace All says how many")
  check(b.lines[1] == "fire fire" and b.lines[3] == "fire here",
        "every match replaced: " .. b.lines[1] .. " / " .. b.lines[3])
  b:undo()
  check(b.lines[1] == "Plasma plasma" and b.lines[3] == "PLASMA here",
        "and one undo takes all of them back")
end

if failed > 0 then
  print(("FAIL: %d of %d checks on the text an editor edits"):format(failed,
        passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the text an editor edits (typing and undo a word "
       .. "at a time, redo, Enter keeping the indent, Backspace and Delete "
       .. "gathered, selections by Shift and by words, Home, indent, outdent "
       .. "and comments as one step each, saved and changed, found and replaced)"):format(passed))
